#include "pch.h"
#include "hca_payload.h"

#include <cstring>
#include <stdexcept>

namespace {

	constexpr uint32_t FormatHca = 26;

	// Where "HCA\0" begins inside ExtraData. The prefix before it is the same 0x18-byte block
	// the Ogg entries carry, and its HeaderSize field states that length -- but the magic is
	// searched for rather than trusted, because a prefix that disagreed with the payload would
	// otherwise be copied into the .hca and hand ffmpeg a file starting mid-header.
	size_t hca_header_offset(std::span<const uint8_t> extra) {
		static constexpr uint8_t Magic[4]{'H', 'C', 'A', 0};
		for (size_t i = 0; i + sizeof Magic <= extra.size(); i++)
			if (std::memcmp(extra.data() + i, Magic, sizeof Magic) == 0)
				return i;
		return SIZE_MAX;
	}

}

bool hca_payload::is_hca(const xivres::sound::reader::sound_item& item) {
	return item.Header && static_cast<uint32_t>(*item.Header->Format) == FormatHca;
}

std::vector<uint8_t> hca_payload::payload_file(const xivres::sound::reader::sound_item& item) {
	if (!is_hca(item))
		throw std::runtime_error("not an HCA sound entry");
	const auto at = hca_header_offset(item.ExtraData);
	if (at == SIZE_MAX)
		throw std::runtime_error("hca: no HCA header in the entry's extra data");

	const auto header = item.ExtraData.subspan(at);
	const auto& data = item.Data;

	// The same two bytes the Ogg version-3 path derives, from the size of the audio.
	const auto byte1 = static_cast<uint8_t>(data.size() & 0x7F);
	const auto byte2 = static_cast<uint8_t>(data.size() & 0x3F);

	std::vector<uint8_t> res;
	res.reserve(header.size() + data.size());
	res.insert(res.end(), header.begin(), header.end());
	for (size_t i = 0; i < data.size(); i++)
		res.push_back(static_cast<uint8_t>(data[i]
			^ xivres::sound::sound_entry_ogg_header::Version3XorTable[(byte2 + header.size() + i) & 0xFF]
			^ byte1));
	return res;
}

hca_payload::info hca_payload::inspect(const xivres::sound::reader::sound_item& item) {
	info res;
	if (!is_hca(item))
		return res;
	const auto at = hca_header_offset(item.ExtraData);
	if (at == SIZE_MAX)
		return res;
	const auto header = item.ExtraData.subspan(at);

	// HCA writes its section fields big-endian, unlike everything else in the container.
	const auto be16 = [&header](size_t i) {
		return static_cast<size_t>(header[i]) << 8 | header[i + 1];
	};
	const auto find = [&header](const char* tag) -> size_t {
		for (size_t i = 0; i + 4 <= header.size(); i++)
			if (std::memcmp(header.data() + i, tag, 4) == 0)
				return i;
		return SIZE_MAX;
	};

	if (const auto fmt = find("fmt\0"); fmt != SIZE_MAX && fmt + 16 <= header.size()) {
		res.Channels = header[fmt + 4];
		res.SamplingRate = static_cast<size_t>(header[fmt + 5]) << 16
			| static_cast<size_t>(header[fmt + 6]) << 8 | header[fmt + 7];
		res.BlockCount = static_cast<size_t>(header[fmt + 8]) << 24
			| static_cast<size_t>(header[fmt + 9]) << 16
			| static_cast<size_t>(header[fmt + 10]) << 8 | header[fmt + 11];
		// Every block carries 1024 sample frames; the first and last are partly the encoder's
		// own lead-in and padding, which the decoder drops.
		const auto delay = be16(fmt + 12), padding = be16(fmt + 14);
		const auto total = res.BlockCount * 1024;
		res.TotalFrames = total > delay + padding ? total - delay - padding : 0;
	}
	if (const auto comp = find("comp"); comp != SIZE_MAX && comp + 6 <= header.size())
		res.BlockSize = be16(comp + 4);
	return res;
}
