#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <xivres/sound.h>

// Sound entries whose audio payload is not Vorbis, for a decoder-substitution hook.
//
// The entry stays format 6 -- the game's "Ogg" streamed format -- because that is the one
// every gate in the engine accepts for music, and because everything around the payload
// (the loop fields, the seek table, the stream offset) then keeps working unchanged. What
// differs is the stream the decoder is handed. The 0x20-byte codec-info block keeps its
// shape and its two sizes, so the entry still reads as [codec info][seek table][header
// region][data], but its version byte is 1, which is the value that names no obfuscation:
// versions 2 and 3 are the game's own and both XOR the header region, which a RIFF or fLaC
// header cannot survive.
//
//   wav   [44-byte RIFF/WAVE header]                 [raw interleaved 16-bit PCM]
//   flac  ["fLaC" + STREAMINFO + VORBIS_COMMENT]     [FLAC frames]
//
// Header region followed by data is a playable file of that format in both cases, which is
// exactly the relationship the Ogg path has between its header pages and its data pages --
// so `extract` unwraps all three the same way.
//
// Two things a decoder reading these should know. The seek table holds one anchor per 1024
// sample frames, and anchor k is the byte offset to start decoding at to reach sample
// k * 1024: for PCM that is the sample's own offset, and for FLAC it is the start of the
// frame holding it. And the FLAC frames are variable-blocksize ones -- sync FFF9, each
// header carrying the absolute index of its first sample rather than a frame number --
// because that is what makes an exact loop point legal; see the .cpp.
//
// The layout follows build_streamed_pcm_scd_full.py, the prototype the hook was written
// against; the .cpp says where it deviates and why.
namespace substitute_codec {

	// 16-bit, because 16-bit PCM is all the game's decoder ever emits: the sample buffer the
	// engine mixes from is int16 whatever the stream behind it was. Both builders take the
	// samples already quantised, so the caller decides once how floats become integers.
	//
	// `loopEndBlockIndex` is expected to be the end of the audio (the callers truncate there,
	// as the Vorbis path does), and 0/0 means the entry does not loop.
	xivres::sound::writer::sound_item make_pcm_entry(
		const std::vector<int16_t>& samples,
		size_t channels,
		size_t samplingRate,
		size_t loopStartBlockIndex,
		size_t loopEndBlockIndex);

	// `compressionLevel` is libFLAC's own 0 to 8. `reportOut` gets a one-line summary of what
	// the encode cost, in the shape the lossless Vorbis path reports.
	xivres::sound::writer::sound_item make_flac_entry(
		const std::vector<int16_t>& samples,
		size_t channels,
		size_t samplingRate,
		size_t loopStartBlockIndex,
		size_t loopEndBlockIndex,
		size_t compressionLevel,
		std::string& reportOut);

	// What a format-6 entry actually carries, by the magic at the start of its header region.
	// Anything the engine's own versions produce reads as Vorbis, so a file this tool did not
	// write is never mistaken for one that it did.
	enum class payload {
		Vorbis,
		Wave,
		Flac,
	};

	payload payload_of(const xivres::sound::reader::sound_item& item);

	// What the payload says about itself, read back out of its own header rather than out of
	// the entry's -- which is what makes it worth reading, since disagreement between the two
	// is exactly the kind of damage `--verify` is looking for. Zeroes for a Vorbis payload,
	// which has an Ogg decoder to answer this properly.
	struct payload_info {
		payload Kind = payload::Vorbis;
		size_t Channels = 0;
		size_t SamplingRate = 0;
		uint64_t TotalFrames = 0;
	};

	payload_info inspect(const xivres::sound::reader::sound_item& item);

	// Where the loop sits, in sample frames rather than in the byte offsets the entry header
	// states.
	//
	// A byte offset is a sample index again only where the payload is linear. For PCM that is
	// a division by the frame size. For FLAC it is not -- but the frames this writes are
	// variable-blocksize ones, so each frame header carries the absolute index of its own
	// first sample, and the encoder splits the stream so that a frame begins exactly at the
	// loop point. The number is therefore written down; it just has to be read out of the
	// frame rather than computed from the offset.
	//
	// Zeroes where the entry does not loop. `nullopt` where the payload is Vorbis, which has
	// an Ogg decoder to answer this properly, or where the bytes at the offset are not the
	// start of a frame this could have written -- an unreadable loop is reported as unknown
	// rather than guessed at from the seek table, whose anchors are only every 1024 frames.
	struct loop_samples {
		uint64_t Start = 0;
		uint64_t End = 0;
	};

	std::optional<loop_samples> loop_in_samples(const xivres::sound::reader::sound_item& item);

	// Header region followed by data, verbatim. Only for version-1 entries: the game's own
	// versions need the de-obfuscation that xivres's get_ogg_file() does, and this does none.
	std::vector<uint8_t> payload_file(const xivres::sound::reader::sound_item& item);

	// The file extension `payload_file` produced, for callers writing it out.
	const wchar_t* payload_extension(payload p);
}
