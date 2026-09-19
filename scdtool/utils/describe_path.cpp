#include "pch.h"
#include "describe_path.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>

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

	// The row id a cell refers to, or 0 for "none" (and for a cell that is not an unsigned
	// integer at all, which is how a column that is not the expected one usually looks).
	uint32_t referenced_row_id(const xivres::excel::cell* cell) {
		if (!cell)
			return 0;
		if (cell->Type == xivres::excel::cell_type::UInt8)
			return cell->uint8;
		if (cell->Type == xivres::excel::cell_type::UInt16)
			return cell->uint16;
		if (cell->Type == xivres::excel::cell_type::UInt32)
			return cell->uint32;
		return 0;
	}

	// The text of a cell, or empty for a cell that does not hold text.
	std::string text_cell(const xivres::excel::cell* cell) {
		return cell && cell->Type == xivres::excel::cell_type::String ? cell->String.repr() : std::string();
	}

	// What the game says a BGM file is used for.
	struct bgm_usage {
		std::set<std::string> PlaceNames;
		std::set<std::string> DutyNames;
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
		// schema_field_columns():
		//   TerritoryType: 9 PlaceName, 12 BGM
		//   ContentFinderCondition: 0 Name, 54 TerritoryType
		//   PlaceName: 0 Name; BGMSituation: 0..4 daytime/night/battle/daybreak/twilight
		//   BGM: 0 File (found by value, see bgm_row_path)
		constexpr size_t TerritoryType_PlaceNameRegion = 7;
		constexpr size_t TerritoryType_PlaceNameZone = 8;
		constexpr size_t TerritoryType_PlaceName = 9;
		constexpr size_t TerritoryType_BGM = 12;
		constexpr size_t ContentFinderCondition_Name = 0;
		constexpr size_t ContentFinderCondition_TerritoryType = 54;
		constexpr size_t PlaceName_Name = 0;
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
						usage[path].PlaceNames.insert(placeName);
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
							usage[path].DutyNames.insert(name->second);
					}
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

			return usage;
		} catch (const std::exception& e) {
			std::cerr << std::format("Warning: could not cross-reference the game's sheets: {}. Paths will carry no placeNames/dutyNames.", e.what()) << '\n';
			return {};
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

	return {paths.begin(), paths.end()};
}

void describe_path(
	const xivres::installation& installation,
	const std::string& path,
	xivres::game_language language,
	std::set<std::string>& placeNames,
	std::set<std::string>& dutyNames) {
	placeNames.clear();
	dutyNames.clear();

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
		return;
	placeNames = it->second.PlaceNames;
	dutyNames = it->second.DutyNames;
}
