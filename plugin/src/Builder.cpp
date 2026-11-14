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
		constexpr REL::ID kStateUpdateFoundation{ 121270 };  // +0x21B2470 (StateUpdateAutoFoundation vfunc 0x130)
		constexpr std::ptrdiff_t kFoundationCallOff = 0x52;  // call 21D5CE0 (per-frame foundation height)
		constexpr REL::ID kRotation{ 883651 };               // +0x5E757C8 float: build-mode rotation (radians)

		// engine fields
		constexpr std::ptrdiff_t kItem = 0x268;      // shared_ptr<WorkshopItem> current preview
		constexpr std::ptrdiff_t kYaw = 0x184;       // float radians
		constexpr std::ptrdiff_t kPreviewPos = 0x138;  // float x, y, z
		constexpr std::ptrdiff_t kStatus = 0x193;    // byte: 2 ok, 4 blocked, 0x18 can't afford ...
		constexpr std::uint8_t kStatusOk = 2;

		std::atomic<bool> g_forcing{ false };
		std::atomic<bool> g_forceAll{ false };
		std::atomic<bool> g_logAim{ false };
		std::atomic<bool> g_logFoundation{ false };
		std::atomic<bool> g_lockZ{ false };           // last pass, non-hab objects: hold the saved height
		std::atomic<std::uint32_t> g_placed{ 0 };    // Workshop::ItemPlacedEvent count
		constexpr float kSmallObject = 20.0f;        // m^2 footprint: furniture and decorations

		class PlacedSink : public RE::BSTEventSink<RE::Workshop::ItemPlacedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemPlacedEvent&, RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>*) override
			{
				++g_placed;
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		std::mutex g_targetLock;
		float g_tx = 0, g_ty = 0, g_tz = 0, g_tyaw = 0;

		std::atomic<bool> g_running{ false };
		std::atomic<bool> g_stop{ false };
		std::atomic<bool> g_aborted{ false };
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
				// last pass: keep the saved height instead of the engine's drop to the surface below
				// (preview z at +0x140, then re-apply the preview transform: item vfunc 0xE0, as 21D74B0 does)
				if (g_lockZ.load()) {
					const auto e = reinterpret_cast<std::uintptr_t>(a_engine);
					{
						std::scoped_lock l(g_targetLock);
						*reinterpret_cast<float*>(e + kPreviewPos + 8) = g_tz;
					}
					if (auto* item = *reinterpret_cast<void**>(e + kItem)) {
						using apply_t = void (*)(void*, void*);
						(*reinterpret_cast<apply_t**>(item))[0xE0 / 8](item, a_engine);
					}
				}
			}
			return r;
		}

		// Snap search (21D2E20, called from the placement update): finds a connection point on a nearby hab,
		// sets +0x188 and moves the preview (+0x138) there; the update then posts the switch to the snapped
		// state. It picks the connection nearest the aim, which is not always the saved one: then drop the
		// snap and keep the saved spot (the hab is then placed like a free-standing one).
		using SnapSearch_t = std::uintptr_t (*)(void* a_engine);
		SnapSearch_t g_snapSearch = nullptr;
		std::atomic<bool> g_logSnap{ false };

		std::uintptr_t SnapSearchHook(void* a_engine)
		{
			const auto r = g_snapSearch(a_engine);
			if (!g_forcing.load()) {
				return r;
			}
			const auto e = reinterpret_cast<std::uintptr_t>(a_engine);
			auto& snapped = *reinterpret_cast<std::uint8_t*>(e + 0x188);
			if (!snapped) {
				return r;
			}
			auto* p = reinterpret_cast<float*>(e + kPreviewPos);
			float tx, ty, tz;
			{
				std::scoped_lock l(g_targetLock);
				tx = g_tx; ty = g_ty; tz = g_tz;
			}
			const bool match = std::abs(p[0] - tx) < 1.0f && std::abs(p[1] - ty) < 1.0f && std::abs(p[2] - tz) < 1.0f;
			if (g_logSnap.exchange(false)) {
				REX::INFO("builder: snap found at {:.2f} {:.2f} {:.2f}, saved {:.2f} {:.2f} {:.2f}: {}", p[0], p[1], p[2], tx, ty, tz,
					match ? "using it" : "ignoring it");
			}
			if (!match) {
				snapped = 0;
				*reinterpret_cast<std::uint8_t*>(e + 0x18A) = 0;
				p[0] = tx;
				p[1] = ty;
				p[2] = tz;
			}
			return r;
		}

		// Auto-foundation state (habs after the first commit): the player sets the height with the camera pitch,
		// then commits again. 21D5CE0 uses the fixed height at engine+0x19C when engine+0x198 is set (else the
		// pitch), clamps it to [ground +0x1A0, max +0x1A4] and writes the preview z (+0x140).
		using Foundation_t = std::uintptr_t (*)(void* a_engine, std::uintptr_t a_2);
		Foundation_t g_foundation = nullptr;

		std::uintptr_t FoundationHook(void* a_engine, std::uintptr_t a_2)
		{
			if (!g_forcing.load()) {
				return g_foundation(a_engine, a_2);
			}
			const auto e = reinterpret_cast<std::uintptr_t>(a_engine);
			auto& fixed = *reinterpret_cast<std::uint8_t*>(e + 0x198);
			auto& height = *reinterpret_cast<float*>(e + 0x19C);
			// this state also takes the yaw (+0x184) from the build-mode rotation global, not from the item
			auto& rotation = *reinterpret_cast<float*>(kRotation.address());
			const auto oldFixed = fixed;
			const auto oldHeight = height;
			fixed = 1;
			{
				std::scoped_lock l(g_targetLock);
				height = g_tz;
				rotation = g_tyaw;
			}
			const auto r = g_foundation(a_engine, a_2);
			if (g_logFoundation.exchange(false)) {
				REX::INFO("builder: foundation height {:.2f} -> preview z {:.2f} (allowed {:.2f}..{:.2f}) yaw {:.3f}", height,
					*reinterpret_cast<float*>(e + 0x140), *reinterpret_cast<float*>(e + 0x1A0), *reinterpret_cast<float*>(e + 0x1A4), *reinterpret_cast<float*>(e + kYaw));
			}
			fixed = oldFixed;
			height = oldHeight;
			return r;
		}

		// Commit handlers (state vfunc 0xB8, engine in r8) refuse unless engine+0x193 == 2: force it there.
		using OnCommit_t = std::uintptr_t (*)(void*, void*, void* a_engine, void*);
		OnCommit_t g_onCommit[3]{};

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
				if constexpr (I == 2) {
					const auto e = reinterpret_cast<std::uintptr_t>(a_engine);
					const auto* p = reinterpret_cast<const float*>(e + kPreviewPos);
					REX::INFO("builder: commit in snapped state: snap {} substate {} at {:.2f} {:.2f} {:.2f}",
						*reinterpret_cast<std::uint8_t*>(e + 0x188), *reinterpret_cast<std::uint8_t*>(e + 0x630), p[0], p[1], p[2]);
					float tx, ty, tz;
					{
						std::scoped_lock l(g_targetLock);
						tx = g_tx; ty = g_ty; tz = g_tz;
					}
					// snapped to some other connection point: better not built than built in the wrong place
					if (std::abs(p[0] - tx) > 1.0f || std::abs(p[1] - ty) > 1.0f || std::abs(p[2] - tz) > 1.0f) {
						REX::INFO("builder: snap point is not the saved spot {:.2f} {:.2f} {:.2f}: not committing", tx, ty, tz);
						return 0;
					}
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

		// ---- player input lock ----
		// Every engine command runs through slot 1 of its BSService TCmd vtable. While a build runs, commands
		// the builder did not post itself (player keys: build, cancel, rotate, other item, exit...) are dropped.
		enum CmdKind { kCmdNewItem, kCmdCommit, kCmdCancel, kCmdPlayerOnly };
		struct Guard
		{
			REL::ID vtbl;
			const char* name;
			CmdKind kind;
		};
		const Guard kGuards[] = {
			{ REL::ID(466536), "NewItem", kCmdNewItem },        // +0x4D49750
			{ REL::ID(466568), "Commit", kCmdCommit },          // +0x4D49910
			{ REL::ID(466554), "Cancel", kCmdCancel },          // +0x4D498F0
			{ REL::ID(466534), "Exit", kCmdPlayerOnly },        // +0x4D49770
			{ REL::ID(466522), "EditItem", kCmdPlayerOnly },    // +0x4D49830
			{ REL::ID(466520), "BulldozeItem", kCmdPlayerOnly },
			{ REL::ID(466546), "RotateItem", kCmdPlayerOnly },
			{ REL::ID(466560), "CycleBehavior", kCmdPlayerOnly },
			{ REL::ID(466558), "RevertBehavior", kCmdPlayerOnly },
			{ REL::ID(466570), "RemoveItem", kCmdPlayerOnly },
			{ REL::ID(466524), "ActivateItem", kCmdPlayerOnly },
		};
		constexpr std::size_t kGuardCount = std::size(kGuards);
		using Cmd_t = std::uintptr_t (*)(void*, void*, void*, void*);
		Cmd_t g_cmdOrig[kGuardCount]{};
		std::atomic<int> g_allow[3]{};   // commands the builder posted and that may run

		template <std::size_t I>
		std::uintptr_t CmdHook(void* a_1, void* a_2, void* a_3, void* a_4)
		{
			if (g_running.load()) {
				const auto kind = kGuards[I].kind;
				if (kind != kCmdPlayerOnly && g_allow[kind].load() > 0) {
					--g_allow[kind];
				} else {
					// the player's exit or cancel key aborts the build (the script then undoes it)
					const bool abortKey = std::string_view(kGuards[I].name) == "Exit" || std::string_view(kGuards[I].name) == "Cancel";
					if (abortKey && !g_stop.exchange(true)) {
						g_aborted = true;
						REX::INFO("builder: player pressed {}: aborting", kGuards[I].name);
					} else {
						REX::INFO("builder: ignored player command {} during the build", kGuards[I].name);
					}
					return 0;
				}
			}
			return g_cmdOrig[I](a_1, a_2, a_3, a_4);
		}

		template <std::size_t I>
		void HookCmd()
		{
			auto* slot = reinterpret_cast<std::uintptr_t*>(kGuards[I].vtbl.address()) + 1;
			unsigned long old = 0;
			if (!VirtualProtect(slot, sizeof(*slot), 0x04, &old)) {
				REX::ERROR("builder: VirtualProtect failed for TCmd {}", kGuards[I].name);
				return;
			}
			g_cmdOrig[I] = reinterpret_cast<Cmd_t>(*slot);
			*slot = reinterpret_cast<std::uintptr_t>(&CmdHook<I>);
			VirtualProtect(slot, sizeof(*slot), old, &old);
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
			++g_allow[kCmdNewItem];
			using func_t = void (*)(void*, std::int32_t);
			REL::Relocation<func_t>{ kPostNewItem }(a_entry, static_cast<std::int32_t>(a_formID));
		}

		void PostCommit()
		{
			++g_allow[kCmdCommit];
			using func_t = void (*)();
			REL::Relocation<func_t>{ kPostCommit }();
		}

		void PostCancel()
		{
			++g_allow[kCmdCancel];
			using func_t = void (*)();
			REL::Relocation<func_t>{ kPostCancel }();
		}

		void* CurrentItem() { return Field<void*>(kItem); }

		bool WaitFor(auto a_cond, int a_ms)
		{
			for (int t = 0; t < a_ms; t += 20) {
				if (a_cond()) {
					return true;
				}
				if (g_aborted.load()) {
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
			g_lockZ = g_forceAll.load() && a_job.size < kSmallObject;
			g_logAim = true;
			g_logFoundation = true;
			g_logSnap = true;
			PostNewItem(a_entry, a_job.formID);
			const bool got = WaitFor([&] { return CurrentItem() && CurrentItem() != before; }, 3000);
			if (got) {
				WaitFor([&] {
					const auto& p = Field<float[3]>(kPreviewPos);
					return std::abs(p[0] - a_job.x) < 0.5f && std::abs(p[1] - a_job.y) < 0.5f;
				}, 1000);
				std::this_thread::sleep_for(std::chrono::milliseconds(150));   // 3D load and snapping settle
			}
			const auto& p = Field<float[3]>(kPreviewPos);
			const auto status = Field<std::uint8_t>(kStatus);
			if (!got) {
				REX::WARN("builder: {} ({:08X}) no preview", a_job.name, a_job.formID);
				return false;
			}
			// small objects whose preview is not at the saved height have no floor yet (their hab is not
			// built): leave them for a later pass, except on the last one
			if (!g_forceAll.load() && a_job.size < kSmallObject && std::abs(p[2] - a_job.z) > 0.3f) {
				REX::INFO("builder: {} ({:08X}, size {:.0f}) at height {:.2f}, saved {:.2f}: later", a_job.name, a_job.formID, a_job.size, p[2], a_job.z);
				return false;
			}
			// early passes: a status the commit hook will not force would only be refused; try it next pass
			if (!g_forceAll.load() && status != kStatusOk && status != 4 && status != 0x18) {
				REX::INFO("builder: {} ({:08X}) status {}: later", a_job.name, a_job.formID, status);
				return false;
			}
			const auto placedBefore = g_placed.load();
			// a hab goes to the auto-foundation state on the first commit and needs another commit there
			// (like confirming the foundation by hand): habs get up to 3 commits, other objects one
			const bool hab = a_job.size >= kSmallObject;
			const int attempts = hab ? 3 : 1;
			bool committed = false;
			for (int attempt = 1; attempt <= attempts && !committed && !g_aborted.load(); ++attempt) {
				PostCommit();
				committed = WaitFor([&] { return g_placed.load() != placedBefore; }, hab ? 1500 : 1000);
				if (!committed && attempt < attempts) {
					REX::INFO("builder: {} not placed after commit {}, committing again", a_job.name, attempt);
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
			// then small things from the lowest saved height up: a desk before the computer on it
			std::stable_sort(a_jobs.begin(), a_jobs.end(), [](const Job& a, const Job& b) {
				const bool bigA = a.size >= kSmallObject, bigB = b.size >= kSmallObject;
				if (bigA != bigB) return bigA;
				if (bigA) return a.size > b.size;
				if (a.z != b.z) return a.z < b.z;
				return a.size > b.size;
			});
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
			g_lockZ = false;
			g_forcing = false;
			PostCancel();   // drop the last preview the engine re-offers after a commit
			REX::INFO("builder: done, built {}, failed {}, of {}", g_built.load(), g_failed.load(), g_total.load());
			g_running = false;
		}
	}

	void Install()
	{
		g_aim = HookCall<Aim_t>(kUpdatePlacement, kAimCallOff, &AimHook, "preview aim (21CE760)");
		// the snapped state aims again every frame (21B4EC0 +0x9D, same 21CE760) and moves to the snap point
		// nearest that aim: aim it at the saved spot too, or it snaps where the player happens to look
		if (auto snapAim = HookCall<Aim_t>(REL::ID(121290), 0x9D, &AimHook, "snapped aim (21CE760)"); snapAim && snapAim != g_aim) {
			REX::WARN("builder: snapped aim calls {:X}, placement aim {:X}", reinterpret_cast<std::uintptr_t>(snapAim), reinterpret_cast<std::uintptr_t>(g_aim));
		}
		g_precheck = HookCall<Precheck_t>(kUpdatePlacement, kPrecheckCallOff, &PrecheckHook, "placement pre-check (21D3DD0)");
		g_update = HookCall<Update_t>(kStateUpdatePlacement, kUpdateCallOff, &UpdateHook, "placement update (21D74B0)");
		g_foundation = HookCall<Foundation_t>(kStateUpdateFoundation, kFoundationCallOff, &FoundationHook, "foundation height (21D5CE0)");
		g_snapSearch = HookCall<SnapSearch_t>(kUpdatePlacement, 0x6C4, &SnapSearchHook, "snap search (21D2E20)");
		HookSlot<0>(REL::ID(466620), 0xB8 / 8, "StateUpdatePlacement commit (21B2E20)");
		HookSlot<1>(REL::ID(466892), 0xB8 / 8, "StateUpdateAutoFoundation commit (21B2360)");
		// a hab next to or on another hab snaps first; this state commits only with +0x188 (snap found),
		// status 2 and +0x630 == 4
		HookSlot<2>(REL::ID(466661), 0xB8 / 8, "StateUpdateSnapped commit (21B4B70)");
		[]<std::size_t... I>(std::index_sequence<I...>) { (HookCmd<I>(), ...); }(std::make_index_sequence<kGuardCount>{});
		REX::INFO("builder: player command lock installed ({} commands)", kGuardCount);
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
		static bool sinkRegistered = false;
		if (!sinkRegistered) {
			using src_t = RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>* (*)();
			if (auto* src = REL::Relocation<src_t>{ REL::ID(73963) }()) {   // +0xF4F840
				static PlacedSink sink;
				src->RegisterSink(&sink);
				sinkRegistered = true;
			}
		}
		for (auto& a : g_allow) {
			a = 0;
		}
		g_stop = false;
		g_aborted = false;
		g_done = 0;
		g_built = 0;
		g_failed = 0;
		g_total = std::int32_t(a_jobs.size());
		std::thread(Worker, std::move(a_jobs)).detach();
		return true;
	}

	std::vector<std::int32_t> Progress()
	{
		return { g_done.load(), g_built.load(), g_failed.load(), g_total.load(), g_running.load() ? 1 : 0, g_aborted.load() ? 1 : 0 };
	}

	void Stop()
	{
		g_stop = true;
	}

	bool IsPlacing()
	{
		return Field<void*>(kItem) != nullptr;
	}

	bool InBuildMenu(std::uint32_t a_formID)
	{
		return LookupEntry(a_formID) != nullptr;
	}
}
