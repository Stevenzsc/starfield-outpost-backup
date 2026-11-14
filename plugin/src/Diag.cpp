// Diagnostics for reverse engineering workshop placement: logs every Workshop::ItemPlacedEvent and
// WorkshopItemPlacedEvent with the item and the call stack (as Starfield.exe offsets), so the function the
// game uses to build an object can be found. Enabled when Documents\My Games\Starfield\OutpostBackup\diag.txt exists.

#include "Diag.h"

extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void**, unsigned long*);

namespace OSB::Diag
{
	namespace
	{
		// Address Library 1.16.244: event source getters (found via the BSTEventSource vtables)
		constexpr REL::ID kItemPlacedSource{ 73963 };          // Starfield.exe+0xF4F840
		constexpr REL::ID kWorkshopItemPlacedSource{ 73962 };  // Starfield.exe+0xF4F7A0
		constexpr std::uintptr_t kItemPlacedSourceRva = 0xF4F840;

		std::uintptr_t ImageBase()
		{
			return kItemPlacedSource.address() - kItemPlacedSourceRva;
		}

		std::string Stack()
		{
			void* frames[40]{};
			const auto n = RtlCaptureStackBackTrace(2, 40, frames, nullptr);
			const auto base = ImageBase();
			std::string s;
			for (unsigned i = 0; i < n; ++i) {
				const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
				if (a >= base && a < base + 0x10000000) {
					s += std::format(" {:X}", a - base);
				} else {
					s += " ?";
				}
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

		class ItemPlacedSink : public RE::BSTEventSink<RE::Workshop::ItemPlacedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::Workshop::ItemPlacedEvent& a_event, RE::BSTEventSource<RE::Workshop::ItemPlacedEvent>*) override
			{
				REX::INFO("diag: ItemPlaced unk00={} item {} workshop {} source {:08X}", a_event.unk00, Ref(a_event.item.get()),
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
				REX::INFO("diag:   stack{}", Stack());
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
		REX::INFO("diag: workshop placement logging on (sources {} {})", static_cast<void*>(items), static_cast<void*>(ws));
	}
}
