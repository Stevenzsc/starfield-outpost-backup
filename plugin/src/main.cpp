// OSBLanding - land exactly on a stored latitude/longitude.
//
// Landing goes through SetLandingSite (Address Library ID 51740) with a site struct whose first two doubles are longitude and latitude in radians.
// This plugin replaces all four calls to SetLandingSite (game 1.16.244). If the file
// <Starfield>\OSB_LandTarget.txt exists, its coordinates replace the clicked ones, once:
//   line 1: <latitude degrees> <longitude degrees>
// After use the file is renamed to OSB_LandTarget.used.txt. Every landing is logged to
// Documents\My Games\Starfield\SFSE\Logs\OSBLanding.log.

#include "Backup.h"
#include "Diag.h"

#include <cmath>
#include <format>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>

namespace
{
	// Every call to SetLandingSite (ID 51740) in game 1.16.244: (function ID, offset of the call)
	// Observed in game: loading a save that is on a planet goes through LandAtLatLon (restores the saved
	// landing zone); a star-map click landing goes through StarMapLanding (97897). Only that one is overridden.
	struct CallSite
	{
		std::uint64_t  id;
		std::ptrdiff_t offset;
		const char*    name;
		bool           override;
	};
	constexpr CallSite kSites[] = {
		{ 51739, 0x83, "LandAtLatLon (save load)", false },
		{ 97897, 0x1EB, "StarMapLanding", true },
		{ 97898, 0x2D2, "Land97898", false },
		{ 97899, 0x170, "LandAtRef", false },
	};

	using SetLandingSite_t = std::uintptr_t (*)(void* a_manager, void* a_out, std::uint32_t a_planet, double* a_site);
	SetLandingSite_t g_original = nullptr;

	std::filesystem::path TargetFile()
	{
		return std::filesystem::current_path() / "OSB_LandTarget.txt";
	}

	bool ReadTarget(double& a_latDeg, double& a_lonDeg)
	{
		std::ifstream in(TargetFile());
		if (!in) {
			return false;
		}
		std::string line;
		std::getline(in, line);
		std::istringstream ss(line);
		ss.imbue(std::locale::classic());
		return static_cast<bool>(ss >> a_latDeg >> a_lonDeg) &&
		       std::abs(a_latDeg) <= 90.0 && std::abs(a_lonDeg) <= 360.0;
	}

	template <std::size_t I>
	std::uintptr_t SetLandingSite_Hook(void* a_manager, void* a_out, std::uint32_t a_planet, double* a_site)
	{
		constexpr double toDeg = 180.0 / std::numbers::pi;
		if (a_site) {
			REX::INFO("landing via {}: planet arg 0x{:08X}, requested lat {:.6f} lon {:.6f}",
				kSites[I].name, a_planet, a_site[1] * toDeg, a_site[0] * toDeg);

			double lat = 0.0, lon = 0.0;
			if (kSites[I].override && OSB::TakeArmedLanding(a_planet, lat, lon)) {
				// armed from the in-game slate
				a_site[0] = lon / toDeg;
				a_site[1] = lat / toDeg;
				REX::INFO("landing: OVERRIDE (slate) -> lat {:.6f} lon {:.6f}", lat, lon);
			} else if (kSites[I].override && ReadTarget(lat, lon)) {
				// armed by tools\arm-landing.ps1
				a_site[0] = lon / toDeg;
				a_site[1] = lat / toDeg;
				REX::INFO("landing: OVERRIDE (file) -> lat {:.6f} lon {:.6f}", lat, lon);

				std::error_code ec;
				auto used = TargetFile();
				used.replace_filename("OSB_LandTarget.used.txt");
				std::filesystem::remove(used, ec);
				std::filesystem::rename(TargetFile(), used, ec);
				if (ec) {
					REX::WARN("landing: could not rename target file: {}", ec.message());
				}
			}
		}
		return g_original(a_manager, a_out, a_planet, a_site);
	}

	// Write the real load order (from TESDataHandler) to <Starfield>\OSB_LoadOrder.txt, so backups can
	// store "plugin name + local form ID" instead of load-order-dependent form IDs.
	//   FULL   <hex 2> <file>   -> form IDs XX______
	//   MEDIUM <hex 2> <file>   -> form IDs FDXX____
	//   SMALL  <hex 3> <file>   -> form IDs FEXXX___
	void DumpLoadOrder()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			REX::WARN("load order: TESDataHandler not available");
			return;
		}
		const auto path = std::filesystem::current_path() / "OSB_LoadOrder.txt";
		std::ofstream out(path, std::ios::trunc);
		if (!out) {
			REX::WARN("load order: cannot write {}", path.string());
			return;
		}
		auto& c = dh->compiledFileCollection;
		std::size_t n = 0;
		auto put = [&](const char* kind, const std::string& idx, const RE::TESFile* f) {
			if (f) {
				out << kind << ' ' << idx << ' ' << f->fileName << '\n';
				++n;
			}
		};
		for (std::uint32_t i = 0; i < c.files.size(); ++i) {
			const auto* f = c.files[i];
			if (f) put("FULL", std::format("{:02X}", f->fileIndex.fullIndex), f);
		}
		for (std::uint32_t i = 0; i < c.mediumFiles.size(); ++i) {
			const auto* f = c.mediumFiles[i];
			if (f) put("MEDIUM", std::format("{:02X}", f->fileIndex.mediumIndex), f);
		}
		for (std::uint32_t i = 0; i < c.smallFiles.size(); ++i) {
			const auto* f = c.smallFiles[i];
			if (f) put("SMALL", std::format("{:03X}", f->fileIndex.smallIndex), f);
		}
		REX::INFO("load order: wrote {} plugins to {}", n, path.string());
	}

	void OnMessage(SFSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg && a_msg->type == SFSE::MessagingInterface::kPostDataLoad) {
			DumpLoadOrder();
			OSB::RegisterNatives();
			OSB::Diag::Install(OSB::DataFolder() / "diag.txt");
		}
	}

	template <std::size_t I>
	void InstallOne()
	{
		const REL::ID id{ kSites[I].id };
		const auto    src = id.address() + kSites[I].offset;
		const auto    op = *reinterpret_cast<const std::uint8_t*>(src);
		if (op != 0xE8) {
			REX::ERROR("{}: expected a call at +0x{:X}, found 0x{:02X}; not hooked", kSites[I].name, kSites[I].offset, op);
			return;
		}
		const auto prev = REL::GetTrampoline().write_call<5>(src, SetLandingSite_Hook<I>);
		if (!g_original) {
			g_original = reinterpret_cast<SetLandingSite_t>(prev);
		} else if (reinterpret_cast<SetLandingSite_t>(prev) != g_original) {
			REX::WARN("{}: call target differs from the first site", kSites[I].name);
		}
		REX::INFO("hooked {} at Starfield.exe+0x{:X}", kSites[I].name, id.offset() + kSites[I].offset);
	}

	void Install()
	{
		[]<std::size_t... I>(std::index_sequence<I...>) {
			(InstallOne<I>(), ...);
		}(std::make_index_sequence<std::size(kSites)>{});
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = 128 });
	Install();
	if (const auto* msg = SFSE::GetMessagingInterface(); !msg || !msg->RegisterListener(OnMessage)) {
		REX::WARN("could not register for SFSE messages; OSB_LoadOrder.txt will not be written");
	}
	return true;
}
