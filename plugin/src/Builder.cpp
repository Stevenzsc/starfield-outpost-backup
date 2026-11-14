// Auto-build through the workshop engine (Starfield 1.16.244, see docs/re-workshop.md).
//
// Per object: look up its build-menu entry (21A6B60, keyed by base form ID), post NewItemEvent, let the
// engine create and load the preview, force the preview's position and yaw every frame, then post CommitEvent.
// While a build runs, the cost/limit pre-check passes and a blocked (red) placement counts as valid.

#include "Builder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numbers>
#include <thread>

extern "C" __declspec(dllimport) int __stdcall VirtualProtect(void*, std::size_t, unsigned long, unsigned long*);

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
		std::atomic<bool> g_forceAll{ false };
		std::atomic<bool> g_logAim{ false };
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
				if (g_logAim.exchange(false)) {
					const auto* m = reinterpret_cast<const float*>(a_out);
					REX::INFO("builder: aim from game {:.2f} {:.2f} {:.2f}  rows {:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f}", t[0], t[1], t[2],
						m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
				}
				t[0] = x;
				t[1] = y;
				t[2] = z;
				// rotation rows (stride 16): [[c,-s,0],[s,c,0],[0,0,1]] with s,c of the ref's angle z
				auto* m = reinterpret_cast<float*>(a_out);
				const float c = std::cos(yaw), s = std::sin(yaw);
				m[0] = c;  m[1] = -s; m[2] = 0;
				m[4] = s;  m[5] = c;  m[6] = 0;
				m[8] = 0;  m[9] = 0;  m[10] = 1;
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
				if (status == 4 || (g_forceAll.load() && status != kStatusOk)) {   // overlap (or anything, last pass): build anyway
					status = kStatusOk;
				}
			}
			return r;
		}

		// Commit handlers (state vfunc 0xB8, engine in r8) refuse unless engine+0x193 == 2: force it there.
		using OnCommit_t = std::uintptr_t (*)(void*, void*, void* a_engine, void*);
		OnCommit_t g_onCommit[2]{};

		template <std::size_t I>
		std::uintptr_t OnCommitHook(void* a_state, void* a_event, void* a_engine, void* a_4)
		{
			if (g_forcing.load() && a_engine) {
				auto& status = *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(a_engine) + kStatus);
				// early passes: only cost (0x18) and overlap (4); last pass: everything
				if (status != kStatusOk && (g_forceAll.load() || status == 0x18 || status == 4)) {
					REX::INFO("builder: commit with status {} forced", status);
					status = kStatusOk;
				}
			}
			return g_onCommit[I](a_state, a_event, a_engine, a_4);
		}

		template <std::size_t I>
		void HookSlot(REL::ID a_vtbl, std::size_t a_slot, const char* a_name)
		{
			auto* slot = reinterpret_cast<std::uintptr_t*>(a_vtbl.address()) + a_slot;
			unsigned long old = 0;
			if (!VirtualProtect(slot, sizeof(*slot), 0x04, &old)) {
				REX::ERROR("builder: VirtualProtect failed for {}", a_name);
				return;
			}
			g_onCommit[I] = reinterpret_cast<OnCommit_t>(*slot);
			*slot = reinterpret_cast<std::uintptr_t>(&OnCommitHook<I>);
			VirtualProtect(slot, sizeof(*slot), old, &old);
			REX::INFO("builder: hooked {}", a_name);
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

		// The lookup returns the menu entry and leaves the matched variant form in the slot after the table
		// (kMenuCtx); NewItem's int is the variant's form ID. We pass the wanted form ID directly.
		void PostNewItem(void* a_entry, std::uint32_t a_formID)
		{
			using func_t = void (*)(void*, std::int32_t);
			REL::Relocation<func_t>{ kPostNewItem }(a_entry, static_cast<std::int32_t>(a_formID));
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

		// One object: preview at the target, commit. True if the engine committed it.
		bool BuildOne(const Job& a_job)
		{
			// look up right before building: the lookup also sets the menu's selected variant
			void* a_entry = LookupEntry(a_job.formID);
			if (!a_entry) {
				REX::WARN("builder: {} ({:08X}) is not in the build menu", a_job.name, a_job.formID);
				return false;
			}
			{
				std::scoped_lock l(g_targetLock);
				g_tx = a_job.x;
				g_ty = a_job.y;
				g_tz = a_job.z;
				g_tyaw = a_job.yawDeg * std::numbers::pi_v<float> / 180.0f;
			}
			void* before = CurrentItem();
			g_logAim = true;
			PostNewItem(a_entry, a_job.formID);
			const bool got = WaitFor([&] { return CurrentItem() && CurrentItem() != before; }, 3000);
			if (got) {
				WaitFor([&] {
					const auto& p = Field<float[3]>(kPreviewPos);
					return std::abs(p[0] - a_job.x) < 0.5f && std::abs(p[1] - a_job.y) < 0.5f;
				}, 2000);
				std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 3D load and snapping settle
			}
			const auto& p = Field<float[3]>(kPreviewPos);
			const auto status = Field<std::uint8_t>(kStatus);
			if (!got) {
				REX::WARN("builder: {} ({:08X}) no preview", a_job.name, a_job.formID);
				return false;
			}
			void* item = CurrentItem();
			PostCommit();
			const bool committed = WaitFor([&] { return CurrentItem() != item; }, 1500);
			REX::INFO("builder: {} ({:08X}, size {:.0f}) at {:.2f} {:.2f} {:.2f} (target {:.2f} {:.2f} {:.2f}) status {} -> {}", a_job.name, a_job.formID, a_job.size,
				p[0], p[1], p[2], a_job.x, a_job.y, a_job.z, status, committed ? "built" : "not built");
			return committed;
		}

		// Passes: objects that the engine refuses (e.g. furniture before its hab exists) are retried after the
		// others; the last pass forces every placement status to OK.
		constexpr int kPasses = 3;

		void Worker(std::vector<Job> a_jobs)
		{
			REX::INFO("builder: start, {} objects", a_jobs.size());
			// big things first (habs, structures), so furniture has a floor to land on
			std::stable_sort(a_jobs.begin(), a_jobs.end(), [](const Job& a, const Job& b) { return a.size > b.size; });
			std::vector<const Job*> todo;
			for (const auto& job : a_jobs) {
				todo.push_back(&job);
			}
			g_forcing = true;
			for (int pass = 1; pass <= kPasses && !todo.empty() && !g_stop.load(); ++pass) {
				g_forceAll = pass == kPasses;
				REX::INFO("builder: pass {} ({} objects{})", pass, todo.size(), g_forceAll.load() ? ", forcing all checks" : "");
				std::vector<const Job*> retry;
				for (const auto* job : todo) {
					if (g_stop.load()) {
						break;
					}
					if (BuildOne(*job)) {
						++g_built;
						++g_done;
					} else {
						retry.push_back(job);
					}
				}
				todo = std::move(retry);
			}
			for (const auto* job : todo) {
				REX::WARN("builder: {} ({:08X}) could not be built", job->name, job->formID);
				++g_failed;
				++g_done;
			}
			g_forceAll = false;
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
		HookSlot<0>(REL::ID(466620), 0xB8 / 8, "StateUpdatePlacement commit (21B2E20)");
		HookSlot<1>(REL::ID(466892), 0xB8 / 8, "StateUpdateAutoFoundation commit (21B2360)");
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
