#pragma once

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

// Describes one in-game music path with the game's own sheets: every sheet of the ten that
// link to BGM and that carries a usable human-readable name -- territorytype (place),
// contentfindercondition (duty, both via a territory's own music and via instancecontent's
// direct BGM/WinBGM), instancecontent (duty), fate, mount, leve, weddingbgm -- plus the title
// the Orchestrion prints for a track (orchestrionpath -> orchestrion). A territory's music is
// either a BGM row, a BGMSituation row (the set of day/night/battle/daybreak/twilight tracks
// it switches between) or the n-th situation row, and all three forms are resolved.
//
// Each returned entry is "<SheetName>:<name>", so a reader can tell which sheet -- and so
// which reason the file is attached to that name -- supplied it, e.g. "TerritoryType:The
// Tempest" vs. "Orchestrion:A New Hope" for the same path. bgmswitch, chocoboraceranking,
// minigamera carry no name field at all and are not sources of any entry.
//
// `language` picks the name page to read. game_language::Unspecified means the client's own,
// and a page the client does not ship falls back to that one, with a warning.
//
// The result is sorted and without duplicates. It is empty for music no sheet references, and
// also when the game's tables cannot be cross-referenced at all (reported on stderr), so a
// schema change yields no names rather than wrong ones.
std::vector<std::string> describe_path(
	const xivres::installation& installation,
	const std::string& path,
	xivres::game_language language);
