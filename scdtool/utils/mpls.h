#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// One PlayItem() entry of a BDMV PlayList (.mpls) file: a reference to a clip
// (BDMV/STREAM/<ClipId>.m2ts, BDMV/CLIPINF/<ClipId>.clpi) and the portion of it
// that is played back, in 45kHz clock ticks.
struct mpls_play_item {
	std::string ClipId; // 5-digit clip id, e.g. "00000" for 00000.m2ts
	uint32_t InTime = 0;
	uint32_t OutTime = 0;

	[[nodiscard]] double duration_seconds() const {
		return OutTime >= InTime ? (OutTime - InTime) / 45000.0 : 0.;
	}
};

struct mpls_playlist {
	std::filesystem::path Path;
	std::vector<mpls_play_item> PlayItems;
};

// Parses a single .mpls file. Only the fixed-size, spec-mandated prefix of each
// PlayItem() is interpreted (clip id + in/out time); everything else (STN tables,
// multi-angle data, marks, sub paths) is skipped using the PlayItem's own length
// prefix, so this stays correct even though it does not decode those parts.
//
// Throws std::runtime_error if the file is not a recognizable .mpls file.
mpls_playlist parse_mpls(const std::filesystem::path& path);

// Parses every *.mpls file directly under <bdmvDir>/PLAYLIST, skipping (and not throwing for)
// any file that fails to parse. Returned in filename order.
std::vector<mpls_playlist> parse_all_playlists(const std::filesystem::path& bdmvDir);
