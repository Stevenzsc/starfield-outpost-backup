// Diagnostics for reverse engineering workshop placement (enabled when OutpostBackup\diag.txt exists):
// - logs every Workshop::ItemPlacedEvent / WorkshopItemPlacedEvent with the item and the call stack;
// - hooks Workshop::PackinWorkshopItem::vfunc[0xA0] (commit) and dumps the item and the engine object,
//   naming every pointer that is a TESForm / TESObjectREFR.
// See docs/re-workshop.md.

#include "Diag.h"

#include <thread>

extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void**, unsigned long*);
extern "C" __declspec(dllimport) int __stdcall VirtualProtect(void*, std::size_t, unsigned long, unsigned long*);
extern "C" __declspec(dllimport) int __stdcall IsBadReadPtr(const void*, std::uintptr_t);

namespace OSB::Diag
{
	namespace
	{
		// Address Library 1.16.244
		constexpr REL::ID kItemPlacedSource{ 73963 };          // +0xF4F840
		constexpr REL::ID kWorkshopItemPlacedSource{ 73962 };  // +0xF4F7A0
		// workshop item vtables whose vfunc[0xA0] (commit) is hooked
		struct ItemClass { REL::ID vtbl; const char* name; };
		const ItemClass kItemClasses[] = {
			{ REL::ID(430826), "PackinWorkshopItem" },        // +0x4C254A0
			{ REL::ID(430834), "DecoratedHabWorkshopItem" },  // +0x4C25710
			{ REL::ID(430818), "SingleWorkshopItem" },        // +0x4C25298
		};
		constexpr std::uintptr_t kItemPlacedSourceRva = 0xF4F840;
		constexpr std::size_t kCommitSlot = 0xA0 / 8;

		std::uintptr_t ImageBase()
		{
			return kItemPlacedSource.address() - kItemPlacedSourceRva;
		}

		std::string Stack(unsigned a_skip = 2)
		{
			void* frames[40]{};
			const auto n = RtlCaptureStackBackTrace(a_skip, 40, frames, nullptr);
			const auto base = ImageBase();
			std::string s;
			for (unsigned i = 0; i < n; ++i) {
				const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
				s += (a >= base && a < base + 0x10000000) ? std::format(" {:X}", a - base) : std::string(" ?");
			}
			return s;
		}

		std::string Ref(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return "none";
			}
			auto base = a_ref->GetBaseObject();
			const auto p = a_ref->GetPosition();
			const auto r = a_ref->GetAngle();
			return std::format("{:08X} base {:08X} type {} pos {:.2f} {:.2f} {:.2f} ang {:.3f} {:.3f} {:.3f}", a_ref->GetFormID(),
				base ? base->GetFormID() : 0, base ? std::to_underlying(base->GetFormType()) : 0, p.x, p.y, p.z, r.x, r.y, r.z);
		}

		bool Readable(std::uintptr_t a_addr, std::size_t a_size)
		{
			return a_addr > 0x10000 && !IsBadReadPtr(reinterpret_cast<const void*>(a_addr), a_size);
		}

		// "form XXXXXXXX type T" if a_ptr points at a live TESForm, "ref ..." for a reference, else "".
		std::string Describe(std::uintptr_t a_ptr)
		{
			const auto base = ImageBase();
			if (!Readable(a_ptr, 0x40)) {
				return {};
			}
			const auto vt = *reinterpret_cast<std::uintptr_t*>(a_ptr);
			if (vt < base || vt > base + 0x10000000) {
				return {};
			}
			auto* form = reinterpret_cast<RE::TESForm*>(a_ptr);
			const auto id = form->GetFormID();
			if (id == 0 || RE::TESForm::LookupByID(id) != form) {
				return std::format("vt {:X}", vt - base);
			}
			if (auto* ref = form->AsReference()) {
				return "ref " + Ref(ref);
			}
			return std::format("form {:08X} type {}", id, std::to_underlying(form->GetFormType()));
		}

		void Dump(const char* a_what, std::uintptr_t a_obj, std::size_t a_size)
		{
			if (!Readable(a_obj, a_size)) {
				REX::INFO("diag: {} {:X} not readable", a_what, a_obj);
				return;
			}
			REX::INFO("diag: {} at {:X} (vt {:X})", a_what, a_obj, *reinterpret_cast<std::uintptr_t*>(a_obj) - ImageBase());
			for (std::size_t o = 0; o < a_size; o += 8) {
				const auto q = *reinterpret_cast<std::uintptr_t*>(a_obj + o);
				if (q == 0) {
					continue;
				}
				const float f0 = *reinterpret_cast<float*>(a_obj + o), f1 = *reinterpret_cast<float*>(a_obj + o + 4);
				const auto d = Describe(q);
				REX::INFO("diag:   +{:03X} {:016X}  f {:.3f} {:.3f}  {}", o, q, f0, f1, d);
			}
		}

		using Commit_t = std::uintptr_t (*)(void* a_item, void* a_engine, std::uintptr_t a_3, std::uintptr_t a_4);
		Commit_t g_commit[3]{};

		template <std::size_t I>
		std::uintptr_t CommitHook(void* a_item, void* a_engine, std::uintptr_t a_3, std::uintptr_t a_4)
		{
			REX::INFO("diag: === {} commit item {} engine {}", kItemClasses[I].name, a_item, a_engine);
			REX::INFO("diag:   stack{}", Stack());
			Dump("item", reinterpret_cast<std::uintptr_t>(a_item), 0xF0);
			Dump("engine", reinterpret_cast<std::uintptr_t>(a_engine), 0x700);
			const auto r = g_commit[I](a_item, a_engine, a_3, a_4);
			REX::INFO("diag: === commit returned {:X}", r);
			Dump("item after", reinterpret_cast<std::uintptr_t>(a_item), 0xF0);
			return r;
		}

		template <std::size_t I>
		void HookCommit()
		{
			auto* slot = reinterpret_cast<std::uintptr_t*>(kItemClasses[I].vtbl.address()) + kCommitSlot;
			unsigned long old = 0;
			if (!VirtualProtect(slot, sizeof(*slot), 0x04 /*PAGE_READWRITE*/, &old)) {
				REX::WARN("diag: VirtualProtect failed");
				return;
			}
			g_commit[I] = reinterpret_cast<Commit_t>(*slot);
			*slot = reinterpret_cast<std::uintptr_t>(&CommitHook<I>);
			VirtualProtect(slot, sizeof(*slot), old, &old);
			REX::INFO("diag: hooked {} commit (was +{:X})", kItemClasses[I].name, reinterpret_cast<std::uintptr_t>(g_commit[I]) - ImageBase());
		}

		class ItemPlacedSink : public RE::BSTEventSink<RE::Workshop::ItemPlacedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemPlacedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>*) override
			{
				REX::INFO("diag: ItemPlaced unk00={} item {} workshop {:08X} source {:08X}", a_event.unk00, Ref(a_event.item.get()),
					a_event.workshop ? a_event.workshop->GetFormID() : 0, a_event.source ? a_event.source->GetFormID() : 0);
				REX::INFO("diag:   stack{}", Stack());
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class WorkshopItemPlacedSink : public RE::BSTEventSink<RE::Workshop::WorkshopItemPlacedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::WorkshopItemPlacedEvent& a_event, RE::BSTEventSource<RE::Workshop::WorkshopItemPlacedEvent>*) override
			{
				REX::INFO("diag: WorkshopItemPlaced workshop {:08X}", a_event.workshop ? a_event.workshop->GetFormID() : 0);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Workshop::PlacementStatusEvent: layout unknown, log the first 0x30 bytes when they change.
		struct PlacementStatusEvent
		{
			std::byte raw[0x30];
		};

		class PlacementStatusSink : public RE::BSTEventSink<PlacementStatusEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const PlacementStatusEvent& a_event, RE::BSTEventSource<PlacementStatusEvent>*) override
			{
				const auto addr = reinterpret_cast<std::uintptr_t>(&a_event);
				if (!Readable(addr, sizeof(a_event))) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::string hex;
				for (std::size_t i = 0; i < sizeof(a_event.raw); i += 4) {
					hex += std::format(" {:08X}", *reinterpret_cast<const std::uint32_t*>(a_event.raw + i));
				}
				const auto stack = Stack();
				if (hex != last || stack != lastStack) {
					REX::INFO("diag: PlacementStatus{}", hex);
					REX::INFO("diag:   stack{}", stack);
					last = hex;
					lastStack = stack;
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			std::string last, lastStack;
		};

		// Background sampler: while the engine holds an item, log engine/item qwords that changed and the
		// positions of the first preview refs, every 0.5 s. Moving the cursor shows where the transform lives.
		constexpr std::uintptr_t kEngineRva = 0x5F5D2E0;

		void Sampler()
		{
			std::vector<std::uint64_t> prevEngine(0x700 / 8), prevItem(0xF0 / 8);
			std::uintptr_t prevItemAddr = 0;
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				const auto engine = ImageBase() + kEngineRva;
				if (!Readable(engine, 0x700)) {
					continue;
				}
				const auto item = *reinterpret_cast<std::uintptr_t*>(engine + 0x268);
				if (!item || !Readable(item, 0xF0)) {
					prevItemAddr = 0;
					continue;
				}
				std::string changed;
				for (std::size_t i = 0; i < prevEngine.size(); ++i) {
					const auto q = *reinterpret_cast<std::uint64_t*>(engine + i * 8);
					if (q != prevEngine[i] && prevItemAddr == item) {
						const float f0 = *reinterpret_cast<float*>(engine + i * 8), f1 = *reinterpret_cast<float*>(engine + i * 8 + 4);
						changed += std::format(" e+{:X}={:.2f},{:.2f}", i * 8, f0, f1);
					}
					prevEngine[i] = q;
				}
				for (std::size_t i = 0; i < prevItem.size(); ++i) {
					const auto q = *reinterpret_cast<std::uint64_t*>(item + i * 8);
					if (q != prevItem[i] && prevItemAddr == item) {
						changed += std::format(" i+{:X}={:X}", i * 8, q);
					}
					prevItem[i] = q;
				}
				// preview refs: item+0x08 = {u32 size, u32 cap}, item+0x10 = data
				const auto n = *reinterpret_cast<std::uint32_t*>(item + 0x08);
				const auto data = *reinterpret_cast<std::uintptr_t*>(item + 0x10);
				std::string refs;
				if (n && n < 4096 && Readable(data, 8 * std::min<std::uint32_t>(n, 3))) {
					for (std::uint32_t k = 0; k < std::min<std::uint32_t>(n, 3); ++k) {
						const auto p = *reinterpret_cast<std::uintptr_t*>(data + k * 8);
						refs += " [" + Describe(p) + "]";
					}
				}
				if (prevItemAddr != item) {
					REX::INFO("diag: sampler: new item {:X} refs {}{}", item, n, refs);
				} else if (!changed.empty()) {
					REX::INFO("diag: sampler:{}", changed);
					REX::INFO("diag: sampler: refs {}{}", n, refs);
				}
				prevItemAddr = item;
			}
		}
	}

	void Install(const std::filesystem::path& a_flagFile)
	{
		std::error_code ec;
		if (!std::filesystem::exists(a_flagFile, ec)) {
			return;
		}
		using ItemSrc = RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>* (*)();
		using WsSrc = RE::BSTEventSource<RE::Workshop::WorkshopItemPlacedEvent>* (*)();
		auto* items = REL::Relocation<ItemSrc>{ kItemPlacedSource }();
		auto* ws = REL::Relocation<WsSrc>{ kWorkshopItemPlacedSource }();
		static ItemPlacedSink itemSink;
		static WorkshopItemPlacedSink wsSink;
		if (items) {
			items->RegisterSink(&itemSink);
		}
		if (ws) {
			ws->RegisterSink(&wsSink);
		}
		HookCommit<0>();
		HookCommit<1>();
		HookCommit<2>();

		using StatusSrc = RE::BSTEventSource<PlacementStatusEvent>* (*)();
		auto* status = REL::Relocation<StatusSrc>{ REL::ID(73955) }();   // +0xF4F340
		static PlacementStatusSink statusSink;
		if (status) {
			status->RegisterSink(&statusSink);
		}
		std::thread(Sampler).detach();
		REX::INFO("diag: workshop placement logging on");
	}
}
