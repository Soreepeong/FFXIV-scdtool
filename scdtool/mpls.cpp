#include "pch.h"
#include "mpls.h"

namespace {
	uint16_t read_u16be(const std::vector<uint8_t>& d, size_t off) {
		if (off + 2 > d.size())
			throw std::runtime_error("mpls: truncated file (u16)");
		return static_cast<uint16_t>((d[off] << 8) | d[off + 1]);
	}

	uint32_t read_u32be(const std::vector<uint8_t>& d, size_t off) {
		if (off + 4 > d.size())
			throw std::runtime_error("mpls: truncated file (u32)");
		return (static_cast<uint32_t>(d[off]) << 24)
			| (static_cast<uint32_t>(d[off + 1]) << 16)
			| (static_cast<uint32_t>(d[off + 2]) << 8)
			| static_cast<uint32_t>(d[off + 3]);
	}
}

mpls_playlist parse_mpls(const std::filesystem::path& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f)
		throw std::runtime_error(std::format("mpls: could not open {}", xivres::util::unicode::convert<std::string>(path.wstring())));

	std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (data.size() < 40 || std::memcmp(data.data(), "MPLS", 4) != 0)
		throw std::runtime_error("mpls: not an MPLS file");

	const auto playListStart = read_u32be(data, 8);
	if (playListStart + 8 > data.size())
		throw std::runtime_error("mpls: PlayList_start_address out of range");

	// PlayList(): length(4) reserved(2) number_of_PlayItems(2) number_of_SubPaths(2)
	const auto numPlayItems = read_u16be(data, playListStart + 6);

	mpls_playlist result;
	result.Path = path;

	size_t p = playListStart + 10;
	for (uint16_t i = 0; i < numPlayItems; ++i) {
		// PlayItem(): length(2) Clip_Information_file_name(5) Clip_codec_identifier(4)
		//             reserved/is_multi_angle/connection_condition(2) ref_to_STC_id(1)
		//             IN_time(4) OUT_time(4) ... (rest skipped via length prefix)
		const auto itemLength = read_u16be(data, p);
		if (p + 2 + 18 > data.size())
			break;

		mpls_play_item item;
		item.ClipId.assign(reinterpret_cast<const char*>(&data[p + 2]), 5);
		item.InTime = read_u32be(data, p + 14) & 0x3FFFFFFF;
		item.OutTime = read_u32be(data, p + 18) & 0x3FFFFFFF;
		result.PlayItems.push_back(std::move(item));

		p += 2 + itemLength;
		if (itemLength == 0)
			break; // avoid an infinite loop on malformed input
	}

	return result;
}

std::vector<mpls_playlist> parse_all_playlists(const std::filesystem::path& bdmvDir) {
	std::vector<mpls_playlist> result;
	const auto playlistDir = bdmvDir / "PLAYLIST";
	if (!std::filesystem::is_directory(playlistDir))
		return result;

	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::directory_iterator(playlistDir)) {
		if (entry.is_regular_file() && xivres::util::unicode::convert<std::string>(entry.path().extension().wstring(), &xivres::util::unicode::lower) == ".mpls")
			files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());

	for (const auto& file : files) {
		try {
			result.push_back(parse_mpls(file));
		} catch (const std::exception&) {
			// best-effort: skip files that don't parse as expected
		}
	}
	return result;
}
