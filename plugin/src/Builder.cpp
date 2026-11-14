// Auto-build through the workshop engine (Starfield 1.16.244, see docs/re-workshop.md).
//
// Per object: look up its build-menu entry (21A6B60, keyed by base form ID), post NewItemEvent, let the
// engine create and load the preview, force the preview's position and yaw every frame, then post CommitEvent.
// While a build runs, the cost/limit pre-check passes and a blocked (red) placement counts as valid.

#include "Builder.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numbers>
#include <thread>

namespace OSB::Builder
{
	namespace
	{
		// Address Library 1.16.244
		constexpr REL::ID kEngine{ 928921 };            // +0x5F5D2E0 WorkshopEngine state (static object)
		constexpr REL::ID kMenuTable{ 830847 };         // +0x591EE40 build menu entries
		constexpr REL::ID kMenuCtx{ 830848 };           // +0x591EE48 pointer; ->+0x28 passed to NewItem
		constexpr REL::ID kLookupEntry{ 121159 };       // +0x21A6B60 (table*, baseFormID) -> entry*
		constexpr REL::ID kPostNewItem{ 121372 };       // +0x21BAB50 (entry*, int)
		constexpr REL::ID kPostCommit{ 121377 };        // +0x21BB3D0 ()
		constexpr REL::ID kPostCancel{ 121378 };        // +0x21BB4A0 ()
		constexpr REL::ID kUpdatePlacement{ 121599 };   // +0x21D74B0 per-frame placement update
		constexpr std::ptrdiff_t kAimCallOff = 0x43B;       // call 21CE760 (compute preview transform)
		constexpr std::ptrdiff_t kPrecheckCallOff = 0x32D;  // call 21D3DD0 (cost/limit status, 0 = ok)
		constexpr REL::ID kStateUpdatePlacement{ 121280 };  // +0x21B41F0 (StateUpdatePlacement vfunc 0x130)
		constexpr std::ptrdiff_t kUpdateCallOff = 0x57;     // call 21D74B0

		// engine fields
		constexpr std::ptrdiff_t kItem = 0x268;      // shared_ptr<WorkshopItem> current preview
		constexpr std::ptrdiff_t kYaw = 0x184;       // float radians
		constexpr std::ptrdiff_t kPreviewPos = 0x138;  // float x, y, z
		constexpr std::ptrdiff_t kStatus = 0x193;    // byte: 2 ok, 4 blocked, 0x18 can't afford ...
		constexpr std::uint8_t kStatusOk = 2;

		std::atomic<bool> g_forcing{ false };
		std::mutex g_targetLock;
		float g_tx = 0, g_ty = 0, g_tz = 0, g_tyaw = 0;

		std::atomic<bool> g_running{ false };
		std::atomic<bool> g_stop{ false };
		std::atomic<std::int32_t> g_done{ 0 }, g_built{ 0 }, g_failed{ 0 }, g_total{ 0 };

		std::uintptr_t Engine() { return kEngine.address(); }

		template <class T>
		T& Field(std::ptrdiff_t a_off)
		{
			return *reinterpret_cast<T*>(Engine() + a_off);
		}

		// ---- hooks ----

		using Aim_t = std::uintptr_t (*)(void* a_out, void* a_engine, float a_f);
		Aim_t g_aim = nullptr;

		std::uintptr_t AimHook(void* a_out, void* a_engine, float a_f)
		{
			const bool forcing = g_forcing.load();
			float x = 0, y = 0, z = 0, yaw = 0;
			if (forcing) {
				std::scoped_lock l(g_targetLock);
				x = g_tx; y = g_ty; z = g_tz; yaw = g_tyaw;
				*reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(a_engine) + kYaw) = yaw;
			}
			const auto r = g_aim(a_out, a_engine, a_f);
			if (forcing) {
				auto* t = reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(a_out) + 0x30);
				t[0] = x;
				t[1] = y;
				t[2] = z;
			}
			return r;
		}

		using Precheck_t = std::uint8_t (*)(void* a_engine, void* a_arg);
		Precheck_t g_precheck = nullptr;

		std::uint8_t PrecheckHook(void* a_engine, void* a_arg)
		{
			const auto r = g_precheck(a_engine, a_arg);
			return g_forcing.load() ? 0 : r;
		}

		using Update_t = std::uintptr_t (*)(void* a_engine, void* a_2, void* a_3, void* a_4);
		Update_t g_update = nullptr;

		std::uintptr_t UpdateHook(void* a_engine, void* a_2, void* a_3, void* a_4)
		{
			const auto r = g_update(a_engine, a_2, a_3, a_4);
			if (g_forcing.load()) {
				auto& status = *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(a_engine) + kStatus);
				if (status == 4) {   // blocked by overlap: build anyway
					status = kStatusOk;
				}
			}
			return r;
		}

		template <class F>
		F HookCall(REL::ID a_func, std::ptrdiff_t a_off, F a_hook, const char* a_name)
		{
			const auto src = a_func.address() + a_off;
			if (*reinterpret_cast<const std::uint8_t*>(src) != 0xE8) {
				REX::ERROR("builder: {} is not a call; not hooked", a_name);
				return nullptr;
			}
			const auto prev = REL::GetTrampoline().write_call<5>(src, a_hook);
			REX::INFO("builder: hooked {}", a_name);
			return reinterpret_cast<F>(prev);
		}

		// ---- engine commands ----

		void* LookupEntry(std::uint32_t a_formID)
		{
			using func_t = void* (*)(void*, std::uint32_t);
			return REL::Relocation<func_t>{ kLookupEntry }(reinterpret_cast<void*>(kMenuTable.address()), a_formID);
		}

		void PostNewItem(void* a_entry)
		{
			std::int32_t ctx = 0;
			if (auto* p = *reinterpret_cast<std::uintptr_t**>(kMenuCtx.address())) {
				ctx = *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(p) + 0x28);
			}
			using func_t = void (*)(void*, std::int32_t);
			REL::Relocation<func_t>{ kPostNewItem }(a_entry, ctx);
		}

		void PostCommit()
		{
			using func_t = void (*)();
			REL::Relocation<func_t>{ kPostCommit }();
		}

		void PostCancel()
		{
			using func_t = void (*)();
			REL::Relocation<func_t>{ kPostCancel }();
		}

		void* CurrentItem() { return Field<void*>(kItem); }

		bool WaitFor(auto a_cond, int a_ms)
		{
			for (int t = 0; t < a_ms; t += 50) {
				if (a_cond()) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
			return a_cond();
		}

		void Worker(std::vector<Job> a_jobs)
		{
			REX::INFO("builder: start, {} objects", a_jobs.size());
			for (const auto& job : a_jobs) {
				if (g_stop.load()) {
					break;
				}
				auto* entry = LookupEntry(job.formID);
				if (!entry) {
					REX::WARN("builder: {} ({:08X}) is not in the build menu; skipped", job.name, job.formID);
					++g_failed;
					++g_done;
					continue;
				}
				{
					std::scoped_lock l(g_targetLock);
					g_tx = job.x;
					g_ty = job.y;
					g_tz = job.z;
					g_tyaw = job.yawDeg * std::numbers::pi_v<float> / 180.0f;
				}
				void* before = CurrentItem();
				g_forcing = true;
				PostNewItem(entry);
				// wait for a new preview item, then for the preview to reach the target
				const bool got = WaitFor([&] { return CurrentItem() && CurrentItem() != before; }, 5000);
				const bool there = got && WaitFor([&] {
					const auto& p = Field<float[3]>(kPreviewPos);
					return std::abs(p[0] - job.x) < 0.5f && std::abs(p[1] - job.y) < 0.5f;
				}, 5000);
				std::this_thread::sleep_for(std::chrono::milliseconds(400));   // let the 3D load and snapping settle
				const auto& p = Field<float[3]>(kPreviewPos);
				REX::INFO("builder: {} ({:08X}) preview {} at {:.2f} {:.2f} {:.2f} (target {:.2f} {:.2f} {:.2f}) status {}", job.name, job.formID,
					got ? "ready" : "MISSING", p[0], p[1], p[2], job.x, job.y, job.z, Field<std::uint8_t>(kStatus));
				if (!got) {
					++g_failed;
					++g_done;
					continue;
				}
				void* item = CurrentItem();
				PostCommit();
				const bool committed = WaitFor([&] { return CurrentItem() != item; }, 5000);
				if (committed && there) {
					++g_built;
				} else {
					REX::WARN("builder: {} commit {}", job.name, committed ? "done but preview was off target" : "did not happen");
					committed ? ++g_built : ++g_failed;
				}
				++g_done;
			}
			g_forcing = false;
			PostCancel();   // drop the last preview the engine re-offers after a commit
			REX::INFO("builder: done, built {}, failed {}, of {}", g_built.load(), g_failed.load(), g_total.load());
			g_running = false;
		}
	}

	void Install()
	{
		g_aim = HookCall<Aim_t>(kUpdatePlacement, kAimCallOff, &AimHook, "preview aim (21CE760)");
		g_precheck = HookCall<Precheck_t>(kUpdatePlacement, kPrecheckCallOff, &PrecheckHook, "placement pre-check (21D3DD0)");
		g_update = HookCall<Update_t>(kStateUpdatePlacement, kUpdateCallOff, &UpdateHook, "placement update (21D74B0)");
	}

	bool Start(std::vector<Job> a_jobs)
	{
		if (g_running.exchange(true)) {
			return false;
		}
		if (!g_aim || !g_precheck || !g_update) {
			REX::ERROR("builder: hooks missing");
			g_running = false;
			return false;
		}
		g_stop = false;
		g_done = 0;
		g_built = 0;
		g_failed = 0;
		g_total = std::int32_t(a_jobs.size());
		std::thread(Worker, std::move(a_jobs)).detach();
		return true;
	}

	std::vector<std::int32_t> Progress()
	{
		return { g_done.load(), g_built.load(), g_failed.load(), g_total.load(), g_running.load() ? 1 : 0 };
	}

	void Stop()
	{
		g_stop = true;
	}
}
