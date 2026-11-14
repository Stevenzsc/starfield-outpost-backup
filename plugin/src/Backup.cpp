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

#include "REX/W32/OLE32.h"
#include "REX/W32/SHELL32.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
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

		constexpr std::int32_t kVersion = 2;

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
		};

		struct Backup
		{
			std::string name, planetName;
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
		Backup     g_loaded;      // read by LoadRestore
		std::vector<std::size_t> g_restoreOrder;   // indices into g_loaded.items after filtering

		std::vector<Entry> g_list;       // last ListBackups result
		std::int32_t       g_page = 0;
		bool               g_pageHasMore = false;
		std::filesystem::path g_selected;

		std::vector<RE::BGSMessage::MESSAGEBOX_BUTTON*> g_pickButtons;   // original OSB_PickMsg buttons

		bool          g_armed = false;
		std::uint32_t g_armPlanet = 0;
		double        g_armLat = 0, g_armLon = 0;

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
			o << "time\t" << b.time << '\n';
			o << "planet\t" << std::format("{:08X}", b.planet) << '\t' << b.radiusKm << '\n';
			o << "origin\t" << b.lat << '\t' << b.lon << '\n';
			o << "beacon\t" << b.bx << '\t' << b.by << '\t' << b.bz << '\t' << b.byaw << '\n';
			for (const auto& it : b.items) {
				o << "item\t" << it.plugin << '\t' << std::format("{:X}", it.local) << '\t' << it.x << '\t' << it.y << '\t' << it.z << '\t'
				  << it.ax << '\t' << it.ay << '\t' << it.az << '\t' << it.scale << '\t' << (it.pack ? 1 : 0) << '\n';
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
						b.items.push_back(std::move(it));
					} else if (t[0] == "name" && t.size() >= 2) {
						b.name = t[1];
					} else if (t[0] == "planetname" && t.size() >= 2) {
						b.planetName = t[1];
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
			return std::format("{} - {} ({} objects{})", name, planet, e.count, when);
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

		bool BeginBackup(std::monostate, RE::TESForm* a_beacon, float a_bx, float a_by, float a_bz, float a_byaw)
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
			if (auto* ref = a_beacon ? a_beacon->AsReference() : nullptr) {
				if (const char* n = ref->GetDisplayFullName(); n && *n) {
					g_pending.name = Clean(n);
				}
			}
			g_pending.planetName = FormName(RE::TESForm::LookupByID(s.planet));
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
			REX::INFO("backup: {} objects, '{}' on {} ({:08X}), center {:.6f} {:.6f} -> {}{}", g_pending.items.size(), g_pending.name,
				g_pending.planetName, g_pending.planet, g_pending.lat, g_pending.lon, target.string(), g_replaced ? " (replaced)" : "");
			return std::int32_t(g_pending.items.size());
		}

		bool LastBackupReplaced(std::monostate)
		{
			std::scoped_lock l(g_lock);
			return g_replaced;
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
		std::int32_t FillPicker(std::monostate, RE::TESForm* a_msg, std::int32_t a_page)
		{
			auto* msg = a_msg ? a_msg->As<RE::BGSMessage>() : nullptr;
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
			g_restoreOrder.clear();
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
		vm->BindNativeMethod(obj, "ListBackups", ListBackups, true, false);
		vm->BindNativeMethod(obj, "FillPicker", FillPicker, true, false);
		vm->BindNativeMethod(obj, "PickerResult", PickerResult, true, false);
		vm->BindNativeMethod(obj, "SelectBackup", SelectBackup, true, false);
		vm->BindNativeMethod(obj, "GetSelectedName", GetSelectedName, true, false);
		vm->BindNativeMethod(obj, "GetSelectedPlanetName", GetSelectedPlanetName, true, false);
		vm->BindNativeMethod(obj, "DeleteBackup", DeleteBackup, true, false);
		vm->BindNativeMethod(obj, "ArmLanding", ArmLanding, true, false);
		vm->BindNativeMethod(obj, "GetGuideSpot", GetGuideSpot, true, false);
		vm->BindNativeMethod(obj, "LoadRestore", LoadRestore, true, false);
		vm->BindNativeMethod(obj, "GetRestoreForm", GetRestoreForm, true, false);
		vm->BindNativeMethod(obj, "GetRestoreXform", GetRestoreXform, true, false);
		vm->BindNativeMethod(obj, "GetRestoreName", GetRestoreName, true, false);
		REX::INFO("papyrus: OSB_Native registered (version {})", kVersion);
	}
}
