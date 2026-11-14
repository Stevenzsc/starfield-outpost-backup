// Diagnostics for reverse engineering workshop placement (enabled when OutpostBackup\diag.txt exists):
// - logs every Workshop::ItemPlacedEvent / WorkshopItemPlacedEvent with the item and the call stack;
// - hooks Workshop::PackinWorkshopItem::vfunc[0xA0] (commit) and dumps the item and the engine object,
//   naming every pointer that is a TESForm / TESObjectREFR.
// See docs/re-workshop.md.

#include "Diag.h"

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
		constexpr REL::ID kPackinItemVtbl{ 430826 };           // +0x4C254A0 Workshop::PackinWorkshopItem
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

		using Commit_t = std::uintptr_t (*)(void* a_item, void* a_engine);
		Commit_t g_commit = nullptr;

		std::uintptr_t CommitHook(void* a_item, void* a_engine)
		{
			REX::INFO("diag: === PackinWorkshopItem commit item {} engine {}", a_item, a_engine);
			REX::INFO("diag:   stack{}", Stack());
			Dump("item", reinterpret_cast<std::uintptr_t>(a_item), 0xF0);
			Dump("engine", reinterpret_cast<std::uintptr_t>(a_engine), 0x700);
			const auto r = g_commit(a_item, a_engine);
			REX::INFO("diag: === commit returned {:X}", r);
			Dump("item after", reinterpret_cast<std::uintptr_t>(a_item), 0xF0);
			return r;
		}

		void HookCommit()
		{
			auto* slot = reinterpret_cast<std::uintptr_t*>(kPackinItemVtbl.address()) + kCommitSlot;
			unsigned long old = 0;
			if (!VirtualProtect(slot, sizeof(*slot), 0x04 /*PAGE_READWRITE*/, &old)) {
				REX::WARN("diag: VirtualProtect failed");
				return;
			}
			g_commit = reinterpret_cast<Commit_t>(*slot);
			*slot = reinterpret_cast<std::uintptr_t>(&CommitHook);
			VirtualProtect(slot, sizeof(*slot), old, &old);
			REX::INFO("diag: hooked PackinWorkshopItem commit (was +{:X})", reinterpret_cast<std::uintptr_t>(g_commit) - ImageBase());
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
		HookCommit();
		REX::INFO("diag: workshop placement logging on");
	}
}
