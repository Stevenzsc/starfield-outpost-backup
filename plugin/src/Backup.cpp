// Outpost backup store and the Papyrus natives behind the in-game slate (script OSB_Native).
//
// One file per outpost in Documents\My Games\Starfield\OutpostBackup\Outposts\*.osb, tab separated:
//   OSB	1
//   name	<outpost name>
//   planetname	<planet name>
//   time	<unix seconds>
//   planet	<hex form id>	<radius km>
//   origin	<lat deg>	<lon deg>          landing-zone center (BGSPlanet::Manager)
//   beacon	<x>	<y>	<z>	<yaw>
//   item	<plugin file>	<local id hex>	<x>	<y>	<z>	<ax>	<ay>	<az>	<scale>	<packin 0|1>
//
// Backing up the same outpost again (same planet, beacon within 50 m) replaces its file.
// The newest backup is also copied to OutpostBackup\latest.osb for the console tools.
// Deleted backups are moved to OutpostBackup\Deleted.
//
// Objects are stored as plugin file + local form ID, so a different load order still restores.

#include "Backup.h"
#include "Builder.h"

#include "REX/W32/OLE32.h"
#include "REX/W32/SHELL32.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <mutex>
#include <numbers>
#include <sstream>
#include <unordered_set>

extern "C" __declspec(dllimport) int __stdcall IsBadReadPtr(const void*, std::uintptr_t);

namespace OSB
{
	namespace
	{
		// BGSPlanet::Manager (static object), see docs: +0x80 planet, +0x98 lon, +0xA0 lat (radians), +0x100 radius km
		constexpr REL::ID kPlanetManager{ 945580 };

		constexpr std::int32_t kVersion = 6;

		// OSB_PickMsg layout (see tools\esp): button 0 Cancel, 1 More..., 2..9 entries
		constexpr std::size_t kPickEntries = 8;
		constexpr std::size_t kPickButtons = 2 + kPickEntries;

		constexpr double kSameOutpostMeters = 50.0;

		struct Item
		{
			std::string plugin;
			std::uint32_t local{ 0 };
			float x{}, y{}, z{}, ax{}, ay{}, az{}, scale{ 1.0f };
			bool  pack{ false };
			std::uint32_t ref{ 0 };   // the object's reference form ID when backed up (FF...: rises in build order)
		};

		struct Backup
		{
			std::string name, planetName, systemName;
			std::string planetLocPlugin;          // the planet's Location form (for the star-map marker)
			std::uint32_t planetLocLocal{ 0 };
			std::int64_t time{ 0 };
			std::uint32_t planet{ 0 };
			double radiusKm{ 0 }, lat{ 0 }, lon{ 0 };
			float bx{}, by{}, bz{}, byaw{};
			std::vector<Item> items;
		};

		// A saved outpost as listed in the picker (items not loaded).
		struct Entry
		{
			std::filesystem::path path;
			Backup head;
			std::size_t count{ 0 };
		};

		std::mutex g_lock;
		Backup     g_pending;     // being written by BeginBackup/AddItem/EndBackup
		bool       g_replaced = false;
		std::size_t g_skipped = 0;   // objects left out of the current backup (not buildable)
		Backup     g_loaded;      // read by LoadRestore
		std::vector<std::size_t> g_restoreOrder;   // indices into g_loaded.items after filtering
		// PackIns with no recipe of their own: vanilla habs are menu variants (e.g. OpsHabFourWall02), hab walls
		// are not in the menu at all. AutoBuild asks the build menu (only filled in build mode) which are which.
		std::vector<std::size_t> g_menuCandidates;

		std::vector<Entry> g_list;       // last ListBackups result
		std::int32_t       g_page = 0;
		bool               g_pageHasMore = false;
		std::filesystem::path g_selected;

		std::vector<RE::BGSMessage::MESSAGEBOX_BUTTON*> g_pickButtons;   // original OSB_PickMsg buttons

		bool          g_armed = false;
		std::uint32_t g_armPlanet = 0;
		double        g_armLat = 0, g_armLon = 0;

		std::int32_t  g_flow = 0;   // id of the running automatic land-and-restore flow

		bool          g_busy = false;
		std::chrono::steady_clock::time_point g_busySince;

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

		std::filesystem::path SubFolder(const char* a_name)
		{
			auto p = Folder() / a_name;
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

		std::string Clean(std::string a_text)
		{
			for (auto& c : a_text) {
				if (c == '\t' || c == '\r' || c == '\n') {
					c = ' ';
				}
			}
			return a_text;
		}

		std::string FormName(RE::TESForm* a_form)
		{
			if (!a_form) {
				return {};
			}
			if (auto* full = starfield_cast<RE::TESFullName*>(a_form)) {
				if (const char* n = full->GetFullName(); n && *n) {
					return Clean(n);
				}
			}
			return {};
		}

		// Beacon position as planet lat/lon (degrees), for telling outposts apart across landing zones.
		void BeaconLatLon(const Backup& b, double& a_lat, double& a_lon)
		{
			const double R = b.radiusKm * 1000.0, toDeg = 180.0 / std::numbers::pi;
			a_lat = b.lat + b.by / R * toDeg;
			const double c = std::max(0.01, std::cos(b.lat / toDeg));
			a_lon = b.lon + b.bx / (R * c) * toDeg;
		}

		double BeaconDistance(const Backup& a, const Backup& b)
		{
			double la1, lo1, la2, lo2;
			BeaconLatLon(a, la1, lo1);
			BeaconLatLon(b, la2, lo2);
			const double R = a.radiusKm * 1000.0, toRad = std::numbers::pi / 180.0;
			double dLon = lo2 - lo1;
			if (dLon > 180) dLon -= 360;
			if (dLon < -180) dLon += 360;
			const double east = dLon * toRad * R * std::cos((la1 + la2) / 2 * toRad);
			const double north = (la2 - la1) * toRad * R;
			return std::sqrt(east * east + north * north);
		}

		bool Readable(std::uintptr_t a_addr, std::size_t a_size)
		{
			return a_addr > 0x10000 && !IsBadReadPtr(reinterpret_cast<const void*>(a_addr), a_size);
		}

		// ---------- buildable objects ----------

		// Form IDs that a workshop recipe (COBJ) creates. Outpost refs also include parts the game spawns for
		// habs (acoustic spaces, occlusion, lights, triggers); those must not be placed by script (EmptyTrigger hangs).
		const std::unordered_set<std::uint32_t>& Buildable()
		{
			static std::unordered_set<std::uint32_t> set;
			static std::once_flag once;
			std::call_once(once, [] {
				auto* dh = RE::TESDataHandler::GetSingleton();
				if (!dh) {
					return;
				}
				// The FormType enum and RTTI casts do not match this game version, so take the runtime form type
				// from a known vanilla recipe: co_Outpost_Furniture_TablesScienceList01_Decor2_06 (0x100C2D)
				// creates ConferenceTable_PI03 (0x2E2EE0).
				constexpr std::uint32_t kProbe = 0x00100C2D, kProbeCreates = 0x002E2EE0;
				auto* probe = RE::TESForm::LookupByID(kProbe);
				if (!probe) {
					REX::WARN("buildable: probe recipe {:08X} not found", kProbe);
					return;
				}
				const auto type = probe->GetFormType();
				auto* probeCobj = reinterpret_cast<RE::BGSConstructibleObject*>(probe);
				if (!probeCobj->createdObject || probeCobj->createdObject->GetFormID() != kProbeCreates) {
					REX::WARN("buildable: recipe layout check failed (created {:08X})", probeCobj->createdObject ? probeCobj->createdObject->GetFormID() : 0);
					return;
				}
				// The per-type form arrays are not where the CommonLibSF header puts them in this game version:
				// scan TESDataHandler for an array {u32 size, u32 capacity, TESForm** data} that holds the probe.
				const auto dhAddr = reinterpret_cast<std::uintptr_t>(dh);
				for (std::uintptr_t o = 0; o < 0x8000 && set.empty(); o += 8) {
					if (!Readable(dhAddr + o, 16)) {
						break;
					}
					const auto size = *reinterpret_cast<std::uint32_t*>(dhAddr + o);
					const auto cap = *reinterpret_cast<std::uint32_t*>(dhAddr + o + 4);
					const auto data = *reinterpret_cast<RE::TESForm***>(dhAddr + o + 8);
					if (size == 0 || size > cap || size > 2000000 || !Readable(reinterpret_cast<std::uintptr_t>(data), std::size_t(size) * 8)) {
						continue;
					}
					bool hasProbe = false;
					for (std::uint32_t i = 0; i < size && !hasProbe; ++i) {
						hasProbe = data[i] == probe;
					}
					if (!hasProbe) {
						continue;
					}
					for (std::uint32_t i = 0; i < size; ++i) {
						auto* f = data[i];
						if (!f || f->GetFormType() != type) {
							continue;
						}
						auto* cobj = reinterpret_cast<RE::BGSConstructibleObject*>(f);
						if (cobj->createdObject) {
							set.insert(cobj->createdObject->GetFormID());
						}
					}
					REX::INFO("buildable: recipe array at TESDataHandler+{:X} (form type {}, {} recipes)", o, std::to_underlying(type), size);
				}
				REX::INFO("buildable: {} objects have a workshop recipe", set.size());
			});
			return set;
		}

		// True if the base can be built in a workshop. Without a recipe list, everything except
		// objects known to hang PlaceAtMe (Starfield.esm EmptyTrigger).
		bool IsBuildable(RE::TESForm* a_base)
		{
			if (!a_base) {
				return false;
			}
			const auto& set = Buildable();
			if (set.empty()) {
				return a_base->GetFormID() != 0x0000573F;
			}
			return set.contains(a_base->GetFormID());
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

		std::string Serialize(const Backup& b)
		{
			std::ostringstream o;
			o.imbue(std::locale::classic());
			o.precision(17);
			o << "OSB\t1\n";
			o << "name\t" << Clean(b.name) << '\n';
			o << "planetname\t" << Clean(b.planetName) << '\n';
			o << "systemname\t" << Clean(b.systemName) << '\n';
			if (!b.planetLocPlugin.empty()) {
				o << "planetloc\t" << b.planetLocPlugin << '\t' << std::format("{:X}", b.planetLocLocal) << '\n';
			}
			o << "time\t" << b.time << '\n';
			o << "planet\t" << std::format("{:08X}", b.planet) << '\t' << b.radiusKm << '\n';
			o << "origin\t" << b.lat << '\t' << b.lon << '\n';
			o << "beacon\t" << b.bx << '\t' << b.by << '\t' << b.bz << '\t' << b.byaw << '\n';
			for (const auto& it : b.items) {
				o << "item\t" << it.plugin << '\t' << std::format("{:X}", it.local) << '\t' << it.x << '\t' << it.y << '\t' << it.z << '\t'
				  << it.ax << '\t' << it.ay << '\t' << it.az << '\t' << it.scale << '\t' << (it.pack ? 1 : 0) << '\t' << std::format("{:08X}", it.ref) << '\n';
			}
			return o.str();
		}

		bool WriteText(const std::filesystem::path& a_path, const std::string& a_text)
		{
			std::ofstream f(a_path, std::ios::binary | std::ios::trunc);
			if (!f) {
				return false;
			}
			f << a_text;
			return static_cast<bool>(f);
		}

		// a_count: number of item lines; a_items: also keep the items.
		bool Load(const std::filesystem::path& a_path, Backup& b, std::size_t* a_count = nullptr, bool a_items = true)
		{
			std::ifstream f(a_path, std::ios::binary);
			if (!f) {
				return false;
			}
			b = {};
			std::size_t count = 0;
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
					if (t[0] == "item" && t.size() >= 11) {
						++count;
						if (!a_items) {
							continue;
						}
						Item it;
						it.plugin = t[1];
						it.local = std::stoul(t[2], nullptr, 16);
						it.x = float(num(3)); it.y = float(num(4)); it.z = float(num(5));
						it.ax = float(num(6)); it.ay = float(num(7)); it.az = float(num(8));
						it.scale = float(num(9));
						it.pack = t[10] == "1";
						if (t.size() >= 12) {
							it.ref = std::stoul(t[11], nullptr, 16);
						}
						b.items.push_back(std::move(it));
					} else if (t[0] == "name" && t.size() >= 2) {
						b.name = t[1];
					} else if (t[0] == "planetname" && t.size() >= 2) {
						b.planetName = t[1];
					} else if (t[0] == "systemname" && t.size() >= 2) {
						b.systemName = t[1];
					} else if (t[0] == "planetloc" && t.size() >= 3) {
						b.planetLocPlugin = t[1];
						b.planetLocLocal = std::stoul(t[2], nullptr, 16);
					} else if (t[0] == "time" && t.size() >= 2) {
						b.time = std::stoll(t[1]);
					} else if (t[0] == "planet" && t.size() >= 3) {
						b.planet = std::stoul(t[1], nullptr, 16);
						b.radiusKm = num(2);
					} else if (t[0] == "origin" && t.size() >= 3) {
						b.lat = num(1);
						b.lon = num(2);
					} else if (t[0] == "beacon" && t.size() >= 5) {
						b.bx = float(num(1)); b.by = float(num(2)); b.bz = float(num(3)); b.byaw = float(num(4));
					}
				} catch (...) {
					REX::WARN("backup: bad line in {}: {}", a_path.filename().string(), line);
				}
			}
			if (a_count) {
				*a_count = count;
			}
			return b.planet != 0 && b.radiusKm > 0;
		}

		// Version 1 kept a single latest.osb; bring it into Outposts once.
		void MigrateLatest(const std::filesystem::path& a_dir)
		{
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(a_dir, ec)) {
				if (e.path().extension() == ".osb") {
					return;
				}
			}
			const auto latest = Folder() / "latest.osb";
			Backup b;
			if (!Load(latest, b)) {
				return;
			}
			if (b.time == 0) {
				const auto ft = std::filesystem::last_write_time(latest, ec);
				b.time = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(ft).time_since_epoch()).count();
			}
			if (b.planetName.empty()) {
				b.planetName = FormName(RE::TESForm::LookupByID(b.planet));
			}
			const auto dest = a_dir / std::format("{:08X}-migrated.osb", b.planet);
			if (WriteText(dest, Serialize(b))) {
				REX::INFO("backup: migrated latest.osb -> {}", dest.string());
			}
		}

		std::vector<Entry> ListAll()
		{
			const auto dir = SubFolder("Outposts");
			MigrateLatest(dir);
			std::vector<Entry> out;
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (e.path().extension() != ".osb") {
					continue;
				}
				Entry en;
				en.path = e.path();
				if (Load(en.path, en.head, &en.count, false)) {
					out.push_back(std::move(en));
				}
			}
			std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.head.time > b.head.time; });
			return out;
		}

		std::string Label(const Entry& e)
		{
			const auto& h = e.head;
			std::string name = h.name.empty() ? "Outpost" : h.name;
			std::string planet = h.planetName.empty() ? std::format("planet {:08X}", h.planet) : h.planetName;
			std::string when;
			if (h.time > 0) {
				std::tm tm{};
				const std::time_t t = static_cast<std::time_t>(h.time);
				if (localtime_s(&tm, &t) == 0) {
					when = std::format(", {:04}-{:02}-{:02} {:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
				}
			}
			if (!h.systemName.empty()) {
				planet += " (" + h.systemName + ")";
			}
			return std::format("{} - {} - {} objects{}", name, planet, e.count, when);
		}

		// The selection survives a game restart (a running slate flow is in the save, the plugin state is not).
		void SaveSelection()
		{
			WriteText(Folder() / "selected.txt", g_selected.filename().string());
		}

		void RestoreSelection()
		{
			if (!g_selected.empty()) {
				return;
			}
			std::ifstream f(Folder() / "selected.txt");
			std::string name;
			if (f && std::getline(f, name) && !name.empty()) {
				const auto p = Folder() / "Outposts" / name;
				std::error_code ec;
				if (std::filesystem::exists(p, ec)) {
					g_selected = p;
				}
			}
		}

		const Entry* SelectedEntry()
		{
			for (const auto& e : g_list) {
				if (e.path == g_selected) {
					return &e;
				}
			}
			return nullptr;
		}

		// ---------- natives ----------

		std::int32_t Version(std::monostate)
		{
			return kVersion;
		}

		// Only one slate action at a time; a stuck lock expires after 3 minutes.
		bool TryLock(std::monostate)
		{
			std::scoped_lock l(g_lock);
			const auto now = std::chrono::steady_clock::now();
			if (g_busy && now - g_busySince < std::chrono::minutes(3)) {
				return false;
			}
			g_busy = true;
			g_busySince = now;
			return true;
		}

		void Unlock(std::monostate)
		{
			std::scoped_lock l(g_lock);
			g_busy = false;
		}

		std::string LocationName(RE::BGSLocation* a_loc)
		{
			if (!a_loc) {
				return {};
			}
			const char* n = static_cast<RE::TESFullName*>(a_loc)->GetFullName();
			return n && *n ? Clean(n) : std::string();
		}

		// a_planetLoc: the planet's location (planet name); a_hereLoc: the player's current location (outpost name).
		bool BeginBackup(std::monostate, RE::TESObjectREFR* a_beacon, RE::BGSLocation* a_planetLoc, std::vector<RE::BGSLocation*> a_planetParents, RE::BGSLocation* a_hereLoc, float a_bx, float a_by, float a_bz, float a_byaw)
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
			g_pending.planetName = LocationName(a_planetLoc);
			if (a_planetLoc) {
				SplitFormID(a_planetLoc->GetFormID(), g_pending.planetLocPlugin, g_pending.planetLocLocal);
			}
			if (g_pending.planetName.empty()) {
				g_pending.planetName = FormName(RE::TESForm::LookupByID(s.planet));
			}
			for (auto* loc : a_planetParents) {   // the star system is one of the planet location's parents
				const auto n = LocationName(loc);
				REX::INFO("backup: planet parent location {:08X} '{}'", loc ? loc->GetFormID() : 0, n);
				if (g_pending.systemName.empty() && !n.empty() && n != g_pending.planetName) {
					g_pending.systemName = n;
				}
			}
			g_pending.name = LocationName(a_hereLoc);
			if (g_pending.name == g_pending.planetName) {
				g_pending.name.clear();
			}
			if (g_pending.name.empty() && a_beacon) {
				if (const char* n = a_beacon->GetDisplayFullName(); n && *n) {
					g_pending.name = Clean(n);
				}
			}
			g_skipped = 0;
			return true;
		}

		bool AddItem(std::monostate, RE::TESForm* a_base, float x, float y, float z, float ax, float ay, float az, float scale, bool a_pack, std::int32_t a_refID)
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
			if (!IsBuildable(a_base) && !a_pack) {   // PackIns stay: vanilla habs are build menu variants without a recipe
				++g_skipped;   // part spawned by a hab or other object; comes back with its parent
				return false;
			}
			g_pending.items.push_back({ plugin, local, x, y, z, ax, ay, az, scale, a_pack, static_cast<std::uint32_t>(a_refID) });
			return true;
		}

		// Returns the number of objects saved, 0 if there was nothing to save, -1 on failure.
		std::int32_t EndBackup(std::monostate)
		{
			auto list = ListAll();
			std::scoped_lock l(g_lock);
			g_replaced = false;
			if (g_pending.planet == 0) {
				return -1;
			}
			if (g_pending.items.empty()) {
				return 0;
			}
			g_pending.time = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

			// the same outpost backed up before: replace its file
			const auto dir = SubFolder("Outposts");
			std::filesystem::path target;
			for (const auto& e : list) {
				if (e.head.planet == g_pending.planet && BeaconDistance(e.head, g_pending) < kSameOutpostMeters) {
					target = e.path;
					g_replaced = true;
					if (g_pending.name.empty()) {
						g_pending.name = e.head.name;
					}
					break;
				}
			}
			if (target.empty()) {
				const auto stamp = std::format("{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
				target = dir / std::format("{:08X}-{}.osb", g_pending.planet, stamp);
			}
			const auto text = Serialize(g_pending);
			if (!WriteText(target, text)) {
				REX::ERROR("backup: could not write {}", target.string());
				return -1;
			}
			WriteText(Folder() / "latest.osb", text);
			REX::INFO("backup: {} objects ({} parts skipped), '{}' on {} ({:08X}), center {:.6f} {:.6f} -> {}{}", g_pending.items.size(), g_skipped, g_pending.name,
				g_pending.planetName, g_pending.planet, g_pending.lat, g_pending.lon, target.string(), g_replaced ? " (replaced)" : "");
			return std::int32_t(g_pending.items.size());
		}

		bool LastBackupReplaced(std::monostate)
		{
			std::scoped_lock l(g_lock);
			return g_replaced;
		}

		// Start a new automatic flow; older flows see the id change and stop.
		std::int32_t NewFlow(std::monostate)
		{
			std::scoped_lock l(g_lock);
			return ++g_flow;
		}

		std::int32_t CurrentFlow(std::monostate)
		{
			std::scoped_lock l(g_lock);
			return g_flow;
		}

		// Select the only saved outpost on this planet, if there is exactly one. Returns the number on this planet.
		std::int32_t SelectOnlyHere(std::monostate)
		{
			auto list = ListAll();
			const auto s = ReadPlanet();
			std::erase_if(list, [&](const Entry& e) { return e.head.planet != s.planet; });
			std::scoped_lock l(g_lock);
			if (list.size() == 1) {
				g_selected = list[0].path;
				SaveSelection();
			}
			g_list = std::move(list);
			return std::int32_t(g_list.size());
		}

		bool HasBackup(std::monostate)
		{
			return !ListAll().empty();
		}

		// List saved outposts (newest first) for the picker. abHereOnly: only the planet you are on.
		std::int32_t ListBackups(std::monostate, bool a_hereOnly)
		{
			auto list = ListAll();
			if (a_hereOnly) {
				const auto s = ReadPlanet();
				std::erase_if(list, [&](const Entry& e) { return e.head.planet != s.planet; });
			}
			std::scoped_lock l(g_lock);
			g_list = std::move(list);
			g_page = 0;
			return std::int32_t(g_list.size());
		}

		// Write page a_page of the list into OSB_PickMsg's buttons: Cancel, [More...], entries.
		// Returns the number of entries on the page.
		std::int32_t FillPicker(std::monostate, RE::BGSMessage* a_msg, std::int32_t a_page)
		{
			auto* msg = a_msg;
			if (!msg) {
				return 0;
			}
			std::scoped_lock l(g_lock);
			if (g_pickButtons.empty()) {
				for (auto* b : msg->menuButtons) {
					g_pickButtons.push_back(b);
				}
			}
			if (g_pickButtons.size() < kPickButtons) {
				REX::ERROR("picker: OSB_PickMsg has {} buttons, expected {}", g_pickButtons.size(), kPickButtons);
				return 0;
			}
			const std::size_t pages = std::max<std::size_t>(1, (g_list.size() + kPickEntries - 1) / kPickEntries);
			g_page = std::clamp<std::int32_t>(a_page, 0, std::int32_t(pages - 1));
			g_pageHasMore = pages > 1;
			const std::size_t first = std::size_t(g_page) * kPickEntries;
			const std::size_t shown = std::min(kPickEntries, g_list.size() - std::min(first, g_list.size()));

			msg->menuButtons.clear();
			msg->menuButtons.push_back(g_pickButtons[0]);
			if (g_pageHasMore) {
				g_pickButtons[1]->text = RE::BSFixedString(std::format("More... (page {} of {})", g_page + 1, pages).c_str());
				msg->menuButtons.push_back(g_pickButtons[1]);
			}
			for (std::size_t i = 0; i < shown; ++i) {
				auto* b = g_pickButtons[2 + i];
				b->text = RE::BSFixedString(Label(g_list[first + i]).c_str());
				msg->menuButtons.push_back(b);
			}
			return std::int32_t(shown);
		}

		// Map the button OSB_PickMsg.Show() returned: list index, -1 cancel, -2 next page.
		std::int32_t PickerResult(std::monostate, std::int32_t a_button)
		{
			std::scoped_lock l(g_lock);
			if (a_button <= 0) {
				return -1;
			}
			std::int32_t i = a_button - 1;
			if (g_pageHasMore) {
				if (i == 0) {
					return -2;
				}
				--i;
			}
			const std::size_t idx = std::size_t(g_page) * kPickEntries + std::size_t(i);
			return idx < g_list.size() ? std::int32_t(idx) : -1;
		}

		bool SelectBackup(std::monostate, std::int32_t a_index)
		{
			std::scoped_lock l(g_lock);
			if (a_index < 0 || std::size_t(a_index) >= g_list.size()) {
				g_selected.clear();
				return false;
			}
			g_selected = g_list[a_index].path;
			SaveSelection();
			return true;
		}

		std::string GetSelectedName(std::monostate)
		{
			std::scoped_lock l(g_lock);
			const auto* e = SelectedEntry();
			return e ? Label(*e) : std::string();
		}

		std::string GetSelectedPlanetName(std::monostate)
		{
			std::scoped_lock l(g_lock);
			const auto* e = SelectedEntry();
			if (!e) {
				return {};
			}
			return e->head.planetName.empty() ? std::string("the saved outpost's planet") : e->head.planetName;
		}

		std::string GetSelectedSystemName(std::monostate)
		{
			std::scoped_lock l(g_lock);
			const auto* e = SelectedEntry();
			return e ? e->head.systemName : std::string();
		}

		// The selected outpost's planet Location (None for backups made before v0.3.1).
		RE::BGSLocation* GetSelectedPlanetLocation(std::monostate)
		{
			std::scoped_lock l(g_lock);
			RestoreSelection();
			Backup b;
			if (g_selected.empty() || !Load(g_selected, b, nullptr, false) || b.planetLocPlugin.empty()) {
				return nullptr;
			}
			auto* form = FindForm(b.planetLocPlugin, b.planetLocLocal);
			return form ? form->As<RE::BGSLocation>() : nullptr;
		}

		bool IsBuildableBase(std::monostate, RE::TESForm* a_base)
		{
			return a_base && IsBuildable(a_base);
		}

		// Move the selected backup to OutpostBackup\Deleted.
		bool DeleteBackup(std::monostate)
		{
			std::scoped_lock l(g_lock);
			if (g_selected.empty()) {
				return false;
			}
			std::error_code ec;
			const auto dest = SubFolder("Deleted") / g_selected.filename();
			std::filesystem::remove(dest, ec);
			std::filesystem::rename(g_selected, dest, ec);
			if (ec) {
				REX::WARN("delete: {} -> {}: {}", g_selected.string(), dest.string(), ec.message());
				return false;
			}
			REX::INFO("delete: moved {} to {}", g_selected.filename().string(), dest.string());
			std::erase_if(g_list, [&](const Entry& e) { return e.path == g_selected; });
			g_selected.clear();
			return true;
		}

		// The next star-map landing on the selected backup's planet goes to its landing-zone center.
		bool ArmLanding(std::monostate)
		{
			std::scoped_lock l(g_lock);
			RestoreSelection();
			Backup b;
			if (g_selected.empty() || !Load(g_selected, b, nullptr, false)) {
				return false;
			}
			g_armed = true;
			g_armPlanet = b.planet;
			g_armLat = b.lat;
			g_armLon = b.lon;
			REX::INFO("landing armed: planet {:08X} {:.6f} {:.6f} ({})", b.planet, b.lat, b.lon, g_selected.filename().string());
			return true;
		}

		// Selected backup's beacon spot in the current landing zone: [x, y, z, distance between landing centers (m)].
		// Empty if nothing is selected or you are on another planet.
		std::vector<float> GetGuideSpot(std::monostate)
		{
			Backup b;
			{
				std::scoped_lock l(g_lock);
				RestoreSelection();
				if (g_selected.empty() || !Load(g_selected, b, nullptr, false)) {
					return {};
				}
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

		// Prepare a restore of the selected backup: returns the number of objects to place
		// (PackIn parts and co-located duplicates skipped), -1 if nothing is selected.
		std::int32_t LoadRestore(std::monostate)
		{
			std::scoped_lock l(g_lock);
			RestoreSelection();
			g_restoreOrder.clear();
			g_menuCandidates.clear();
			if (g_selected.empty() || !Load(g_selected, g_loaded)) {
				return -1;
			}
			const auto s = ReadPlanet();
			if (s.planet != g_loaded.planet) {
				REX::WARN("restore: backup is for planet {:08X}, you are on {:08X}", g_loaded.planet, s.planet);
				return -1;
			}
			// shift objects from the backup's landing frame into the current one
			const double R = s.radiusKm * 1000.0, toRad = std::numbers::pi / 180.0;
			double dLon = g_loaded.lon - s.lon;
			if (dLon > 180) dLon -= 360;
			if (dLon < -180) dLon += 360;
			const float east = float(dLon * toRad * R * std::cos((g_loaded.lat + s.lat) / 2 * toRad));
			const float north = float((g_loaded.lat - s.lat) * toRad * R);
			for (auto& it : g_loaded.items) {
				it.x += east;
				it.y += north;
			}

			std::size_t notBuildable = 0;
			struct Group { float x, y, z; bool pack; };
			std::vector<Group> groups;
			for (std::size_t i = 0; i < g_loaded.items.size(); ++i) {
				const auto& it = g_loaded.items[i];
				if (auto* form = FindForm(it.plugin, it.local); form && !IsBuildable(form)) {
					if (it.pack) {
						g_menuCandidates.push_back(i);
					}
					++notBuildable;   // hab part (light, trigger, sound space...): its parent spawns it
					continue;
				}
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
			REX::INFO("restore: {} of {} objects to place ({} hab parts left to their parents)", g_restoreOrder.size(), g_loaded.items.size(), notBuildable);
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

		// Build the loaded restore list through the workshop engine (player must be in build mode).
		bool AutoBuild(std::monostate)
		{
			std::vector<Builder::Job> jobs;
			{
				std::scoped_lock l(g_lock);
				// recipe-buildable objects, then PackIns the build menu offers as variants (vanilla habs);
				// a PackIn at the same pivot as one already kept is spawned by it
				std::vector<std::size_t> order = g_restoreOrder;
				std::vector<std::size_t> packs;
				for (auto idx : g_restoreOrder) {
					if (g_loaded.items[idx].pack) {
						packs.push_back(idx);
					}
				}
				std::size_t fromMenu = 0;
				for (auto idx : g_menuCandidates) {
					const auto& it = g_loaded.items[idx];
					auto* form = FindForm(it.plugin, it.local);
					if (!form || !Builder::InBuildMenu(form->GetFormID())) {
						continue;
					}
					const bool covered = std::ranges::any_of(packs, [&](std::size_t p) {
						const auto& o = g_loaded.items[p];
						return std::abs(o.x - it.x) < 0.01f && std::abs(o.y - it.y) < 0.01f && std::abs(o.z - it.z) < 0.2f;
					});
					if (covered) {
						continue;
					}
					REX::INFO("restore: {} {:X} at {:.2f} {:.2f} {:.2f} is a build menu variant: building it", it.plugin, it.local, it.x, it.y, it.z);
					order.push_back(idx);
					packs.push_back(idx);
					++fromMenu;
				}
				REX::INFO("restore: {} objects from recipes, {} more from build menu variants", g_restoreOrder.size(), fromMenu);
				for (auto idx : order) {
					const auto& it = g_loaded.items[idx];
					auto* form = FindForm(it.plugin, it.local);
					if (!form) {
						continue;
					}
					// footprint from the base bounds (TESBoundObject +0x50 min, +0x5C max)
					const auto* b = reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(form) + 0x50);
					float size = (b[3] - b[0]) * (b[4] - b[1]);
					if (!std::isfinite(size) || size < 0 || size > 1e6f) {
						size = 0;
					}
					const float hx = std::isfinite(b[3] - b[0]) ? std::abs(b[3] - b[0]) / 2 : 0.0f;
					const float hy = std::isfinite(b[4] - b[1]) ? std::abs(b[4] - b[1]) / 2 : 0.0f;
					jobs.push_back({ form->GetFormID(), it.x, it.y, it.z, it.az, std::format("{} {:X}", it.plugin, it.local), size, it.ref, it.ax, it.ay, hx, hy });
				}
			}
			return !jobs.empty() && Builder::Start(std::move(jobs));
		}

		// [done, built, failed, total, running]
		std::vector<std::int32_t> AutoBuildProgress(std::monostate)
		{
			return Builder::Progress();
		}

		bool IsPlacing(std::monostate)
		{
			return Builder::IsPlacing();
		}

		void AutoBuildStop(std::monostate)
		{
			Builder::Stop();
		}

		// ---- after a build: put misplaced objects on their saved spot, report hab parts ----
		std::vector<Builder::Correction> g_corr;   // snapshot taken by CorrectionCount

		std::int32_t CorrectionCount(std::monostate)
		{
			g_corr = Builder::Corrections();
			return std::int32_t(g_corr.size());
		}

		// the reference (Papyrus casts it to ObjectReference); None if it is gone
		RE::TESForm* CorrectionRef(std::monostate, std::int32_t i)
		{
			if (i < 0 || std::size_t(i) >= g_corr.size()) {
				return nullptr;
			}
			return RE::TESForm::LookupByID(g_corr[i].ref);
		}

		// x y z, angle x y z (degrees)
		std::vector<float> CorrectionXform(std::monostate, std::int32_t i)
		{
			if (i < 0 || std::size_t(i) >= g_corr.size()) {
				return {};
			}
			const auto& c = g_corr[i];
			return { c.x, c.y, c.z, c.ax, c.ay, c.az };
		}

		// The outpost as it is after the build, listed by the script (like a backup): base, position, angle z (deg)
		std::vector<Builder::Placed> g_now;

		void ReportBegin(std::monostate)
		{
			g_now.clear();
		}

		void ReportAddRef(std::monostate, RE::TESForm* a_base, float x, float y, float z, float az)
		{
			if (a_base) {
				g_now.push_back({ 0, a_base->GetFormID(), x, y, z, 0, 0, az * std::numbers::pi_v<float> / 180.0f });
			}
		}

		// the whole outpost in one call (one Papyrus call per reference took about a minute)
		void ReportRefs(std::monostate, std::vector<RE::TESObjectREFR*> a_refs)
		{
			g_now.clear();
			for (auto* r : a_refs) {
				if (!r) {
					continue;
				}
				const auto base = r->GetBaseObject();
				if (!base) {
					continue;
				}
				const auto p = r->GetPosition();
				const auto a = r->GetAngle();
				g_now.push_back({ r->GetFormID(), base->GetFormID(), p.x, p.y, p.z, a.x, a.y, a.z });
			}
		}

		// wall slots whose style differs from the backup: the wall there now, and the style (PackIn) it should be
		struct StyleFix
		{
			std::uint32_t ref{ 0 }, want{ 0 };
			std::string name;
		};
		std::vector<StyleFix> g_styleFixes;

		float AngleDiffDeg(float a_deg, float b_deg)
		{
			return std::abs(std::remainder(a_deg - b_deg, 360.0f));
		}

		// Compare the backup's PackIns (habs, hab walls with their door / window style, corridors) with what the
		// outpost now has (ReportRefs). Logs each saved part that is not there and what is there instead, and collects
		// the walls that are there with another style (g_styleFixes, for FixStyles).
		// Returns how many are not there.
		std::int32_t ReportParts(std::monostate)
		{
			const auto& placed = g_now;
			std::scoped_lock l(g_lock);
			g_styleFixes.clear();
			std::int32_t missing = 0, found = 0;
			constexpr float toDeg = 180.0f / std::numbers::pi_v<float>;
			for (const auto& it : g_loaded.items) {
				if (!it.pack) {
					continue;
				}
				auto* form = FindForm(it.plugin, it.local);
				if (!form) {
					continue;
				}
				const auto want = form->GetFormID();
				const Builder::Placed* same = nullptr;
				const Builder::Placed* slot = nullptr;    // another PackIn at the same spot and angle: the same wall, other style
				const Builder::Placed* other = nullptr;   // anything else nearby, for the log
				float otherDist = 0.5f;
				for (const auto& p : placed) {
					const float d = std::hypot(p.x - it.x, p.y - it.y);
					if (std::abs(p.z - it.z) > 0.5f || d > 0.5f) {
						continue;
					}
					const bool sameAngle = AngleDiffDeg(p.az * toDeg, it.az) < 10.0f;
					if (p.base == want && d < 0.3f) {
						same = &p;
						break;
					}
					if (p.base == want || p.base == 0x15804) {
						continue;
					}
					auto* pf = RE::TESForm::LookupByID(p.base);
					const bool isPack = pf && pf->GetFormType() == form->GetFormType();
					// only hab walls: a hab or anything else from the build menu is not a style of a wall
					if (isPack && sameAngle && d < 0.3f && !slot && !Builder::InBuildMenu(want) && !Builder::InBuildMenu(p.base)) {
						slot = &p;
					}
					if (d < otherDist) {
						other = &p;
						otherDist = d;
					}
				}
				if (same) {
					++found;
					continue;
				}
				++missing;
				const auto* shown = slot ? slot : other;
				auto* shownForm = shown ? RE::TESForm::LookupByID(shown->base) : nullptr;
				REX::INFO("parts: saved {} {:X} '{}' at {:.2f} {:.2f} {:.2f} angle {:.0f}: not there{}{}", it.plugin, it.local, FormName(form), it.x, it.y,
					it.z, it.az,
					shown ? std::format(", instead {:08X} '{}' angle {:.0f}", shown->base, FormName(shownForm), shown->az * toDeg) : "",
					slot ? " (same wall, other style: will switch)" : "");
				if (slot) {
					g_styleFixes.push_back({ slot->ref, want, std::format("{} {:X}", it.plugin, it.local) });
				}
			}
			// the other way round: hab walls the outpost has now on a spot and side where the backup had none
			// (a door or window opened on the wrong side shows up here, or in the list above as the saved plain
			// wall "not there, instead" the door)
			std::int32_t extra = 0;
			RE::TESForm* packSample = nullptr;
			for (const auto& it : g_loaded.items) {
				if (it.pack) {
					if ((packSample = FindForm(it.plugin, it.local))) {
						break;
					}
				}
			}
			for (const auto& p : placed) {
				if (!packSample || p.base == 0x15804 || Builder::InBuildMenu(p.base)) {
					continue;
				}
				auto* pf = RE::TESForm::LookupByID(p.base);
				if (!pf || pf->GetFormType() != packSample->GetFormType()) {
					continue;
				}
				const bool saved = std::ranges::any_of(g_loaded.items, [&](const Item& it) {
					return it.pack && std::abs(it.z - p.z) < 0.5f && std::hypot(it.x - p.x, it.y - p.y) < 0.3f &&
					       AngleDiffDeg(p.az * toDeg, it.az) < 10.0f;
				});
				if (!saved) {
					++extra;
					REX::INFO("parts: extra {:08X} '{}' (ref {:08X}) at {:.2f} {:.2f} {:.2f} angle {:.0f}: the backup has no PackIn on that spot and side",
						p.base, FormName(pf), p.ref, p.x, p.y, p.z, p.az * toDeg);
				}
			}
			REX::INFO("parts: {} of {} saved PackIns are there, {} are not, {} extra ({} walls to switch style)", found, found + missing, missing, extra, g_styleFixes.size());
			return missing;
		}

		// One "change style" press for each wall in g_styleFixes, the way the player does it in build mode's
		// inspect mode (point at the wall, press "change"). Builder runs the presses one wall at a time on the
		// frame task (see Builder::StartStyles); the script calls this once per round and looks again in between.
		// Returns how many walls still have the wrong style (0: nothing to do). While a run is still going it
		// only reports that count and starts nothing.
		std::int32_t FixStyles(std::monostate)
		{
			std::vector<StyleFix> fixes;
			{
				std::scoped_lock l(g_lock);
				fixes = g_styleFixes;
			}
			if (fixes.empty()) {
				return 0;
			}
			if (Builder::StylesRunning()) {
				return std::int32_t(fixes.size());
			}
			const auto* tasks = SFSE::GetTaskInterface();
			if (!tasks) {
				REX::WARN("styles: no SFSE task interface");
				return 0;
			}
			tasks->AddTask([fixes]() {
				// reference -> handle: the handle table (manager at +0x5E60380: entries {object, tag} at +0x50,
				// index mask at +0x68); handle = index | (tag & ~mask)
				const auto mgr = *reinterpret_cast<std::uintptr_t*>(REL::ID(883285).address());
				std::unordered_map<std::uintptr_t, std::uint32_t> handles;
				std::unordered_set<std::uintptr_t> wanted;
				for (const auto& f : fixes) {
					if (auto* ref = RE::TESForm::LookupByID(f.ref)) {
						wanted.insert(reinterpret_cast<std::uintptr_t>(ref));
					}
				}
				if (mgr) {
					const auto mask = *reinterpret_cast<std::uint32_t*>(mgr + 0x68);
					const auto table = *reinterpret_cast<std::uintptr_t*>(mgr + 0x50);
					for (std::uint64_t i = 1; table && i <= mask && handles.size() < wanted.size(); ++i) {
						const auto obj = *reinterpret_cast<std::uintptr_t*>(table + i * 16);
						if (obj && wanted.contains(obj)) {
							const auto tag = *reinterpret_cast<std::uint32_t*>(table + i * 16 + 8);
							handles[obj] = std::uint32_t(i) | (tag & ~mask);
						}
					}
				}
				std::vector<Builder::StyleTarget> targets;
				for (const auto& f : fixes) {
					auto* ref = static_cast<RE::TESObjectREFR*>(RE::TESForm::LookupByID(f.ref));
					if (!ref) {
						REX::INFO("styles: {} wall {:08X} is gone", f.name, f.ref);
						continue;
					}
					const auto base = ref->GetBaseObject();
					const auto id = base ? base->GetFormID() : 0;
					if (id == f.want) {
						REX::INFO("styles: {} wall {:08X} is {:08X}: done", f.name, f.ref, id);
						continue;
					}
					const auto h = handles.find(reinterpret_cast<std::uintptr_t>(ref));
					if (h == handles.end()) {
						REX::INFO("styles: {} wall {:08X} ({:08X}): no handle", f.name, f.ref, id);
						continue;
					}
					targets.push_back({ f.ref, h->second, f.want, f.name });
				}
				if (!targets.empty() && !Builder::StartStyles(std::move(targets))) {
					REX::WARN("styles: could not start a style run (build or style run active, or hooks missing)");
				}
			});
			return std::int32_t(fixes.size());
		}
	}

	std::filesystem::path DataFolder()
	{
		return Folder();
	}

	bool TakeArmedLanding(std::uint32_t a_planet, double& a_latDeg, double& a_lonDeg)
	{
		std::scoped_lock l(g_lock);
		if (!g_armed) {
			return false;
		}
		if (a_planet != g_armPlanet) {
			REX::INFO("landing: armed for planet {:08X}, this landing is on {:08X}; not changed", g_armPlanet, a_planet);
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
		vm->BindNativeMethod(obj, "Version", Version, true, false);
		vm->BindNativeMethod(obj, "TryLock", TryLock, true, false);
		vm->BindNativeMethod(obj, "Unlock", Unlock, true, false);
		vm->BindNativeMethod(obj, "BeginBackup", BeginBackup, true, false);
		vm->BindNativeMethod(obj, "AddItem", AddItem, true, false);
		vm->BindNativeMethod(obj, "EndBackup", EndBackup, true, false);
		vm->BindNativeMethod(obj, "LastBackupReplaced", LastBackupReplaced, true, false);
		vm->BindNativeMethod(obj, "HasBackup", HasBackup, true, false);
		vm->BindNativeMethod(obj, "NewFlow", NewFlow, true, false);
		vm->BindNativeMethod(obj, "CurrentFlow", CurrentFlow, true, false);
		vm->BindNativeMethod(obj, "SelectOnlyHere", SelectOnlyHere, true, false);
		vm->BindNativeMethod(obj, "ListBackups", ListBackups, true, false);
		vm->BindNativeMethod(obj, "FillPicker", FillPicker, true, false);
		vm->BindNativeMethod(obj, "PickerResult", PickerResult, true, false);
		vm->BindNativeMethod(obj, "SelectBackup", SelectBackup, true, false);
		vm->BindNativeMethod(obj, "GetSelectedName", GetSelectedName, true, false);
		vm->BindNativeMethod(obj, "GetSelectedPlanetName", GetSelectedPlanetName, true, false);
		vm->BindNativeMethod(obj, "GetSelectedSystemName", GetSelectedSystemName, true, false);
		vm->BindNativeMethod(obj, "IsBuildable", IsBuildableBase, true, false);
		vm->BindNativeMethod(obj, "GetSelectedPlanetLocation", GetSelectedPlanetLocation, true, false);
		vm->BindNativeMethod(obj, "DeleteBackup", DeleteBackup, true, false);
		vm->BindNativeMethod(obj, "ArmLanding", ArmLanding, true, false);
		vm->BindNativeMethod(obj, "GetGuideSpot", GetGuideSpot, true, false);
		vm->BindNativeMethod(obj, "LoadRestore", LoadRestore, true, false);
		vm->BindNativeMethod(obj, "GetRestoreForm", GetRestoreForm, true, false);
		vm->BindNativeMethod(obj, "GetRestoreXform", GetRestoreXform, true, false);
		vm->BindNativeMethod(obj, "GetRestoreName", GetRestoreName, true, false);
		// AutoBuild and ReportParts read the build menu (Builder::InBuildMenu -> 21A6B60, which walks the menu
		// table unlocked and writes its selection): not tasklet-callable, so the VM runs them on the main thread
		vm->BindNativeMethod(obj, "AutoBuild", AutoBuild, false, false);
		vm->BindNativeMethod(obj, "AutoBuildProgress", AutoBuildProgress, true, false);
		vm->BindNativeMethod(obj, "AutoBuildStop", AutoBuildStop, true, false);
		vm->BindNativeMethod(obj, "CorrectionCount", CorrectionCount, true, false);
		vm->BindNativeMethod(obj, "CorrectionRef", CorrectionRef, true, false);
		vm->BindNativeMethod(obj, "CorrectionXform", CorrectionXform, true, false);
		vm->BindNativeMethod(obj, "ReportParts", ReportParts, false, false);
		vm->BindNativeMethod(obj, "ReportBegin", ReportBegin, true, false);
		vm->BindNativeMethod(obj, "ReportAddRef", ReportAddRef, true, false);
		vm->BindNativeMethod(obj, "ReportRefs", ReportRefs, true, false);
		vm->BindNativeMethod(obj, "FixStyles", FixStyles, true, false);
		vm->BindNativeMethod(obj, "IsPlacing", IsPlacing, true, false);
		REX::INFO("papyrus: OSB_Native registered (version {})", kVersion);
	}
}
