#include "pch.h"
#include "describe_path.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>

#include <xivres/excel.h>

namespace {
	// The music path of a BGM row: the only string cell in the row that looks like one. The
	// sheet's File field is its only string column, and the exh carries no field names, so it
	// is recognised by its value rather than by a column number.
	std::string bgm_row_path(const xivres::excel::exd::row::buffer& row) {
		for (const auto& subrow : row)
			for (const auto& cell : subrow)
				if (cell.Type == xivres::excel::cell_type::String) {
					auto path = cell.String.repr();
					if (path.starts_with("music/") && path.ends_with(".scd"))
						return path;
				}
		return {};
	}

	// A sheet's columns in the order the linked schemas describe the sheet's fields in.
	//
	// The exh stores a type and a byte offset per column, and lists them in no particular
	// order, while a schema's field order is the order the fields appear in a row: sorting the
	// columns by their byte offset gives that order. Packed bools share the byte they live in,
	// and their cell types encode which bit they select, which is the order the schema lists
	// them in as well.
	std::vector<size_t> schema_field_columns(const xivres::excel::exh::reader& exh) {
		const auto& columns = exh.get_columns();
		std::vector<size_t> order(columns.size());
		std::ranges::iota(order, size_t{0});
		std::ranges::sort(order, [&](size_t a, size_t b) {
			return std::pair(*columns[a].Offset, *columns[a].Type) < std::pair(*columns[b].Offset, *columns[b].Type);
		});
		return order;
	}

	// One field of a row, addressed as the schemas number fields, or nullptr when the row does
	// not have that many columns (a truncated or otherwise unexpected table).
	const xivres::excel::cell* schema_field(const xivres::excel::exd::row::buffer& row, const std::vector<size_t>& columns, size_t field) {
		if (row.size() == 0 || field >= columns.size() || columns[field] >= row[0].size())
			return nullptr;
		return &row[0][columns[field]];
	}

	// The row id a cell refers to, or 0 for "none" (and for a cell that is not an integer at
	// all, which is how a column that is not the expected one usually looks).
	//
	// Both signednesses count: the tables store a link in whatever width fits, and a sheet
	// whose link column happens to be signed (OrchestrionPath's File is) would otherwise read
	// as "no reference" for every row, silently yielding an empty cross-reference instead of
	// an error. A negative value is the tables' own "none".
	uint32_t referenced_row_id(const xivres::excel::cell* cell) {
		if (!cell)
			return 0;
		switch (cell->Type) {
			case xivres::excel::cell_type::UInt8: return cell->uint8;
			case xivres::excel::cell_type::UInt16: return cell->uint16;
			case xivres::excel::cell_type::UInt32: return cell->uint32;
			case xivres::excel::cell_type::Int8: return cell->int8 > 0 ? static_cast<uint32_t>(cell->int8) : 0;
			case xivres::excel::cell_type::Int16: return cell->int16 > 0 ? static_cast<uint32_t>(cell->int16) : 0;
			case xivres::excel::cell_type::Int32: return cell->int32 > 0 ? static_cast<uint32_t>(cell->int32) : 0;
			default: return 0;
		}
	}

	// The text of a cell, or empty for a cell that does not hold text.
	// parsed() rather than repr(): a name cell can carry rich-text payloads, and repr()
	// renders those as markup -- duty names came out as
	// `<payload type="Italics">...</payload>The Merchant's Tale...`, which is neither
	// readable nor matchable against an album's track title.
	std::string text_cell(const xivres::excel::cell* cell) {
		return cell && cell->Type == xivres::excel::cell_type::String ? cell->String.parsed() : std::string();
	}

	// What the game says a BGM file is used for: names, each prefixed with the sheet that
	// supplied it (e.g. "TerritoryType:The Tempest"), so a reader can tell why it is attached.
	// A set both sorts and dedupes, which is the contract describe_path() promises.
	struct bgm_usage {
		std::set<std::string> Names;
	};

	// Cross-references bgm <-> territorytype <-> contentfindercondition/placename, so a path
	// can be described by the places and duties it belongs to instead of being just a path.
	//
	// The field numbers below are those of the linked schemas
	// (https://github.com/xivdev/EXDSchema/tree/latest) and are resolved through
	// schema_field_columns(). Nothing is trusted blindly: the links have to resolve against the
	// sheet they are supposed to point at, and a table that does not look like the schema is
	// reported and skipped rather than used to attach names that only look plausible.
	std::map<std::string, bgm_usage> map_bgm_usage(const xivres::installation& installation, xivres::game_language language) {
		// Field numbers of the sheets this reads, in the order the linked schemas list them
		// (https://github.com/xivdev/EXDSchema/tree/latest), resolved through
		// schema_field_columns(). Fields after an array field are numbered past the end of
		// that array (a `type: array, count: N` field occupies N raw columns, and a nested
		// array of sub-fields N * (sub-field count)), because schema_field_columns() recovers
		// raw column order, not the yml's top-level list position; this was checked against
		// this repo's .github/columns.yml (the actual per-sheet raw column dump the schema's
		// own CI validates against) for every sheet below that has an array field ahead of the
		// field used (Fate, InstanceContent) before trusting the numbers:
		//   TerritoryType: 9 PlaceName, 12 BGM
		//   ContentFinderCondition: 0 Name, 54 TerritoryType
		//   PlaceName: 0 Name; BGMSituation: 0..4 daytime/night/battle/daybreak/twilight
		//   BGM: 0 File (found by value, see bgm_row_path)
		//   Fate: 0 Name, 91 Music (91 because StatusText[3], Unknown2[3] and
		//     ObjectiveIcon[32].{LayoutId,Icon} precede it)
		//   Mount: 0 Singular, 17 RideBGM (no arrays precede it)
		//   Leve: 0 Name, 20 BGM (no arrays precede it)
		//   InstanceContent: 46 BGM, 47 WinBGM, 48 ContentFinderCondition (BossExp[5] and
		//     BossCurrencyA/B/C[5] each precede them); it has no name field of its own --
		//     displayField in its schema is ContentFinderCondition, i.e. its name is the duty
		//     it belongs to, read via namesByConditionRow below
		//   WeddingBGM: 0 SongName, 1 Song (no arrays; direct-only, not BGMSituation)
		constexpr size_t TerritoryType_PlaceNameRegion = 7;
		constexpr size_t TerritoryType_PlaceNameZone = 8;
		constexpr size_t TerritoryType_PlaceName = 9;
		constexpr size_t TerritoryType_BGM = 12;
		constexpr size_t ContentFinderCondition_Name = 0;
		constexpr size_t ContentFinderCondition_TerritoryType = 54;
		constexpr size_t PlaceName_Name = 0;
		constexpr size_t Orchestrion_Name = 0;
		constexpr size_t OrchestrionPath_File = 0;
		constexpr size_t Fate_Name = 0;
		constexpr size_t Fate_Music = 91;
		constexpr size_t Mount_Singular = 0;
		constexpr size_t Mount_RideBGM = 17;
		constexpr size_t Leve_Name = 0;
		constexpr size_t Leve_BGM = 20;
		constexpr size_t InstanceContent_BGM = 46;
		constexpr size_t InstanceContent_WinBGM = 47;
		constexpr size_t InstanceContent_ContentFinderCondition = 48;
		constexpr size_t WeddingBGM_SongName = 0;
		constexpr size_t WeddingBGM_Song = 1;
		// A territory's BGM is either a BGM row, a BGMSituation row (the set of tracks a
		// territory switches between), or -- from this value up -- the n-th BGMSituation row.
		// All three forms are in use in the current tables.
		constexpr uint32_t SituationByIndexFrom = 50000;
		constexpr size_t SituationLinks = 5;

		try {
			const auto bgmSheet = installation.get_excel("bgm");
			const auto situationSheet = installation.get_excel("bgmsituation");
			const auto territorySheet = installation.get_excel("territorytype");
			const auto conditionSheet = installation.get_excel("contentfindercondition");

			std::map<uint32_t, std::string> pathsByBgmRow;
			for (size_t page = 0; page < bgmSheet.get_exh_reader().get_pages().size(); ++page)
				for (const auto& row : bgmSheet.get_exd_reader(page))
					if (auto path = bgm_row_path(row); !path.empty())
						pathsByBgmRow[row.row_id()] = std::move(path);

			// BGMSituation row -> the tracks it selects, and the rows in id order for the
			// "50000 + n" form of the reference.
			std::map<uint32_t, std::vector<std::string>> pathsBySituationRow;
			std::vector<uint32_t> situationRows;
			const auto situationColumns = schema_field_columns(situationSheet.get_exh_reader());
			for (size_t page = 0; page < situationSheet.get_exh_reader().get_pages().size(); ++page)
				for (const auto& row : situationSheet.get_exd_reader(page)) {
					situationRows.push_back(row.row_id());
					for (size_t field = 0; field < SituationLinks; ++field) {
						const auto bgmRow = referenced_row_id(schema_field(row, situationColumns, field));
						if (!bgmRow)
							continue;
						if (const auto path = pathsByBgmRow.find(bgmRow); path != pathsByBgmRow.end()
							&& !std::ranges::contains(pathsBySituationRow[row.row_id()], path->second))
								pathsBySituationRow[row.row_id()].push_back(path->second);
					}
				}
			std::ranges::sort(situationRows);

			// The sheets carrying text are read from the requested language's page, falling back to
			// the client's own when it does not ship that page.
			const auto read_names = [&](const std::string& sheetName, size_t nameField, const char* what) {
				const auto read = [&](const xivres::excel::reader& sheet) {
					std::map<uint32_t, std::string> names;
					const auto columns = schema_field_columns(sheet.get_exh_reader());
					for (size_t page = 0; page < sheet.get_exh_reader().get_pages().size(); ++page)
						for (const auto& row : sheet.get_exd_reader(page))
							if (auto name = text_cell(schema_field(row, columns, nameField)); !name.empty())
								names[row.row_id()] = std::move(name);
					return names;
				};
				if (language == xivres::game_language::Unspecified)
					return read(installation.get_excel(sheetName));
				try {
					return read(installation.get_excel(sheetName).new_with_language(language));
				} catch (const std::exception& e) {
					std::cerr << std::format("Warning: {} for {} are not available ({}) and the client's own are used instead.\n",
						what, game_language_code(language) ? game_language_code(language) : "", e.what());
					return read(installation.get_excel(sheetName));
				}
			};
			const auto namesByPlaceNameRow = read_names("placename", PlaceName_Name, "place names");
			const auto namesByConditionRow = read_names("contentfindercondition", ContentFinderCondition_Name, "duty names");

			// TerritoryType is what ties music to a place, and the duties of a territory are what
			// the same music gets attached to next.
			std::map<std::string, bgm_usage> usage;

			// The Orchestrion names a track the way the game itself prints it, which is the
			// piece's official title -- the only thing that ties a .scd path to a name
			// without listening to either, and so the only evidence available for music no
			// territory uses (boss themes, cutscene cues), which is exactly where the
			// place/duty cross-reference above finds nothing.
			//
			// Orchestrion and OrchestrionPath are parallel sheets: row n of OrchestrionPath
			// points at the BGM row that row n of Orchestrion names.
			try {
				const auto pathSheet = installation.get_excel("orchestrionpath");
				const auto namesByOrchestrionRow = read_names("orchestrion", Orchestrion_Name, "orchestrion track names");
				const auto pathColumns = schema_field_columns(pathSheet.get_exh_reader());
				size_t rolls = 0, resolved = 0;
				for (size_t page = 0; page < pathSheet.get_exh_reader().get_pages().size(); ++page)
					for (const auto& row : pathSheet.get_exd_reader(page)) {
						// File is the roll's own .scd path, not a reference to the BGM row the
						// zone uses -- the Orchestrion ships its own copy of each track.
						auto path = text_cell(schema_field(row, pathColumns, OrchestrionPath_File));
						if (path.empty() || !path.starts_with("music/") || !path.ends_with(".scd"))
							continue;
						++rolls;
						const auto name = namesByOrchestrionRow.find(row.row_id());
						if (name == namesByOrchestrionRow.end() || name->second.empty())
							continue;
						++resolved;
						usage[std::move(path)].Names.insert("Orchestrion:" + name->second);
					}
				// Silence here would be indistinguishable from "this game has no Orchestrion",
				// which is never true, so an empty result is reported rather than assumed.
				if (!rolls)
					std::cerr << "Warning: orchestrionpath yielded no BGM references; paths will carry no Orchestrion names.\n";
				else if (resolved * 10 < rolls * 9)
					std::cerr << std::format("Warning: orchestrion tables resolved only {}/{} rolls; Orchestrion names may be incomplete.\n", resolved, rolls);
			} catch (const std::exception& e) {
				// Losing Orchestrion names must not cost the other sheets' names.
				std::cerr << std::format("Warning: could not read the orchestrion tables ({}); paths will carry no Orchestrion names.\n", e.what());
			}

			std::map<uint32_t, std::vector<std::string>> pathsByTerritoryRow;
			size_t musicLinks = 0, resolvedMusicLinks = 0, placeNameLinks = 0, resolvedPlaceNameLinks = 0;
			const auto territoryColumns = schema_field_columns(territorySheet.get_exh_reader());
			for (size_t page = 0; page < territorySheet.get_exh_reader().get_pages().size(); ++page)
				for (const auto& row : territorySheet.get_exd_reader(page)) {
					const auto music = referenced_row_id(schema_field(row, territoryColumns, TerritoryType_BGM));
					if (!music)
						continue;
					++musicLinks;
					std::vector<std::string> paths;
					if (const auto bgm = pathsByBgmRow.find(music); bgm != pathsByBgmRow.end())
						paths.push_back(bgm->second);
					else if (music >= SituationByIndexFrom) {
						const auto index = music - SituationByIndexFrom;
						if (index < situationRows.size())
							paths = pathsBySituationRow[situationRows[index]];
					} else if (const auto situation = pathsBySituationRow.find(music); situation != pathsBySituationRow.end())
						paths = situation->second;
					if (paths.empty())
						continue; // music this client's tables do not know; see the check below
					++resolvedMusicLinks;
					pathsByTerritoryRow[row.row_id()] = paths;
					++placeNameLinks;
					// The territory's own place name, or the zone and then the region it is in, for
					// the interiors that have none of their own: an inn or a bar is not a place in
					// its own right, but the city it sits in is what names its music.
					std::string placeName;
					for (const auto field : {TerritoryType_PlaceName, TerritoryType_PlaceNameZone, TerritoryType_PlaceNameRegion}) {
						const auto placeNameRow = referenced_row_id(schema_field(row, territoryColumns, field));
						if (!placeNameRow)
							continue;
						const auto name = namesByPlaceNameRow.find(placeNameRow);
						if (name == namesByPlaceNameRow.end())
							continue;
						placeName = name->second;
						break;
					}
					if (placeName.empty())
						continue;
					++resolvedPlaceNameLinks;
					for (const auto& path : paths)
						usage[path].Names.insert("TerritoryType:" + placeName);
				}

			size_t dutyLinks = 0, resolvedDutyLinks = 0;
			const auto conditionColumns = schema_field_columns(conditionSheet.get_exh_reader());
			for (size_t page = 0; page < conditionSheet.get_exh_reader().get_pages().size(); ++page)
				for (const auto& row : conditionSheet.get_exd_reader(page)) {
					const auto territoryRow = referenced_row_id(schema_field(row, conditionColumns, ContentFinderCondition_TerritoryType));
					if (!territoryRow)
						continue;
					const auto name = namesByConditionRow.find(row.row_id());
					if (name == namesByConditionRow.end())
						continue;
					++dutyLinks;
					if (const auto paths = pathsByTerritoryRow.find(territoryRow); paths != pathsByTerritoryRow.end()) {
						++resolvedDutyLinks;
						for (const auto& path : paths->second)
							usage[path].Names.insert("ContentFinderCondition:" + name->second);
					}
				}

			// A sheet with its own name field and one field that links straight to a BGM row (not
			// BGMSituation -- none of these five multi-target the way TerritoryType.BGM does).
			// Self-contained per sheet, exactly like the Orchestrion block above: losing one of
			// these must not cost the others, so each gets its own try/catch and its own
			// zero-result warning rather than folding into the outer catch.
			const auto add_direct_bgm_names = [&](const char* sheetName, const char* prefix, size_t nameField, size_t bgmField) {
				try {
					const auto sheet = installation.get_excel(sheetName);
					const auto names = read_names(sheetName, nameField, prefix);
					const auto columns = schema_field_columns(sheet.get_exh_reader());
					size_t links = 0, resolved = 0;
					for (size_t page = 0; page < sheet.get_exh_reader().get_pages().size(); ++page)
						for (const auto& row : sheet.get_exd_reader(page)) {
							const auto bgmRow = referenced_row_id(schema_field(row, columns, bgmField));
							if (!bgmRow)
								continue;
							++links;
							const auto bgmPath = pathsByBgmRow.find(bgmRow);
							if (bgmPath == pathsByBgmRow.end())
								continue;
							const auto name = names.find(row.row_id());
							if (name == names.end() || name->second.empty())
								continue;
							++resolved;
							usage[bgmPath->second].Names.insert(std::format("{}:{}", prefix, name->second));
						}
					if (!links)
						std::cerr << std::format("Warning: {} yielded no BGM references; paths will carry no {} names.\n", sheetName, prefix);
					else if (resolved * 10 < links * 9)
						std::cerr << std::format("Warning: {} tables resolved only {}/{} links; {} names may be incomplete.\n", sheetName, resolved, links, prefix);
				} catch (const std::exception& e) {
					std::cerr << std::format("Warning: could not read {} ({}); paths will carry no {} names.\n", sheetName, e.what(), prefix);
				}
			};
			add_direct_bgm_names("fate", "Fate", Fate_Name, Fate_Music);
			add_direct_bgm_names("mount", "Mount", Mount_Singular, Mount_RideBGM);
			add_direct_bgm_names("leve", "Leve", Leve_Name, Leve_BGM);
			add_direct_bgm_names("weddingbgm", "WeddingBGM", WeddingBGM_SongName, WeddingBGM_Song);

			// InstanceContent carries no name field of its own (its schema's displayField is
			// ContentFinderCondition -- the duty it belongs to is its name), so both of its BGM
			// links borrow namesByConditionRow instead of a read_names() call of their own.
			try {
				const auto sheet = installation.get_excel("instancecontent");
				const auto columns = schema_field_columns(sheet.get_exh_reader());
				size_t links = 0, resolved = 0;
				for (size_t page = 0; page < sheet.get_exh_reader().get_pages().size(); ++page)
					for (const auto& row : sheet.get_exd_reader(page))
						for (const auto bgmField : {InstanceContent_BGM, InstanceContent_WinBGM}) {
							const auto bgmRow = referenced_row_id(schema_field(row, columns, bgmField));
							if (!bgmRow)
								continue;
							++links;
							const auto bgmPath = pathsByBgmRow.find(bgmRow);
							if (bgmPath == pathsByBgmRow.end())
								continue;
							const auto cfcRow = referenced_row_id(schema_field(row, columns, InstanceContent_ContentFinderCondition));
							if (!cfcRow)
								continue;
							const auto name = namesByConditionRow.find(cfcRow);
							if (name == namesByConditionRow.end() || name->second.empty())
								continue;
							++resolved;
							usage[bgmPath->second].Names.insert("InstanceContent:" + name->second);
						}
				if (!links)
					std::cerr << "Warning: instancecontent yielded no BGM references; paths will carry no InstanceContent names.\n";
				else if (resolved * 10 < links * 9)
					std::cerr << std::format("Warning: instancecontent tables resolved only {}/{} links; InstanceContent names may be incomplete.\n", resolved, links);
			} catch (const std::exception& e) {
				std::cerr << std::format("Warning: could not read instancecontent ({}); paths will carry no InstanceContent names.\n", e.what());
			}

			// What a table that no longer matches the schema looks like: references that mostly do
			// not resolve, or music that only a handful of territories use.
			const auto misdirected = [](size_t links, size_t resolved) { return links && resolved * 10 < links * 9; };
			if (pathsByBgmRow.empty() || pathsByTerritoryRow.size() < 50
				|| misdirected(musicLinks, resolvedMusicLinks)
				|| misdirected(placeNameLinks, resolvedPlaceNameLinks)
				|| misdirected(dutyLinks, resolvedDutyLinks))
				throw std::runtime_error(std::format(
					"territory/music tables do not look like the current schema (bgm rows {}, territories with music {}, music links {}/{}, place name links {}/{}, duty links {}/{})",
					pathsByBgmRow.size(), pathsByTerritoryRow.size(), resolvedMusicLinks, musicLinks, resolvedPlaceNameLinks, placeNameLinks, resolvedDutyLinks, dutyLinks));

			// A track used by everything is identified by nothing. The shared cues collect
			// absurd lists -- BGM_Leves.scd draws a name from all 1496 levequests, BGM_Null
			// 1027 duties -- and such a list says only "this is generic", at the cost of
			// swamping the output. Past a point the names stop narrowing anything down, so
			// that sheet's contribution to that path is dropped entirely rather than
			// truncated, which would imply the few kept were the relevant ones. Real reuse
			// stays well under this: the widest genuine case is a raid theme across ~14
			// InstanceContent tiers.
			constexpr size_t MaxNamesPerSheet = 32;
			for (auto& [path, entry] : usage) {
				// Owning keys, not views into the set being erased from.
				std::map<std::string, size_t> countsByPrefix;
				for (const auto& name : entry.Names)
					countsByPrefix[name.substr(0, name.find(':'))]++;
				std::erase_if(entry.Names, [&](const std::string& name) {
					const auto it = countsByPrefix.find(name.substr(0, name.find(':')));
					return it != countsByPrefix.end() && it->second > MaxNamesPerSheet;
				});
			}

			return usage;
		} catch (const std::exception& e) {
			std::cerr << std::format("Warning: could not cross-reference the game's sheets: {}. Paths will carry no names.", e.what()) << '\n';
			return {};
		}
	}
}

namespace {
	// The Orchestrion's own copies, which the bgm sheet does not list: it maps rows to the
	// tracks territories play, and a roll is a separate .scd the jukebox plays instead. They
	// are shipped music like any other, and are exactly the files the hand-written presets
	// cover under music/ffxiv/Orchestrion/, so leaving them out of discovery silently drops
	// several hundred replaceable tracks.
	void append_orchestrion_paths(const xivres::installation& installation, std::set<std::string>& paths) {
		try {
			const auto sheet = installation.get_excel("orchestrionpath");
			const auto columns = schema_field_columns(sheet.get_exh_reader());
			for (size_t page = 0; page < sheet.get_exh_reader().get_pages().size(); ++page)
				for (const auto& row : sheet.get_exd_reader(page)) {
					auto path = text_cell(schema_field(row, columns, 0));
					if (!path.empty() && path.starts_with("music/") && path.ends_with(".scd"))
						paths.insert(std::move(path));
				}
		} catch (const std::exception& e) {
			std::cerr << std::format("Warning: could not read orchestrionpath ({}); Orchestrion rolls will not be discovered.\n", e.what());
		}
	}
}

std::vector<std::string> enumerate_bgm_paths(const xivres::installation& installation) {
	// Deliberately the default reader, not new_with_language(): a client need not ship
	// every localized page file (this one only has exd/bgm_0.exd, not bgm_0_en.exd),
	// and the columns read here are paths, which are not localized anyway.
	const auto sheet = installation.get_excel("bgm");
	const auto& exh = sheet.get_exh_reader();

	std::set<std::string> paths;
	for (size_t pageIndex = 0; pageIndex < exh.get_pages().size(); ++pageIndex)
		for (const auto& row : sheet.get_exd_reader(pageIndex))
			if (auto path = bgm_row_path(row); !path.empty())
				paths.insert(std::move(path));

	append_orchestrion_paths(installation, paths);

	return {paths.begin(), paths.end()};
}

std::vector<std::string> describe_path(
	const xivres::installation& installation,
	const std::string& path,
	xivres::game_language language) {
	// Reading the sheets is the expensive part, and a caller asks about many paths against one
	// installation, so the cross-reference is kept: a single entry, keyed by the installation
	// object and the language, covers how this is called.
	static std::mutex cacheMutex;
	static std::shared_ptr<const std::map<std::string, bgm_usage>> cache;
	static const xivres::installation* cacheInstallation = nullptr;
	static auto cacheLanguage = xivres::game_language::Unspecified;

	std::shared_ptr<const std::map<std::string, bgm_usage>> usage;
	{
		const auto lock = std::scoped_lock(cacheMutex);
		if (!cache || cacheInstallation != &installation || cacheLanguage != language) {
			cache = std::make_shared<const std::map<std::string, bgm_usage>>(map_bgm_usage(installation, language));
			cacheInstallation = &installation;
			cacheLanguage = language;
		}
		usage = cache;
	}

	const auto it = usage->find(path);
	if (it == usage->end())
		return {};
	return {it->second.Names.begin(), it->second.Names.end()};
}
