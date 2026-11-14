// Outpost backup store and the Papyrus natives behind the in-game slate (script OSB_Native).
//
// Backup file: Documents\My Games\Starfield\OutpostBackup\latest.osb (+ a dated copy), tab separated:
//   OSB	1
//   planet	<hex form id>	<radius km>
//   origin	<lat deg>	<lon deg>          landing-zone center (BGSPlanet::Manager)
//   beacon	<x>	<y>	<z>	<yaw>
//   item	<plugin file>	<local id hex>	<x>	<y>	<z>	<ax>	<ay>	<az>	<scale>	<packin 0|1>
//
// Objects are stored as plugin file + local form ID, so a different load order still restores.

#include "Backup.h"

#include "REX/W32/OLE32.h"
#include "REX/W32/SHELL32.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <numbers>
#include <sstream>

namespace OSB
{
	namespace
	{
		// BGSPlanet::Manager (static object), see docs: +0x80 planet, +0x98 lon, +0xA0 lat (radians), +0x100 radius km
		constexpr REL::ID kPlanetManager{ 945580 };

		struct Item
		{
			std::string plugin;
			std::uint32_t local{ 0 };
			float x{}, y{}, z{}, ax{}, ay{}, az{}, scale{ 1.0f };
			bool  pack{ false };
		};

		struct Backup
		{
			std::uint32_t planet{ 0 };
			double radiusKm{ 0 }, lat{ 0 }, lon{ 0 };
			float bx{}, by{}, bz{}, byaw{};
			std::vector<Item> items;
		};

		std::mutex g_lock;
		Backup     g_pending;   // being written by BeginBackup/AddItem/EndBackup
		Backup     g_loaded;    // read by LoadRestore
		std::vector<std::size_t> g_restoreOrder;   // indices into g_loaded.items after filtering

		bool   g_armed = false;
		double g_armLat = 0, g_armLon = 0;

		std::filesystem::path Folder()
		{
			// SFSE's log folder is Documents\My Games\Starfield\SFSE\Logs; keep backups next to it.
			wchar_t* docs = nullptr;
			std::filesystem::path p;
			if (REX::W32::SHGetKnownFolderPath(REX::W32::FOLDERID_Documents, 0, nullptr, &docs) == 0 && docs) {
				p = std::filesystem::path(docs) / "My Games" / "Starfield" / "OutpostBackup";
			}
			if (docs) {
				REX::W32::CoTaskMemFree(docs);
			}
			if (p.empty()) {
				p = std::filesystem::current_path() / "OutpostBackup";
			}
			std::error_code ec;
			std::filesystem::create_directories(p, ec);
			return p;
		}

		struct PlanetState
		{
			std::uint32_t planet{ 0 };
			double lat{ 0 }, lon{ 0 }, radiusKm{ 0 };
		};

		PlanetState ReadPlanet()
		{
			const auto base = kPlanetManager.address();
			PlanetState s;
			s.planet = *reinterpret_cast<const std::uint32_t*>(base + 0x80);
			s.lon = *reinterpret_cast<const double*>(base + 0x98) * 180.0 / std::numbers::pi;
			s.lat = *reinterpret_cast<const double*>(base + 0xA0) * 180.0 / std::numbers::pi;
			s.radiusKm = *reinterpret_cast<const float*>(base + 0x100);
			return s;
		}

		// ---------- load order: runtime form id <-> plugin + local id ----------

		bool SplitFormID(std::uint32_t a_id, std::string& a_plugin, std::uint32_t& a_local)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return false;
			}
			auto& c = dh->compiledFileCollection;
			const std::uint32_t top = a_id >> 24;
			if (top == 0xFE) {
				const std::uint32_t idx = (a_id >> 12) & 0xFFF;
				for (std::uint32_t i = 0; i < c.smallFiles.size(); ++i) {
					if (auto* f = c.smallFiles[i]; f && f->fileIndex.smallIndex == idx) {
						a_plugin = f->fileName;
						a_local = a_id & 0xFFF;
						return true;
					}
				}
			} else if (top == 0xFD) {
				const std::uint32_t idx = (a_id >> 16) & 0xFF;
				for (std::uint32_t i = 0; i < c.mediumFiles.size(); ++i) {
					if (auto* f = c.mediumFiles[i]; f && f->fileIndex.mediumIndex == idx) {
						a_plugin = f->fileName;
						a_local = a_id & 0xFFFF;
						return true;
					}
				}
			} else {
				for (std::uint32_t i = 0; i < c.files.size(); ++i) {
					if (auto* f = c.files[i]; f && f->fileIndex.fullIndex == top) {
						a_plugin = f->fileName;
						a_local = a_id & 0xFFFFFF;
						return true;
					}
				}
			}
			return false;
		}

		bool SameName(const char* a, const std::string& b)
		{
			return _stricmp(a, b.c_str()) == 0;
		}

		RE::TESForm* FindForm(const std::string& a_plugin, std::uint32_t a_local)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return nullptr;
			}
			auto& c = dh->compiledFileCollection;
			for (std::uint32_t i = 0; i < c.files.size(); ++i) {
				if (auto* f = c.files[i]; f && SameName(f->fileName, a_plugin)) {
					return RE::TESForm::LookupByID((std::uint32_t(f->fileIndex.fullIndex) << 24) | (a_local & 0xFFFFFF));
				}
			}
			for (std::uint32_t i = 0; i < c.mediumFiles.size(); ++i) {
				if (auto* f = c.mediumFiles[i]; f && SameName(f->fileName, a_plugin)) {
					return RE::TESForm::LookupByID(0xFD000000 | (std::uint32_t(f->fileIndex.mediumIndex) << 16) | (a_local & 0xFFFF));
				}
			}
			for (std::uint32_t i = 0; i < c.smallFiles.size(); ++i) {
				if (auto* f = c.smallFiles[i]; f && SameName(f->fileName, a_plugin)) {
					return RE::TESForm::LookupByID(0xFE000000 | (std::uint32_t(f->fileIndex.smallIndex) << 12) | (a_local & 0xFFF));
				}
			}
			return nullptr;
		}

		// ---------- file io ----------

		bool Save(const Backup& b, std::filesystem::path& a_written)
		{
			const auto dir = Folder();
			const auto stamp = std::format("{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
			std::ostringstream o;
			o.imbue(std::locale::classic());
			o.precision(17);
			o << "OSB\t1\n";
			o << "planet\t" << std::format("{:08X}", b.planet) << '\t' << b.radiusKm << '\n';
			o << "origin\t" << b.lat << '\t' << b.lon << '\n';
			o << "beacon\t" << b.bx << '\t' << b.by << '\t' << b.bz << '\t' << b.byaw << '\n';
			for (const auto& it : b.items) {
				o << "item\t" << it.plugin << '\t' << std::format("{:X}", it.local) << '\t' << it.x << '\t' << it.y << '\t' << it.z << '\t'
				  << it.ax << '\t' << it.ay << '\t' << it.az << '\t' << it.scale << '\t' << (it.pack ? 1 : 0) << '\n';
			}
			const auto text = o.str();
			for (const auto& name : { std::string("latest.osb"), "outpost-" + stamp + ".osb" }) {
				std::ofstream f(dir / name, std::ios::binary | std::ios::trunc);
				if (!f) {
					return false;
				}
				f << text;
			}
			a_written = dir / "latest.osb";
			return true;
		}

		bool Load(Backup& b)
		{
			std::ifstream f(Folder() / "latest.osb", std::ios::binary);
			if (!f) {
				return false;
			}
			b = {};
			std::string line;
			while (std::getline(f, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				std::vector<std::string> t;
				std::size_t start = 0;
				for (std::size_t i = 0; i <= line.size(); ++i) {
					if (i == line.size() || line[i] == '\t') {
						t.emplace_back(line.substr(start, i - start));
						start = i + 1;
					}
				}
				auto num = [&](std::size_t i) { return i < t.size() ? std::stod(t[i]) : 0.0; };
				try {
					if (t[0] == "planet" && t.size() >= 3) {
						b.planet = std::stoul(t[1], nullptr, 16);
						b.radiusKm = num(2);
					} else if (t[0] == "origin" && t.size() >= 3) {
						b.lat = num(1);
						b.lon = num(2);
					} else if (t[0] == "beacon" && t.size() >= 5) {
						b.bx = float(num(1)); b.by = float(num(2)); b.bz = float(num(3)); b.byaw = float(num(4));
					} else if (t[0] == "item" && t.size() >= 11) {
						Item it;
						it.plugin = t[1];
						it.local = std::stoul(t[2], nullptr, 16);
						it.x = float(num(3)); it.y = float(num(4)); it.z = float(num(5));
						it.ax = float(num(6)); it.ay = float(num(7)); it.az = float(num(8));
						it.scale = float(num(9));
						it.pack = t[10] == "1";
						b.items.push_back(std::move(it));
					}
				} catch (...) {
					REX::WARN("backup: bad line: {}", line);
				}
			}
			return b.planet != 0;
		}

		// ---------- natives ----------

		bool BeginBackup(std::monostate, float a_bx, float a_by, float a_bz, float a_byaw)
		{
			std::scoped_lock l(g_lock);
			const auto s = ReadPlanet();
			g_pending = {};
			if (s.planet == 0 || s.radiusKm <= 0) {
				REX::WARN("backup: not on a planet surface");
				return false;
			}
			g_pending.planet = s.planet;
			g_pending.radiusKm = s.radiusKm;
			g_pending.lat = s.lat;
			g_pending.lon = s.lon;
			g_pending.bx = a_bx; g_pending.by = a_by; g_pending.bz = a_bz; g_pending.byaw = a_byaw;
			return true;
		}

		bool AddItem(std::monostate, RE::TESForm* a_base, float x, float y, float z, float ax, float ay, float az, float scale, bool a_pack)
		{
			if (!a_base) {
				return false;
			}
			const auto id = a_base->GetFormID();
			std::string plugin;
			std::uint32_t local = 0;
			if (!SplitFormID(id, plugin, local)) {
				REX::WARN("backup: no plugin for form {:08X}", id);
				return false;
			}
			if (id < 0x800) {   // engine-reserved forms (markers)
				return false;
			}
			std::scoped_lock l(g_lock);
			g_pending.items.push_back({ plugin, local, x, y, z, ax, ay, az, scale, a_pack });
			return true;
		}

		std::int32_t EndBackup(std::monostate)
		{
			std::scoped_lock l(g_lock);
			if (g_pending.planet == 0) {
				return -1;
			}
			std::filesystem::path written;
			if (!Save(g_pending, written)) {
				REX::ERROR("backup: could not write file");
				return -1;
			}
			REX::INFO("backup: {} objects, planet {:08X}, center {:.6f} {:.6f} -> {}", g_pending.items.size(), g_pending.planet, g_pending.lat, g_pending.lon, written.string());
			return std::int32_t(g_pending.items.size());
		}

		bool HasBackup(std::monostate)
		{
			return std::filesystem::exists(Folder() / "latest.osb");
		}

		bool ArmLanding(std::monostate)
		{
			Backup b;
			if (!Load(b)) {
				return false;
			}
			std::scoped_lock l(g_lock);
			g_armed = true;
			g_armLat = b.lat;
			g_armLon = b.lon;
			REX::INFO("landing armed: {:.6f} {:.6f}", b.lat, b.lon);
			return true;
		}

		// Old beacon spot in the current landing zone: [x, y, z, distance from the backup's center in m].
		// Empty if there is no backup or you are on another planet.
		std::vector<float> GetGuideSpot(std::monostate)
		{
			Backup b;
			if (!Load(b)) {
				return {};
			}
			const auto s = ReadPlanet();
			if (s.planet != b.planet || s.radiusKm <= 0) {
				return {};
			}
			const double R = s.radiusKm * 1000.0, toRad = std::numbers::pi / 180.0;
			double dLon = b.lon - s.lon;
			if (dLon > 180) dLon -= 360;
			if (dLon < -180) dLon += 360;
			const double east = dLon * toRad * R * std::cos((b.lat + s.lat) / 2 * toRad);
			const double north = (b.lat - s.lat) * toRad * R;
			return { float(b.bx + east), float(b.by + north), b.bz, float(std::sqrt(east * east + north * north)) };
		}

		// Prepare a restore: returns the number of objects to place (PackIn parts and co-located duplicates skipped).
		std::int32_t LoadRestore(std::monostate)
		{
			std::scoped_lock l(g_lock);
			g_restoreOrder.clear();
			if (!Load(g_loaded)) {
				return -1;
			}
			struct Group { float x, y, z; bool pack; };
			std::vector<Group> groups;
			for (std::size_t i = 0; i < g_loaded.items.size(); ++i) {
				const auto& it = g_loaded.items[i];
				const Group* parent = nullptr;
				for (const auto& g : groups) {
					if (std::abs(g.x - it.x) < 0.01f && std::abs(g.y - it.y) < 0.01f && std::abs(g.z - it.z) < 0.2f) {
						parent = &g;
						break;
					}
				}
				if (parent && parent->pack) {
					continue;   // spawned again by its PackIn parent
				}
				if (!parent) {
					groups.push_back({ it.x, it.y, it.z, it.pack });
				}
				g_restoreOrder.push_back(i);
			}
			return std::int32_t(g_restoreOrder.size());
		}

		RE::TESForm* GetRestoreForm(std::monostate, std::int32_t i)
		{
			std::scoped_lock l(g_lock);
			if (i < 0 || std::size_t(i) >= g_restoreOrder.size()) {
				return nullptr;
			}
			const auto& it = g_loaded.items[g_restoreOrder[i]];
			auto* form = FindForm(it.plugin, it.local);
			if (!form) {
				REX::WARN("restore: {} {:X} not found (plugin missing?)", it.plugin, it.local);
			}
			return form;
		}

		// [x, y, z, ax, ay, az, scale]
		std::vector<float> GetRestoreXform(std::monostate, std::int32_t i)
		{
			std::scoped_lock l(g_lock);
			if (i < 0 || std::size_t(i) >= g_restoreOrder.size()) {
				return {};
			}
			const auto& it = g_loaded.items[g_restoreOrder[i]];
			return { it.x, it.y, it.z, it.ax, it.ay, it.az, it.scale };
		}

		std::string GetRestoreName(std::monostate, std::int32_t i)
		{
			std::scoped_lock l(g_lock);
			if (i < 0 || std::size_t(i) >= g_restoreOrder.size()) {
				return {};
			}
			const auto& it = g_loaded.items[g_restoreOrder[i]];
			return std::format("{} {:X}", it.plugin, it.local);
		}
	}

	bool TakeArmedLanding(double& a_latDeg, double& a_lonDeg)
	{
		std::scoped_lock l(g_lock);
		if (!g_armed) {
			return false;
		}
		g_armed = false;
		a_latDeg = g_armLat;
		a_lonDeg = g_armLon;
		return true;
	}

	void RegisterNatives()
	{
		auto* gvm = RE::GameVM::GetSingleton();
		auto* vm = gvm ? gvm->GetVM() : nullptr;
		if (!vm) {
			REX::ERROR("papyrus VM not available; OSB_Native not registered");
			return;
		}
		constexpr std::string_view obj = "OSB_Native";
		vm->BindNativeMethod(obj, "BeginBackup", BeginBackup, true, false);
		vm->BindNativeMethod(obj, "AddItem", AddItem, true, false);
		vm->BindNativeMethod(obj, "EndBackup", EndBackup, true, false);
		vm->BindNativeMethod(obj, "HasBackup", HasBackup, true, false);
		vm->BindNativeMethod(obj, "ArmLanding", ArmLanding, true, false);
		vm->BindNativeMethod(obj, "GetGuideSpot", GetGuideSpot, true, false);
		vm->BindNativeMethod(obj, "LoadRestore", LoadRestore, true, false);
		vm->BindNativeMethod(obj, "GetRestoreForm", GetRestoreForm, true, false);
		vm->BindNativeMethod(obj, "GetRestoreXform", GetRestoreXform, true, false);
		vm->BindNativeMethod(obj, "GetRestoreName", GetRestoreName, true, false);
		REX::INFO("papyrus: OSB_Native registered");
	}
}
