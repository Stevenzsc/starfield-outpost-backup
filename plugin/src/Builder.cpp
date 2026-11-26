// Auto-build through the workshop engine (Starfield 1.16.244, see docs/re-workshop.md).
//
// Per object: look up its build-menu entry (21A6B60, keyed by base form ID), post NewItemEvent, let the
// engine create and load the preview, force the preview's position and yaw every frame, then post CommitEvent.
// While a build runs, the placement pre-check passes and a blocked (red) placement counts as valid.
// The build is a state machine stepped by an SFSE permanent task (Tick), one thread at a time, instead of a
// thread of its own: the build-menu lookup (21A6B60) walks the menu table unlocked, so it must run where the
// game itself runs frame work.

#include "Builder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numbers>

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
		constexpr std::ptrdiff_t kPrecheckCallOff = 0x32D;  // call 21D3DD0 (placement check: raycast + per-mode validator, 0 = ok)
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

		std::atomic<std::uint32_t> g_jobForm{ 0 };   // base of the object being built
		std::mutex g_placedLock;
		std::array<float, 4> g_placedAt{};           // x y z, angle z (radians) of the last placed object
		bool g_placedValid = false;
		std::vector<std::string> g_offList;          // objects not at their saved spot (checked after each build)
		std::vector<Placed> g_jobRefs;               // refs placed while building the current object
		std::vector<Placed> g_allRefs;               // every ref placed by this build (positions after corrections)
		std::vector<Correction> g_corrections;       // moves that put misplaced objects on their saved spot
		void AddCorrections(const Job& a_job, const std::array<float, 4>& a_at, const std::vector<Placed>& a_refs);

		class PlacedSink : public RE::BSTEventSink<RE::Workshop::ItemPlacedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemPlacedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>*) override
			{
				// the object itself, or for a PackIn its group dummy (OutpostGroupPackinDummy 0x15804)
				if (auto* ref = a_event.item.get()) {
					const auto base = ref->GetBaseObject();
					const auto id = base ? base->GetFormID() : 0;
					const auto p = ref->GetPosition();
					const auto r = ref->GetAngle();
					std::scoped_lock l(g_placedLock);
					g_jobRefs.push_back({ ref->GetFormID(), id, p.x, p.y, p.z, r.x, r.y, r.z });
					if (id && (id == g_jobForm.load() || id == 0x15804)) {
						g_placedAt = { p.x, p.y, p.z, r.z };
						g_placedValid = true;
					}
				}
				++g_placed;
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		std::mutex g_targetLock;
		float g_tx = 0, g_ty = 0, g_tz = 0, g_tyaw = 0;
		float g_hx = 0, g_hy = 0;                    // half the bounds of the object being built (local x, y)
		int g_snapTry = 0, g_snapOrigin = -1, g_snapCurrent = 0;   // snap ray origin cycling (see SnapSearchHook)

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
		std::atomic<bool> g_logSnapSearch{ false };

		std::uintptr_t SnapSearchHook(void* a_engine)
		{
			if (!g_forcing.load()) {
				return g_snapSearch(a_engine);
			}
			const auto e = reinterpret_cast<std::uintptr_t>(a_engine);
			// The search casts a ray from the preview (+0x138) along the camera's forward (+0xC0) and snaps to the
			// hab it hits. The player's camera points anywhere: cast straight down from 1.5 m above the saved spot
			// instead, so an upper floor finds the roof under it and a ground hab the ground.
			auto* dir = reinterpret_cast<float*>(e + 0xC0);
			auto* pos = reinterpret_cast<float*>(e + kPreviewPos);
			const float oldDir[3] = { dir[0], dir[1], dir[2] };
			const float old[3] = { pos[0], pos[1], pos[2] };
			// A corridor joins two habs: straight down from its middle hits the ground between them. Until a
			// snap lands on the saved spot, also try straight down at each end (over a hab's doorway): the
			// origin cycles middle, +x, -x, +y, -y (local axes, half the bounds), a few frames each.
			float ox = 0, oy = 0;
			{
				std::scoped_lock l(g_targetLock);
				int k = g_snapOrigin >= 0 ? g_snapOrigin : (g_snapTry++ / 3) % 5;
				g_snapCurrent = k;
				const float lx = k == 1 ? g_hx : k == 2 ? -g_hx : 0.0f;
				const float ly = k == 3 ? g_hy : k == 4 ? -g_hy : 0.0f;
				const float c = std::cos(g_tyaw), s = std::sin(g_tyaw);
				ox = lx * c + ly * s;
				oy = -lx * s + ly * c;
			}
			dir[0] = 0.0f;
			dir[1] = 0.0f;
			dir[2] = -1.0f;
			pos[0] = old[0] + ox;
			pos[1] = old[1] + oy;
			pos[2] = old[2] + 1.5f;
			if (g_logSnapSearch.exchange(false)) {
				REX::INFO("builder: snap search from {:.2f} {:.2f} {:.2f} straight down (camera forward was {:.2f} {:.2f} {:.2f})", pos[0], pos[1], pos[2],
					oldDir[0], oldDir[1], oldDir[2]);
			}
			const float cast[3] = { pos[0], pos[1], pos[2] };
			const auto r = g_snapSearch(a_engine);
			dir[0] = oldDir[0];
			dir[1] = oldDir[1];
			dir[2] = oldDir[2];
			if (pos[0] == cast[0] && pos[1] == cast[1] && pos[2] == cast[2]) {   // not moved to a snap point: back
				pos[0] = old[0];
				pos[1] = old[1];
				pos[2] = old[2];
			}
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
			if (g_logSnap.exchange(false) || (match && g_snapOrigin < 0)) {
				REX::INFO("builder: snap found at {:.2f} {:.2f} {:.2f} (cast {}), saved {:.2f} {:.2f} {:.2f}: {}", p[0], p[1], p[2], g_snapCurrent, tx, ty, tz,
					match ? "using it" : "ignoring it");
			}
			if (match) {
				g_snapOrigin = g_snapCurrent;   // keep casting from there
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
				// x y come from the anchor (+0x480): where the first commit put the hab (maybe a snap point)
				*reinterpret_cast<float*>(e + 0x480) = g_tx;
				*reinterpret_cast<float*>(e + 0x484) = g_ty;
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
				if constexpr (I == 0) {
					// placement: the preview may have moved to a snap point (another hab's connection, another
					// wall); put it back on the saved spot and angle, drop the snap, re-apply (item vfunc 0xE0)
					const auto e = reinterpret_cast<std::uintptr_t>(a_engine);
					auto* p = reinterpret_cast<float*>(e + kPreviewPos);
					{
						std::scoped_lock l(g_targetLock);
						if (std::abs(p[0] - g_tx) > 0.05f || std::abs(p[1] - g_ty) > 0.05f || std::abs(p[2] - g_tz) > 0.05f) {
							REX::INFO("builder: preview at {:.2f} {:.2f} {:.2f} moved back to {:.2f} {:.2f} {:.2f}", p[0], p[1], p[2], g_tx, g_ty, g_tz);
						}
						p[0] = g_tx;
						p[1] = g_ty;
						p[2] = g_tz;
						*reinterpret_cast<float*>(e + kYaw) = g_tyaw;
					}
					*reinterpret_cast<std::uint8_t*>(e + 0x188) = 0;
					*reinterpret_cast<std::uint8_t*>(e + 0x18A) = 0;
					if (auto* item = *reinterpret_cast<void**>(e + kItem)) {
						using apply_t = void (*)(void*, void*);
						(*reinterpret_cast<apply_t**>(item))[0xE0 / 8](item, a_engine);
					}
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

		// StateUpdatePlacement vfunc 0x98 (21B40E0): the "snapped" event. It carries the snap position
		// (event+0x08, x y z) and the hab snapped to (event+0x18), copies them to engine+0x480/+0x490 and
		// switches to StateUpdateSnapped. The engine picks the connection nearest the aim, not always the
		// saved one: drop the event then, so the hab stays in placement at its saved spot.
		using SnapEvent_t = std::uintptr_t (*)(void* a_state, void* a_event, void* a_engine, void* a_4);
		SnapEvent_t g_snapEvent = nullptr;

		std::uintptr_t SnapEventHook(void* a_state, void* a_event, void* a_engine, void* a_4)
		{
			if (g_forcing.load() && a_event) {
				const auto* p = reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(a_event) + 0x08);
				float tx, ty, tz;
				{
					std::scoped_lock l(g_targetLock);
					tx = g_tx; ty = g_ty; tz = g_tz;
				}
				const bool match = std::abs(p[0] - tx) < 1.0f && std::abs(p[1] - ty) < 1.0f && std::abs(p[2] - tz) < 1.0f;
				if (g_logSnap.exchange(false)) {
					REX::INFO("builder: snap to {:.2f} {:.2f} {:.2f}, saved {:.2f} {:.2f} {:.2f}: {}", p[0], p[1], p[2], tx, ty, tz,
						match ? "using it" : "ignoring it");
				}
				if (!match) {
					return 0;
				}
			}
			return g_snapEvent(a_state, a_event, a_engine, a_4);
		}

		void HookSnapEvent()
		{
			auto* slot = reinterpret_cast<std::uintptr_t*>(REL::ID(466620).address()) + 0x98 / 8;
			unsigned long old = 0;
			if (!VirtualProtect(slot, sizeof(*slot), 0x04, &old)) {
				REX::ERROR("builder: VirtualProtect failed for the snap event");
				return;
			}
			g_snapEvent = reinterpret_cast<SnapEvent_t>(*slot);
			*slot = reinterpret_cast<std::uintptr_t>(&SnapEventHook);
			VirtualProtect(slot, sizeof(*slot), old, &old);
			REX::INFO("builder: hooked StateUpdatePlacement snap event (21B40E0)");
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

		// ---- build state machine ----
		// Everything below is stepped by an SFSE permanent task (Tick). In game, that task ran many times per frame
		// and on several job threads (test 2026-11-27: a 66-"frame" wait took 73 ms), so Tick lets one caller in
		// at a time and every wait measures time: each call adds the time since the last call, but at most
		// kMaxStepMs, so a paused game (menu, alt-tab) does not use up a timeout.

		// time budgets (ms), the same as the old worker thread's waits
		constexpr float kPreviewMs = 3000.0f;     // NewItem -> preview item exists
		constexpr float kAimMs = 1000.0f;         // preview reaches the saved spot
		constexpr float kSettleMs = 150.0f;       // 3D load and snapping settle
		constexpr float kCommitMsHab = 1500.0f;   // commit -> ItemPlacedEvent, per attempt
		constexpr float kCommitMsOther = 1000.0f;
		constexpr float kAfterCommitMs = 100.0f;  // let the placed references report in
		constexpr float kMaxStepMs = 50.0f;       // most one call may add (a longer gap is a pause)

		enum class Phase
		{
			kNextJob,
			kWaitPreview,
			kWaitAim,
			kSettle,
			kWaitPlaced,
			kAfterCommit,
		};

		struct Run
		{
			std::vector<Job> jobs;                // sorted; owns the jobs the pointers below refer to
			std::vector<const Job*> todo;         // this pass
			std::vector<const Job*> retry;        // for the next pass
			std::size_t next{ 0 };                // index into todo
			int pass{ 0 };
			Phase phase{ Phase::kNextJob };
			float elapsedMs{ 0 };                 // time spent in the current phase (see kMaxStepMs)
			std::chrono::steady_clock::time_point last{};   // previous Tick call
			const Job* job{ nullptr };
			void* before{ nullptr };              // preview item before NewItem
			std::uint8_t status{ 0 };             // placement status when the commit was decided
			std::uint32_t placedBefore{ 0 };
			int attempt{ 0 }, attempts{ 1 };
			bool hab{ false };
			bool committed{ false };
		};
		Run g_run;                                // touched only by Start (before g_ready) and Tick

		// Post NewItem for the current job. False if it cannot be built now (goes to the next pass).
		bool BeginJob(const Job& a_job)
		{
			// look up right before building: the lookup also sets the menu's selected variant
			void* entry = LookupEntry(a_job.formID);
			if (!entry) {
				REX::WARN("builder: {} ({:08X}) is not in the build menu", a_job.name, a_job.formID);
				return false;
			}
			{
				std::scoped_lock l(g_targetLock);
				g_tx = a_job.x;
				g_ty = a_job.y;
				g_tz = a_job.z;
				g_tyaw = a_job.yawDeg * std::numbers::pi_v<float> / 180.0f;
				g_hx = a_job.hx;
				g_hy = a_job.hy;
				g_snapTry = 0;
				g_snapOrigin = -1;
			}
			g_run.before = CurrentItem();
			g_lockZ = g_forceAll.load() && a_job.size < kSmallObject;
			g_logAim = true;
			g_logFoundation = true;
			g_logSnap = true;
			g_logSnapSearch = true;
			PostNewItem(entry, a_job.formID);
			return true;
		}

		// Preview settled: commit it, or leave it for a later pass. True if a commit was posted.
		bool DecideCommit(const Job& a_job)
		{
			const auto& p = Field<float[3]>(kPreviewPos);
			g_run.status = Field<std::uint8_t>(kStatus);
			// small objects whose preview is not at the saved height have no floor yet (their hab is not
			// built): leave them for a later pass, except on the last one
			if (!g_forceAll.load() && a_job.size < kSmallObject && std::abs(p[2] - a_job.z) > 0.3f) {
				REX::INFO("builder: {} ({:08X}, size {:.0f}) at height {:.2f}, saved {:.2f}: later", a_job.name, a_job.formID, a_job.size, p[2], a_job.z);
				return false;
			}
			// early passes: a status the commit hook will not force would only be refused; try it next pass
			const auto status = g_run.status;
			if (!g_forceAll.load() && status != kStatusOk && status != 4 && status != 0x18) {
				REX::INFO("builder: {} ({:08X}) status {}: later", a_job.name, a_job.formID, status);
				return false;
			}
			{
				std::scoped_lock l(g_placedLock);
				g_placedValid = false;
				g_jobRefs.clear();
			}
			g_jobForm = a_job.formID;
			g_run.placedBefore = g_placed.load();
			// a hab goes to the auto-foundation state on the first commit and needs another commit there
			// (like confirming the foundation by hand): habs get up to 3 commits, other objects one
			g_run.hab = a_job.size >= kSmallObject;
			g_run.attempts = g_run.hab ? 3 : 1;
			g_run.attempt = 1;
			g_run.committed = false;
			PostCommit();
			return true;
		}

		// After the commit window: log, check where it went, queue corrections.
		void RecordResult(const Job& a_job)
		{
			const auto& p = Field<float[3]>(kPreviewPos);
			const bool committed = g_run.committed;
			REX::INFO("builder: {} ({:08X}, size {:.0f}) at {:.2f} {:.2f} {:.2f} (target {:.2f} {:.2f} {:.2f}) status {} -> {}", a_job.name, a_job.formID, a_job.size,
				p[0], p[1], p[2], a_job.x, a_job.y, a_job.z, g_run.status, committed ? "built" : "not built");
			if (!committed) {
				return;
			}
			// check where it really went
			std::array<float, 4> at{};
			bool valid;
			std::vector<Placed> refs;
			{
				std::scoped_lock l(g_placedLock);
				at = g_placedAt;
				valid = g_placedValid;
				refs = g_jobRefs;
			}
			if (valid) {
				const float yaw = a_job.yawDeg * std::numbers::pi_v<float> / 180.0f;
				float dAng = std::remainder(at[3] - yaw, 2.0f * std::numbers::pi_v<float>) * 180.0f / std::numbers::pi_v<float>;
				const float dPos = std::hypot(at[0] - a_job.x, at[1] - a_job.y);
				const float dZ = at[2] - a_job.z;
				if (dPos > 0.3f || std::abs(dZ) > 0.3f || std::abs(dAng) > 5.0f) {
					auto line = std::format("{} at {:.2f} {:.2f} {:.2f} (saved {:.2f} {:.2f} {:.2f}): off {:.2f} m, height {:+.2f} m, angle {:+.0f} deg",
						a_job.name, at[0], at[1], at[2], a_job.x, a_job.y, a_job.z, dPos, dZ, dAng);
					REX::WARN("builder: check: {}", line);
					g_offList.push_back(std::move(line));
					AddCorrections(a_job, at, refs);
				} else {
					std::scoped_lock l(g_placedLock);
					g_allRefs.insert(g_allRefs.end(), refs.begin(), refs.end());
				}
			} else {
				REX::INFO("builder: check: {} placed, but its reference was not reported", a_job.name);
				std::scoped_lock l(g_placedLock);
				g_allRefs.insert(g_allRefs.end(), refs.begin(), refs.end());
			}
		}

		// An object that landed off its saved spot: move every reference its commit placed (one for a plain
		// object; the group dummy and all parts for a PackIn) by the same turn about the pivot and shift, so the
		// pivot ends on the saved spot and angle. Heading is clockwise seen from above (forward = sin, cos).
		void AddCorrections(const Job& a_job, const std::array<float, 4>& a_at, const std::vector<Placed>& a_refs)
		{
			constexpr float toDeg = 180.0f / std::numbers::pi_v<float>;
			const float yaw = a_job.yawDeg / toDeg;
			const float d = yaw - a_at[3];
			const float c = std::cos(d), s = std::sin(d);
			std::scoped_lock l(g_placedLock);
			for (const auto& r : a_refs) {
				Correction m{ r.ref };
				if (a_refs.size() == 1 && r.base == 0x15804) {
					break;   // a hab reports only its group dummy, not its walls: moving the dummy alone does nothing
				}
				if (a_refs.size() == 1) {
					m.x = a_job.x; m.y = a_job.y; m.z = a_job.z;
					m.ax = a_job.axDeg; m.ay = a_job.ayDeg; m.az = a_job.yawDeg;
				} else {
					const float dx = r.x - a_at[0], dy = r.y - a_at[1];
					m.x = a_job.x + dx * c + dy * s;
					m.y = a_job.y - dx * s + dy * c;
					m.z = r.z + (a_job.z - a_at[2]);
					m.ax = r.ax * toDeg; m.ay = r.ay * toDeg;
					m.az = std::remainder(r.az + d, 2.0f * std::numbers::pi_v<float>) * toDeg;
				}
				g_corrections.push_back(m);
				g_allRefs.push_back({ r.ref, r.base, m.x, m.y, m.z, m.ax / toDeg, m.ay / toDeg, m.az / toDeg });
			}
			REX::INFO("builder: correction for {}: {} references to move", a_job.name, a_refs.size());
		}

		// Habs snap to the habs already there, and the engine picks the connection nearest the aim, which is
		// not always the saved one. So build the hab with the most neighbours first (nothing to snap to: it
		// goes exactly to its saved spot), then always the hab touching the most built ones, lower floors first.
		// Expects the big objects (size >= kSmallObject) at the front of a_jobs.
		void OrderHabs(std::vector<Job>& a_jobs)
		{
			const auto bigEnd = std::ranges::find_if(a_jobs, [](const Job& j) { return j.size < kSmallObject; });
			const std::size_t n = std::size_t(bigEnd - a_jobs.begin());
			if (n < 2) {
				return;
			}
			// backups from 0.6 on keep each object's reference ID; it rises in the order the player built,
			// and that order snapped every hab right once: replay it
			if (std::all_of(a_jobs.begin(), bigEnd, [](const Job& j) { return j.order != 0; })) {
				std::stable_sort(a_jobs.begin(), bigEnd, [](const Job& a, const Job& b) { return a.order < b.order; });
				for (std::size_t i = 0; i < n; ++i) {
					const auto& j = a_jobs[i];
					REX::INFO("builder: hab order {} (as built, ref {:08X}): {} at {:.2f} {:.2f} {:.2f}", i + 1, j.order, j.name, j.x, j.y, j.z);
				}
				return;
			}
			auto radius = [](const Job& j) { return 0.5f * std::sqrt(j.size); };
			std::vector<std::vector<std::size_t>> links(n);
			for (std::size_t i = 0; i < n; ++i) {
				for (std::size_t k = i + 1; k < n; ++k) {
					const auto& a = a_jobs[i];
					const auto& b = a_jobs[k];
					const float d = std::hypot(a.x - b.x, a.y - b.y);
					// side by side, or stacked (one floor apart)
					if (d < (radius(a) + radius(b)) * 1.15f + 1.0f && std::abs(a.z - b.z) < 5.0f) {
						links[i].push_back(k);
						links[k].push_back(i);
					}
				}
			}
			std::vector<bool> placed(n, false);
			std::vector<Job> ordered;
			ordered.reserve(n);
			for (std::size_t step = 0; step < n; ++step) {
				std::size_t best = n;
				std::size_t bestLinked = 0;
				for (std::size_t i = 0; i < n; ++i) {
					if (placed[i]) {
						continue;
					}
					const auto linked = std::size_t(std::ranges::count_if(links[i], [&](std::size_t k) { return placed[k]; }));
					if (best == n) {
						best = i;
						bestLinked = linked;
						continue;
					}
					const auto& c = a_jobs[i];
					const auto& b = a_jobs[best];
					// first hab: most neighbours; then: touches the most built habs, lower floor, more neighbours
					bool better;
					if (step > 0 && linked != bestLinked) {
						better = linked > bestLinked;
					} else if (step > 0 && std::abs(c.z - b.z) > 1.0f) {
						better = c.z < b.z;
					} else if (links[i].size() != links[best].size()) {
						better = links[i].size() > links[best].size();
					} else {
						better = c.size > b.size;
					}
					if (better) {
						best = i;
						bestLinked = linked;
					}
				}
				placed[best] = true;
				const auto& j = a_jobs[best];
				REX::INFO("builder: hab order {}: {} at {:.2f} {:.2f} {:.2f}, {} neighbours, {} already built", step + 1, j.name, j.x, j.y, j.z,
					links[best].size(), bestLinked);
				ordered.push_back(j);
			}
			std::ranges::move(ordered, a_jobs.begin());
		}

		// Passes: objects that the engine refuses (e.g. furniture before its hab exists) are retried after the
		// others; the last pass forces every placement status to OK.
		constexpr int kPasses = 3;

		std::atomic<bool> g_ready{ false };      // g_run is filled in and Tick may drive it
		bool g_tickInstalled = false;
		std::atomic_flag g_inTick;                // set while one thread runs Step
		std::atomic<std::uint64_t> g_tickCalls{ 0 }, g_tickSkipped{ 0 };   // per build, for the log

		void Finish()
		{
			// whatever was not built: what this pass still had queued, and what it put off
			std::vector<const Job*> left(g_run.todo.begin() + std::min(g_run.next, g_run.todo.size()), g_run.todo.end());
			left.insert(left.end(), g_run.retry.begin(), g_run.retry.end());
			for (const auto* job : left) {
				REX::WARN("builder: {} ({:08X}) could not be built", job->name, job->formID);
				++g_failed;
				++g_done;
			}
			g_forceAll = false;
			g_lockZ = false;
			g_forcing = false;
			PostCancel();   // drop the last preview the engine re-offers after a commit
			REX::INFO("builder: done, built {}, failed {}, of {}{}", g_built.load(), g_failed.load(), g_total.load(), g_aborted.load() ? " (aborted)" : "");
			REX::INFO("builder: frame task ran {} times, {} skipped because another thread was inside", g_tickCalls.load(), g_tickSkipped.load());
			REX::INFO("builder: check: {} built objects not at their saved spot", g_offList.size());
			for (const auto& line : g_offList) {
				REX::INFO("builder: check:   {}", line);
			}
			g_run = {};
			g_ready = false;
			g_running = false;
		}

		// Next pass, or false when the build is over.
		bool NextPass()
		{
			if (g_run.pass > 0) {
				g_run.todo = std::move(g_run.retry);
				g_run.retry.clear();
				g_run.next = 0;
			}
			if (g_run.todo.empty() || g_run.pass >= kPasses || g_stop.load()) {
				return false;
			}
			++g_run.pass;
			g_forceAll = g_run.pass == kPasses;
			REX::INFO("builder: pass {} ({} objects{})", g_run.pass, g_run.todo.size(), g_forceAll.load() ? ", forcing all checks" : "");
			return true;
		}

		void RegisterSink()
		{
			static bool registered = false;
			if (registered) {
				return;
			}
			using src_t = RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>* (*)();
			if (auto* src = REL::Relocation<src_t>{ REL::ID(73963) }()) {   // +0xF4F840
				static PlacedSink sink;
				src->RegisterSink(&sink);
				registered = true;
			}
		}

		// One step of the build. Only called by Tick, by one thread at a time.
		void Step()
		{
			if (g_aborted.load()) {
				Finish();
				return;
			}
			auto& r = g_run;
			const auto now = std::chrono::steady_clock::now();
			if (r.last != std::chrono::steady_clock::time_point{}) {
				const float dt = std::chrono::duration<float, std::milli>(now - r.last).count();
				r.elapsedMs += std::clamp(dt, 0.0f, kMaxStepMs);
			}
			r.last = now;
			switch (r.phase) {
			case Phase::kNextJob:
				{
					const bool first = r.pass == 0;
					if (first) {   // first frame of a build
						RegisterSink();
						g_forcing = true;
					}
					if (first || r.next >= r.todo.size()) {
						if (!NextPass()) {
							Finish();
							return;
						}
					}
					if (g_stop.load()) {
						Finish();
						return;
					}
					r.job = r.todo[r.next++];
					if (BeginJob(*r.job)) {
						r.phase = Phase::kWaitPreview;
						r.elapsedMs = 0;
					} else {
						r.retry.push_back(r.job);
					}
					return;
				}
			case Phase::kWaitPreview:
				if (auto* item = CurrentItem(); item && item != r.before) {
					r.phase = Phase::kWaitAim;
					r.elapsedMs = 0;
				} else if (r.elapsedMs > kPreviewMs) {
					REX::WARN("builder: {} ({:08X}) no preview", r.job->name, r.job->formID);
					r.retry.push_back(r.job);
					r.phase = Phase::kNextJob;
				}
				return;
			case Phase::kWaitAim:
				{
					const auto& p = Field<float[3]>(kPreviewPos);
					const bool there = std::abs(p[0] - r.job->x) < 0.5f && std::abs(p[1] - r.job->y) < 0.5f;
					if (there || r.elapsedMs > kAimMs) {
						r.phase = Phase::kSettle;
						r.elapsedMs = 0;
					}
					return;
				}
			case Phase::kSettle:
				if (r.elapsedMs >= kSettleMs) {
					if (DecideCommit(*r.job)) {
						r.phase = Phase::kWaitPlaced;
					} else {
						r.retry.push_back(r.job);
						r.phase = Phase::kNextJob;
					}
					r.elapsedMs = 0;
				}
				return;
			case Phase::kWaitPlaced:
				if (g_placed.load() != r.placedBefore) {
					r.committed = true;
					r.phase = Phase::kAfterCommit;
					r.elapsedMs = 0;
				} else if (r.elapsedMs > (r.hab ? kCommitMsHab : kCommitMsOther)) {
					if (r.attempt < r.attempts) {
						REX::INFO("builder: {} not placed after commit {}, committing again", r.job->name, r.attempt);
						++r.attempt;
						PostCommit();
					} else {
						r.phase = Phase::kAfterCommit;
					}
					r.elapsedMs = 0;
				}
				return;
			case Phase::kAfterCommit:
				if (r.elapsedMs >= kAfterCommitMs) {
					RecordResult(*r.job);
					if (r.committed) {
						++g_built;
						++g_done;
					} else {
						r.retry.push_back(r.job);
					}
					r.phase = Phase::kNextJob;
				}
				return;
			}
		}

		// ---- wall styles ----
		// Build mode's inspect state picks its target every frame from the crosshair: 21E7570 calls 21DC940
		// (validate, then 21DBC30 sets engine +0x588 and makes the inspected item). Setting +0x588 directly is
		// undone on the next frame, and the item stays empty (test 2026-11-27: "selected 00000000", item refs 0,
		// so CycleBehavior changed nothing). While a style run is active this hook hands 21DC940 the wall's
		// handle instead, so build mode itself inspects that wall.
		constexpr REL::ID kInspectTargetCaller{ 121659 };   // +0x21E7570
		constexpr REL::ID kStateInspectVtbl{ 467015 };      // +0x4D4AC60 WorkshopEngine::StateInspect
		constexpr REL::ID kWorkshopCtx{ 928905 };           // +0x5F5CE40 (engine - 0x4A0); +0x490 current state
		constexpr REL::ID kCycleExec{ 121444 };             // +0x21BFBB0 CycleBehavior TCmd execute
		constexpr REL::ID kCycleCmdVtbl{ 466560 };          // +0x4D49A50 TCmd<..., CycleBehaviorEvent>
		constexpr REL::ID kCycleEventVtbl{ 467307 };        // +0x4D4B848 CycleBehaviorEvent
		constexpr std::ptrdiff_t kSelected = 0x588;         // u32 reference handle build mode inspects

		using SetTarget_t = void (*)(std::uint8_t* a_out, void* a_engine, std::uint32_t a_handle, void* a_4);
		SetTarget_t g_setTarget = nullptr;
		std::atomic<std::uint32_t> g_forceHandle{ 0 };

		void SetTargetHook(std::uint8_t* a_out, void* a_engine, std::uint32_t a_handle, void* a_4)
		{
			if (const auto h = g_forceHandle.load()) {
				a_handle = h;
			}
			g_setTarget(a_out, a_engine, a_handle, a_4);
		}

		constexpr float kStyleSelectMs = 2000.0f;   // build mode inspects the wall, with refs in its item
		constexpr float kStyleSettleMs = 150.0f;    // after that, before pressing "change"
		constexpr float kStyleCycleMs = 4000.0f;    // "change" -> back in StateInspect
		constexpr float kStyleAfterMs = 300.0f;     // at least this long after "change"

		enum class StylePhase
		{
			kNext,
			kSelect,
			kAfterCycle,
		};

		struct StyleRun
		{
			std::vector<StyleTarget> targets;
			std::size_t next{ 0 };
			StylePhase phase{ StylePhase::kNext };
			float elapsedMs{ 0 };
			std::chrono::steady_clock::time_point last{};
			const StyleTarget* t{ nullptr };
			std::uint32_t before{ 0 };   // wall base before "change"
			int pressed{ 0 }, skipped{ 0 };
		};
		StyleRun g_styles;
		std::atomic<bool> g_stylesReady{ false };
		std::atomic<bool> g_stylesRunning{ false };

		void Advance(float& a_elapsedMs, std::chrono::steady_clock::time_point& a_last)
		{
			const auto now = std::chrono::steady_clock::now();
			if (a_last != std::chrono::steady_clock::time_point{}) {
				const float dt = std::chrono::duration<float, std::milli>(now - a_last).count();
				a_elapsedMs += std::clamp(dt, 0.0f, kMaxStepMs);
			}
			a_last = now;
		}

		bool InInspect()
		{
			const auto state = *reinterpret_cast<std::uintptr_t*>(kWorkshopCtx.address() + 0x490);
			return state && *reinterpret_cast<std::uintptr_t*>(state) == kStateInspectVtbl.address();
		}

		std::uintptr_t StateVtblOffset()
		{
			const auto state = *reinterpret_cast<std::uintptr_t*>(kWorkshopCtx.address() + 0x490);
			const auto base = kEngine.address() - 0x5F5D2E0;
			return state ? *reinterpret_cast<std::uintptr_t*>(state) - base : 0;
		}

		// preview refs of the item build mode holds: item +0x08 = {u32 size, u32 cap}
		std::uint32_t ItemRefCount()
		{
			const auto item = reinterpret_cast<std::uintptr_t>(CurrentItem());
			return item ? *reinterpret_cast<std::uint32_t*>(item + 0x08) : 0;
		}

		std::uint32_t BaseOf(std::uint32_t a_ref)
		{
			auto* ref = RE::TESForm::LookupByID(a_ref);
			if (!ref) {
				return 0;
			}
			const auto obj = static_cast<RE::TESObjectREFR*>(ref)->GetBaseObject();
			return obj ? obj->GetFormID() : 0;
		}

		// Run the CycleBehavior command the way the input handler (21F4F30) builds it.
		void PressChange()
		{
			using exec_t = void (*)(void* a_cmd);
			struct
			{
				std::uintptr_t vtbl, fn, ctx, eventVtbl;
				std::uint8_t eventByte, pad[7];
			} cmd{ kCycleCmdVtbl.address(), kCycleExec.address(), kWorkshopCtx.address(), kCycleEventVtbl.address(), 0, {} };
			REL::Relocation<exec_t>{ kCycleExec }(&cmd);
		}

		void FinishStyles()
		{
			g_forceHandle = 0;
			REX::INFO("styles: run done, pressed change on {} walls, {} skipped", g_styles.pressed, g_styles.skipped);
			g_styles = {};
			g_stylesReady = false;
			g_stylesRunning = false;
		}

		void StyleStep()
		{
			auto& s = g_styles;
			Advance(s.elapsedMs, s.last);
			switch (s.phase) {
			case StylePhase::kNext:
				{
					if (s.next >= s.targets.size()) {
						FinishStyles();
						return;
					}
					s.t = &s.targets[s.next++];
					s.before = BaseOf(s.t->ref);
					if (s.before == s.t->want) {
						REX::INFO("styles: {} wall {:08X} is {:08X}: done", s.t->name, s.t->ref, s.before);
						return;
					}
					g_forceHandle = s.t->handle;
					s.phase = StylePhase::kSelect;
					s.elapsedMs = 0;
					return;
				}
			case StylePhase::kSelect:
				{
					const auto selected = Field<std::uint32_t>(kSelected);
					const auto refs = ItemRefCount();
					if (InInspect() && selected == s.t->handle && refs > 0 && s.elapsedMs >= kStyleSettleMs) {
						REX::INFO("styles: {} wall {:08X} ({:08X}) inspected, item refs {}: change", s.t->name, s.t->ref, s.before, refs);
						PressChange();
						++s.pressed;
						s.phase = StylePhase::kAfterCycle;
						s.elapsedMs = 0;
					} else if (s.elapsedMs > kStyleSelectMs) {
						REX::WARN("styles: {} wall {:08X}: not inspected (selected {:08X}, want handle {:08X}, item refs {}, state +{:X})", s.t->name,
							s.t->ref, selected, s.t->handle, refs, StateVtblOffset());
						++s.skipped;
						s.phase = StylePhase::kNext;
					}
					return;
				}
			case StylePhase::kAfterCycle:
				if ((s.elapsedMs >= kStyleAfterMs && InInspect()) || s.elapsedMs > kStyleCycleMs) {
					const auto after = BaseOf(s.t->ref);
					REX::INFO("styles: {} wall {:08X}: {:08X} -> change -> {:08X}, want {:08X}{}{}", s.t->name, s.t->ref, s.before, after, s.t->want,
						after == 0 ? " (reference replaced)" : "", InInspect() ? "" : std::format(" (still in state +{:X})", StateVtblOffset()));
					s.phase = StylePhase::kNext;
				}
				return;
			}
		}

		// SFSE permanent task. It can run several times per frame and on different job threads: let one in.
		void Tick()
		{
			if (!g_ready.load() && !g_stylesReady.load()) {
				return;
			}
			++g_tickCalls;
			if (g_inTick.test_and_set(std::memory_order_acquire)) {
				++g_tickSkipped;
				return;
			}
			if (g_ready.load()) {   // a thread that was inside may just have finished the build
				Step();
			} else if (g_stylesReady.load()) {
				StyleStep();
			}
			g_inTick.clear(std::memory_order_release);
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
		HookCall<SnapSearch_t>(kUpdatePlacement, 0xCE8, &SnapSearchHook, "snap search 2 (21D2E20)");
		HookCall<SnapSearch_t>(kUpdatePlacement, 0xDE8, &SnapSearchHook, "snap search 3 (21D2E20)");
		HookSnapEvent();
		HookSlot<0>(REL::ID(466620), 0xB8 / 8, "StateUpdatePlacement commit (21B2E20)");
		HookSlot<1>(REL::ID(466892), 0xB8 / 8, "StateUpdateAutoFoundation commit (21B2360)");
		// a hab next to or on another hab snaps first; this state commits only with +0x188 (snap found),
		// status 2 and +0x630 == 4
		HookSlot<2>(REL::ID(466661), 0xB8 / 8, "StateUpdateSnapped commit (21B4B70)");
		// build mode's inspect target (both calls in 21E7570 go to 21DC940)
		g_setTarget = HookCall<SetTarget_t>(kInspectTargetCaller, 0x78B, &SetTargetHook, "inspect target (21DC940)");
		if (auto second = HookCall<SetTarget_t>(kInspectTargetCaller, 0x7FE, &SetTargetHook, "inspect target 2 (21DC940)"); second != g_setTarget) {
			REX::WARN("builder: inspect target calls differ: {:X} {:X}", reinterpret_cast<std::uintptr_t>(g_setTarget), reinterpret_cast<std::uintptr_t>(second));
		}
		[]<std::size_t... I>(std::index_sequence<I...>) { (HookCmd<I>(), ...); }(std::make_index_sequence<kGuardCount>{});
		REX::INFO("builder: player command lock installed ({} commands)", kGuardCount);
	}

	bool Start(std::vector<Job> a_jobs)
	{
		if (g_running.exchange(true)) {
			return false;
		}
		if (g_stylesRunning.load()) {
			REX::WARN("builder: a style run is active");
			g_running = false;
			return false;
		}
		if (!g_aim || !g_precheck || !g_update || !g_tickInstalled) {
			REX::ERROR("builder: hooks or frame task missing");
			g_running = false;
			return false;
		}
		// big things first (habs, structures), so furniture has a floor to land on
		// then small things from the lowest saved height up: a desk before the computer on it
		std::stable_sort(a_jobs.begin(), a_jobs.end(), [](const Job& a, const Job& b) {
			const bool bigA = a.size >= kSmallObject, bigB = b.size >= kSmallObject;
			if (bigA != bigB) return bigA;
			if (bigA) return a.size > b.size;
			if (a.z != b.z) return a.z < b.z;
			return a.size > b.size;
		});
		OrderHabs(a_jobs);
		REX::INFO("builder: start, {} objects", a_jobs.size());

		for (auto& a : g_allow) {
			a = 0;
		}
		g_stop = false;
		g_aborted = false;
		g_done = 0;
		g_offList.clear();
		{
			std::scoped_lock l(g_placedLock);
			g_corrections.clear();
			g_allRefs.clear();
		}
		g_built = 0;
		g_failed = 0;
		g_total = std::int32_t(a_jobs.size());
		g_tickCalls = 0;
		g_tickSkipped = 0;

		g_run = {};
		g_run.jobs = std::move(a_jobs);
		for (const auto& job : g_run.jobs) {
			g_run.todo.push_back(&job);
		}
		g_ready = true;   // Tick (main thread) takes it from the next frame
		return true;
	}

	void InstallTick()
	{
		const auto* tasks = SFSE::GetTaskInterface();
		if (!tasks) {
			REX::ERROR("builder: no SFSE task interface; auto-build disabled");
			return;
		}
		tasks->AddPermanentTask([]() { Tick(); });
		g_tickInstalled = true;
		REX::INFO("builder: frame task installed");
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

	bool StartStyles(std::vector<StyleTarget> a_targets)
	{
		if (a_targets.empty() || g_running.load() || !g_tickInstalled || !g_setTarget) {
			return false;
		}
		if (g_stylesRunning.exchange(true)) {
			return false;
		}
		REX::INFO("styles: run start, {} walls", a_targets.size());
		g_styles = {};
		g_styles.targets = std::move(a_targets);
		g_stylesReady = true;
		return true;
	}

	bool StylesRunning()
	{
		return g_stylesRunning.load();
	}

	bool InBuildMenu(std::uint32_t a_formID)
	{
		return LookupEntry(a_formID) != nullptr;
	}

	std::vector<Correction> Corrections()
	{
		std::scoped_lock l(g_placedLock);
		return g_corrections;
	}

	std::vector<Placed> PlacedRefs()
	{
		std::scoped_lock l(g_placedLock);
		return g_allRefs;
	}
}
