#pragma once

#include <cstdint>
#include <vector>

#include <xivres/sound.h>

// Sound entries whose payload is CRI HCA -- format 26, which the music folder never uses but
// `sound/` does. `sound/zingle/Zingle_Sleep.scd` is one, and nothing in the game's bgm sheet
// references it, so it only turns up when someone names the path directly.
//
// The entry is shaped like the Ogg one: a 0x18-byte prefix in ExtraData that is
// `sound_entry_ogg_header` under another name (Version 2, HeaderSize 0x18, and the block size
// where the Ogg form keeps its seek-table size), then the HCA header, then the frames in Data.
//
// The obfuscation is the surprise. The HCA header sits in ExtraData **in the clear** -- "HCA",
// "fmt", "comp", "ciph", "pad" are all readable in a hex dump -- while the frames are scrambled
// with the same version-3 XOR table the Ogg path uses, indexed as though the header had been
// scrambled too. So the frames descramble with `table[(size & 0x3F) + headerSize + i] ^
// (size & 0x7F)`, and the header is copied across untouched. Measured rather than assumed: with
// that offset all 378 frames of Zingle_Sleep begin with the 0xFF sync byte and ffmpeg decodes
// the result without a single error, where every other combination tried produced garbage.
//
// ffmpeg has had an HCA decoder since 2020, so reconstructing the standalone .hca file is all
// the rest of the pipeline needs -- everything downstream of the unwrap is ffmpeg already.
namespace hca_payload {

	// Whether this entry's payload is HCA. Reads the format field rather than sniffing, since
	// the format is what the engine dispatches on.
	bool is_hca(const xivres::sound::reader::sound_item& item);

	// The entry as a standalone .hca file: header region verbatim, frames descrambled.
	std::vector<uint8_t> payload_file(const xivres::sound::reader::sound_item& item);

	// What the HCA header says about itself, for callers that would otherwise have to trust
	// the entry header. Zeroes if the payload is not HCA or the header is unreadable.
	struct info {
		size_t Channels = 0;
		size_t SamplingRate = 0;
		uint64_t TotalFrames = 0;   // sample frames, after the encoder delay and padding
		size_t BlockSize = 0;
		size_t BlockCount = 0;
	};

	info inspect(const xivres::sound::reader::sound_item& item);
}
