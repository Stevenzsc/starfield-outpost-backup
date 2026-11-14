// OSBLanding - land exactly on a stored latitude/longitude.
//
// When you land from the star map, the game calls
//   LandAtLatLon   (Address Library ID 51739)  ->  SetLandingSite (ID 51740)
// with a site struct whose first two doubles are longitude and latitude in radians.
// This plugin replaces the call at LandAtLatLon+0x83 (game 1.16.244). If the file
// <Starfield>\OSB_LandTarget.txt exists, its coordinates replace the clicked ones, once:
//   line 1: <latitude degrees> <longitude degrees>
// After use the file is renamed to OSB_LandTarget.used.txt. Every landing is logged to
// Documents\My Games\Starfield\SFSE\Logs\OSBLanding.log.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>

namespace
{
	constexpr REL::ID      kLandAtLatLon{ 51739 };
	constexpr std::ptrdiff_t kCallOffset = 0x83;  // call SetLandingSite inside LandAtLatLon

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

	std::uintptr_t SetLandingSite_Hook(void* a_manager, void* a_out, std::uint32_t a_planet, double* a_site)
	{
		constexpr double toDeg = 180.0 / std::numbers::pi;
		if (a_site) {
			REX::INFO("landing: planet arg 0x{:08X}, clicked lat {:.6f} lon {:.6f}",
				a_planet, a_site[1] * toDeg, a_site[0] * toDeg);

			double lat = 0.0, lon = 0.0;
			if (ReadTarget(lat, lon)) {
				a_site[0] = lon / toDeg;
				a_site[1] = lat / toDeg;
				REX::INFO("landing: OVERRIDE -> lat {:.6f} lon {:.6f}", lat, lon);

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

	void Install()
	{
		const auto src = kLandAtLatLon.address() + kCallOffset;
		if (*reinterpret_cast<const std::uint8_t*>(src) != 0xE8) {
			REX::ERROR("expected a call at LandAtLatLon+0x{:X}, found 0x{:02X}; game version not supported, hook not installed",
				kCallOffset, *reinterpret_cast<const std::uint8_t*>(src));
			return;
		}
		auto& trampoline = REL::GetTrampoline();
		g_original = reinterpret_cast<SetLandingSite_t>(trampoline.write_call<5>(src, SetLandingSite_Hook));
		REX::INFO("hook installed at Starfield.exe+0x{:X}", kLandAtLatLon.offset() + kCallOffset);
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = 64 });
	Install();
	return true;
}
