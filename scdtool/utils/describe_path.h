#pragma once

#include <set>
#include <string>
#include <vector>

#include <xivres/common.h>

namespace xivres {
	class installation;
}

// Every music path the installed game knows about, read from its `bgm` excel sheet.
//
// xivres can only look files up by hash, so there is no way to enumerate the music folder
// directly; this sheet is the only way to discover target .scd paths, which is what makes
// presets for content that has none yet possible at all.
std::vector<std::string> enumerate_bgm_paths(const xivres::installation& installation);

// Describes one in-game music path with the game's own sheets: the places that play it
// (territorytype -> placename) and the duties whose territory has it as its music
// (contentfindercondition). A territory's music is either a BGM row, a BGMSituation row (the
// set of day/night/battle/daybreak/twilight tracks it switches between) or the n-th situation
// row, and all three forms are resolved.
//
// `language` picks the place-name page to read. game_language::Unspecified means the client's
// own, and a page the client does not ship falls back to that one, with a warning.
//
// Both outputs are cleared first and then filled with the names found, sorted and without
// duplicates. They stay empty for music that no territory uses, and also when the game's
// tables cannot be cross-referenced at all (reported on stderr), so a schema change yields no
// names rather than wrong ones.
void describe_path(
	const xivres::installation& installation,
	const std::string& path,
	xivres::game_language language,
	std::set<std::string>& placeNames,
	std::set<std::string>& dutyNames);
