#include "pch.h"
#include "substitute_codec.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>

#include <FLAC/format.h>
#include <FLAC/stream_encoder.h>

#include <xivres/util.on_dtor.h>

namespace {

	// The codec-info block, unchanged in shape from the one the Ogg path writes: 0x20 bytes,
	// then a seek table of SeekTableSize, then a header region of VorbisHeaderSize. HeaderSize
	// has to stay 0x20 -- both the engine and xivres index the two regions from it, and
	// xivres rejects any other value outright.
	constexpr size_t CodecInfoSize = 0x20;
	static_assert(sizeof(xivres::sound::sound_entry_ogg_header) == CodecInfoSize);

	// Versions 2 and 3 are the game's own, and each names an obfuscation the engine undoes
	// over the header region -- a flat XOR for 2, a table-driven one over the whole stream for
	// 3. 1 names none, which is the only way a RIFF or fLaC header reaches the decoder intact.
	constexpr uint8_t VersionNoObfuscation = 1;

	// One seek anchor per this many sample frames, as the prototype's linear table used.
	//
	// The prototype sized the table at a fixed 6224 bytes -- 1556 anchors, which is what the
	// template it copied happened to carry -- and clamped every anchor past the end of the
	// stream to the end. Here the table is sized to the stream instead, so a five-minute track
	// has an anchor throughout rather than only over its first 36 seconds. That is the one
	// place this deviates from the prototype's constants, and it is a deviation the prototype
	// invites: "size is ours to choose".
	constexpr size_t SeekAnchorFrames = 1024;

	// An entry is addressed by 32-bit offsets throughout -- the stream size, the seek anchors,
	// the file size -- so a payload that does not fit in one has to be refused rather than
	// silently truncated. Raw PCM is the format that can actually reach this: 96 kHz stereo
	// runs 384 KB/s, so the ceiling is a little over three hours.
	constexpr size_t MaxPayloadBytes = 0xF0000000;

	// `trailingBytes` is whatever chunks follow the data chunk -- the tags -- which the RIFF
	// size has to cover and the data chunk's own size must not.
	std::vector<uint8_t> wave_header(size_t channels, size_t samplingRate, size_t dataBytes, size_t trailingBytes = 0) {
		const auto blockAlign = static_cast<uint16_t>(channels * sizeof(int16_t));
		std::vector<uint8_t> res;
		res.reserve(44);
		const auto u32 = [&res](uint32_t v) {
			for (int i = 0; i < 4; i++)
				res.push_back(static_cast<uint8_t>(v >> (8 * i)));
		};
		const auto u16 = [&res](uint16_t v) {
			for (int i = 0; i < 2; i++)
				res.push_back(static_cast<uint8_t>(v >> (8 * i)));
		};
		const auto tag = [&res](const char* s) { res.insert(res.end(), s, s + 4); };
		tag("RIFF");
		u32(static_cast<uint32_t>(36 + dataBytes + trailingBytes));
		tag("WAVE");
		tag("fmt ");
		u32(16);
		u16(1);  // WAVE_FORMAT_PCM. Deliberately not WAVE_FORMAT_EXTENSIBLE, which is what
		         // ffmpeg would write for more than two channels: the hook reads a fixed
		         // 44-byte header, and an extensible one is 60.
		u16(static_cast<uint16_t>(channels));
		u32(static_cast<uint32_t>(samplingRate));
		u32(static_cast<uint32_t>(samplingRate * blockAlign));
		u16(blockAlign);
		u16(16);
		tag("data");
		u32(static_cast<uint32_t>(dataBytes));
		return res;
	}

	// Anchor k is the byte offset of sample frame k * SeekAnchorFrames, which for a linear
	// payload is that multiplication and nothing else. The last anchor is the end of the
	// stream, so a seek past the audio lands on it rather than off the table.
	std::vector<uint32_t> linear_seek_table(size_t dataBytes, size_t frameBytes) {
		std::vector<uint32_t> res;
		const auto anchorBytes = SeekAnchorFrames * frameBytes;
		res.reserve(dataBytes / anchorBytes + 2);
		for (size_t offset = 0;; offset += anchorBytes) {
			res.push_back(static_cast<uint32_t>((std::min)(offset, dataBytes)));
			if (offset >= dataBytes)
				break;
		}
		return res;
	}

	xivres::sound::writer::sound_item assemble(
		const std::vector<uint8_t>& headerRegion,
		std::vector<uint8_t> data,
		const std::vector<uint32_t>& seekTable,
		size_t channels,
		size_t samplingRate,
		size_t loopStartOffset,
		size_t loopEndOffset) {

		const auto seekBytes = seekTable.size() * sizeof(uint32_t);
		if (data.size() > MaxPayloadBytes - seekBytes - headerRegion.size())
			throw std::runtime_error(std::format(
				"payload of {} bytes does not fit a .scd entry's 32-bit offsets", data.size()));

		std::vector<uint8_t> extra(CodecInfoSize + seekBytes + headerRegion.size());
		auto& info = *reinterpret_cast<xivres::sound::sound_entry_ogg_header*>(extra.data());
		info.Version = VersionNoObfuscation;
		info.HeaderSize = static_cast<uint8_t>(CodecInfoSize);
		info.SeekTableSize = static_cast<uint32_t>(seekBytes);
		info.VorbisHeaderSize = static_cast<uint32_t>(headerRegion.size());
		if (seekBytes)
			std::memcpy(&extra[CodecInfoSize], seekTable.data(), seekBytes);
		if (!headerRegion.empty())
			std::memcpy(&extra[CodecInfoSize + seekBytes], headerRegion.data(), headerRegion.size());

		return xivres::sound::writer::sound_item{
			.Header = {
				.StreamSize = static_cast<uint32_t>(data.size()),
				.ChannelCount = static_cast<uint32_t>(channels),
				.SamplingRate = static_cast<uint32_t>(samplingRate),
				.Format = xivres::sound::sound_entry_format::Ogg,
				// Byte offsets into the data, which is what a streamed entry's loop fields
				// are -- the same fields the Vorbis path fills with the offset of the page
				// the loop starts on.
				.LoopStartOffset = static_cast<uint32_t>(loopStartOffset),
				.LoopEndOffset = static_cast<uint32_t>(loopEndOffset),
				.StreamOffset = static_cast<uint32_t>(extra.size()),
				.Flags = xivres::sound::sound_entry_flags::None,
			},
			.ExtraData = std::move(extra),
			.Data = std::move(data),
		};
	}

	// ------------------------------------------------------------------------- FLAC ---

	constexpr auto Crc8Table = [] {
		std::array<uint8_t, 256> table{};
		for (size_t i = 0; i < 256; i++) {
			auto crc = static_cast<uint8_t>(i);
			for (int bit = 0; bit < 8; bit++)
				crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
			table[i] = crc;
		}
		return table;
	}();

	constexpr auto Crc16Table = [] {
		std::array<uint16_t, 256> table{};
		for (size_t i = 0; i < 256; i++) {
			auto crc = static_cast<uint16_t>(i << 8);
			for (int bit = 0; bit < 8; bit++)
				crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
			table[i] = crc;
		}
		return table;
	}();

	uint8_t crc8(std::span<const uint8_t> data) {
		uint8_t crc = 0;
		for (const auto b : data)
			crc = Crc8Table[crc ^ b];
		return crc;
	}

	uint16_t crc16(std::span<const uint8_t> data) {
		uint16_t crc = 0;
		for (const auto b : data)
			crc = static_cast<uint16_t>((crc << 8) ^ Crc16Table[(crc >> 8) ^ b]);
		return crc;
	}

	// FLAC codes the number in a frame header the way UTF-8 codes a code point, extended to
	// seven bytes so that a 36-bit sample index fits.
	void append_utf8_number(std::vector<uint8_t>& out, uint64_t value) {
		// One byte carries 7 bits, and each byte after the first trades a bit of the lead byte
		// for six of a continuation byte: 7, 11, 16, 21, 26, 31, 36. The steps are not uniform
		// -- the first is four bits and the rest are five -- so the widths are spelled out.
		static constexpr uint64_t Limits[6]{0x80, 0x800, 0x10000, 0x200000, 0x4000000, 0x80000000};
		size_t length = 1;
		for (const auto limit : Limits) {
			if (value < limit)
				break;
			length++;
		}
		if (length == 1) {
			out.push_back(static_cast<uint8_t>(value));
			return;
		}
		if (value >> (6 * (length - 1)) >> (7 - length))
			throw std::runtime_error("FLAC: sample index too large to code");
		out.push_back(static_cast<uint8_t>((0xFFu << (8 - length)) | static_cast<uint8_t>(value >> (6 * (length - 1)))));
		for (size_t i = length - 1; i > 0; i--)
			out.push_back(static_cast<uint8_t>(0x80 | ((value >> (6 * (i - 1))) & 0x3F)));
	}

	size_t utf8_number_length(uint8_t first) {
		if (first < 0x80) return 1;
		if ((first & 0xE0) == 0xC0) return 2;
		if ((first & 0xF0) == 0xE0) return 3;
		if ((first & 0xF8) == 0xF0) return 4;
		if ((first & 0xFC) == 0xF8) return 5;
		if ((first & 0xFE) == 0xFC) return 6;
		if (first == 0xFE) return 7;
		throw std::runtime_error("FLAC: malformed frame number");
	}

	// libFLAC only ever writes fixed-blocksize streams, where the frame header carries a
	// frame *number* and every frame but the last one is the same length. Splitting the
	// encode at the loop start -- which is what puts a frame boundary exactly there instead
	// of up to a blocksize short of it -- leaves a short frame in the middle of the stream,
	// and that is legal only in the other blocking strategy, where the header carries the
	// index of the frame's first sample. So each frame is rewritten into that form: the
	// strategy bit goes up, the number becomes the absolute sample index, and both checksums
	// are recomputed. Everything after the header -- the subframes -- is copied untouched.
	std::vector<uint8_t> rewrite_frame_header(std::span<const uint8_t> frame, uint64_t firstSample) {
		if (frame.size() < 10 || frame[0] != 0xFF || (frame[1] & 0xFE) != 0xF8)
			throw std::runtime_error("FLAC: encoder wrote something that is not a frame");

		auto pos = size_t{4} + utf8_number_length(frame[4]);
		const auto numberEnd = pos;
		switch (frame[2] >> 4) {
			case 6: pos += 1; break;   // blocksize - 1, 8 bits, follows the number
			case 7: pos += 2; break;   // blocksize - 1, 16 bits
			default: break;
		}
		switch (frame[2] & 0x0F) {
			case 12: pos += 1; break;  // sample rate in kHz, 8 bits
			case 13:
			case 14: pos += 2; break;  // sample rate in Hz / tens of Hz, 16 bits
			default: break;
		}
		if (pos + 1 > frame.size())
			throw std::runtime_error("FLAC: frame header runs past the frame");
		const auto headerEnd = pos + 1;  // the CRC-8 byte

		std::vector<uint8_t> res;
		res.reserve(frame.size() + 8);
		res.push_back(frame[0]);
		res.push_back(static_cast<uint8_t>(frame[1] | 0x01));  // variable blocksize
		res.push_back(frame[2]);
		res.push_back(frame[3]);
		append_utf8_number(res, firstSample);
		res.insert(res.end(), frame.begin() + static_cast<ptrdiff_t>(numberEnd),
			frame.begin() + static_cast<ptrdiff_t>(headerEnd - 1));
		res.push_back(crc8(res));

		// Everything between the header and the frame's own CRC-16 is bit-identical to what
		// the encoder produced, so only the trailing checksum has to follow the header.
		res.insert(res.end(), frame.begin() + static_cast<ptrdiff_t>(headerEnd), frame.end() - 2);
		const auto footer = crc16(res);
		res.push_back(static_cast<uint8_t>(footer >> 8));
		res.push_back(static_cast<uint8_t>(footer & 0xFF));
		return res;
	}

	struct flac_frame {
		size_t Offset;   // into the capture's bytes
		size_t Bytes;
		size_t Samples;
	};

	struct flac_capture {
		std::vector<uint8_t> Bytes;
		std::vector<flac_frame> Frames;
	};

	FLAC__StreamEncoderWriteStatus flac_write(
		const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes,
		uint32_t samples, uint32_t, void* clientData) {
		auto& capture = *static_cast<flac_capture*>(clientData);
		// samples == 0 is metadata. libFLAC writes a STREAMINFO it cannot go back and fill in
		// (there is no seek callback here) and a vendor comment; this builds both itself, from
		// what the encode actually produced, so its copies are dropped.
		if (!samples)
			return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
		capture.Frames.push_back({capture.Bytes.size(), bytes, samples});
		capture.Bytes.insert(capture.Bytes.end(), buffer, buffer + bytes);
		return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
	}

	void encode_span(
		const int16_t* samples, size_t frames, size_t channels, size_t samplingRate,
		size_t compressionLevel, flac_capture& out) {
		if (!frames)
			return;

		auto* const encoder = FLAC__stream_encoder_new();
		if (!encoder)
			throw std::runtime_error("FLAC__stream_encoder_new failed");
		const auto cleanup = xivres::util::on_dtor([encoder] { FLAC__stream_encoder_delete(encoder); });

		FLAC__stream_encoder_set_channels(encoder, static_cast<uint32_t>(channels));
		FLAC__stream_encoder_set_bits_per_sample(encoder, 16);
		FLAC__stream_encoder_set_sample_rate(encoder, static_cast<uint32_t>(samplingRate));
		FLAC__stream_encoder_set_compression_level(encoder, static_cast<uint32_t>(compressionLevel));
		FLAC__stream_encoder_set_total_samples_estimate(encoder, frames);
		// The encode is already verified end to end by --verify and by the caller's own
		// round trip, and libFLAC's verify runs a second decoder over everything.
		FLAC__stream_encoder_set_verify(encoder, false);
		// --sampling-rate can ask for a rate the streamable subset does not allow. Nothing
		// reads these streams but the hook, so a non-subset stream is a better answer than
		// refusing to build the entry at all.
		FLAC__stream_encoder_set_streamable_subset(encoder, false);

		if (const auto status = FLAC__stream_encoder_init_stream(encoder, flac_write, nullptr, nullptr, nullptr, &out);
			status != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
			throw std::runtime_error(std::format("FLAC__stream_encoder_init_stream: {}",
				FLAC__StreamEncoderInitStatusString[status]));

		// libFLAC takes one int32 per sample whatever the bit depth, so the buffer is fed in
		// chunks rather than converted whole: a six-channel 96 kHz track would otherwise want
		// four bytes per sample of the entire recording at once, on top of everything `apply`
		// is already holding.
		constexpr size_t ChunkFrames = 16384;
		std::vector<FLAC__int32> buffer(ChunkFrames * channels);
		for (size_t at = 0; at < frames;) {
			const auto count = (std::min)(ChunkFrames, frames - at);
			for (size_t i = 0; i < count * channels; i++)
				buffer[i] = samples[at * channels + i];
			if (!FLAC__stream_encoder_process_interleaved(encoder, buffer.data(), static_cast<uint32_t>(count)))
				throw std::runtime_error(std::format("FLAC__stream_encoder_process_interleaved: {}",
					FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(encoder)]));
			at += count;
		}
		if (!FLAC__stream_encoder_finish(encoder))
			throw std::runtime_error(std::format("FLAC__stream_encoder_finish: {}",
				FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(encoder)]));
	}

	// MD5 of the unencoded audio, which is the one field of STREAMINFO a decoder can check its
	// own work against. The format allows all-zero for "not computed", and an earlier version
	// of this wrote that -- but the hook's harness verifies the digest, and `flac -t` silently
	// skips the comparison when it is zero, so the field is worth the sixty lines.
	std::array<uint8_t, 16> md5(std::span<const uint8_t> data) {
		static constexpr uint32_t K[64]{
			0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
			0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
			0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
			0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
			0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
			0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
			0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
			0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
		};
		static constexpr int Shift[64]{
			7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
			5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
			4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
			6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
		};

		uint32_t h[4]{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
		const auto block = [&h](const uint8_t* at) {
			uint32_t m[16];
			for (size_t i = 0; i < 16; i++)
				m[i] = static_cast<uint32_t>(at[i * 4]) | (static_cast<uint32_t>(at[i * 4 + 1]) << 8)
					| (static_cast<uint32_t>(at[i * 4 + 2]) << 16) | (static_cast<uint32_t>(at[i * 4 + 3]) << 24);
			auto a = h[0], b = h[1], c = h[2], d = h[3];
			for (size_t i = 0; i < 64; i++) {
				uint32_t f;
				size_t g;
				if (i < 16) {
					f = (b & c) | (~b & d);
					g = i;
				} else if (i < 32) {
					f = (d & b) | (~d & c);
					g = (5 * i + 1) % 16;
				} else if (i < 48) {
					f = b ^ c ^ d;
					g = (3 * i + 5) % 16;
				} else {
					f = c ^ (b | ~d);
					g = (7 * i) % 16;
				}
				f += a + K[i] + m[g];
				a = d;
				d = c;
				c = b;
				b += (f << Shift[i]) | (f >> (32 - Shift[i]));
			}
			h[0] += a;
			h[1] += b;
			h[2] += c;
			h[3] += d;
		};

		size_t at = 0;
		for (; at + 64 <= data.size(); at += 64)
			block(data.data() + at);
		uint8_t tail[128]{};
		const auto rest = data.size() - at;
		if (rest)
			std::memcpy(tail, data.data() + at, rest);
		tail[rest] = 0x80;
		const auto tailSize = rest + 1 + 8 <= 64 ? size_t{64} : size_t{128};
		const auto bits = static_cast<uint64_t>(data.size()) * 8;
		for (size_t i = 0; i < 8; i++)
			tail[tailSize - 8 + i] = static_cast<uint8_t>(bits >> (8 * i));
		block(tail);
		if (tailSize == 128)
			block(tail + 64);

		std::array<uint8_t, 16> res{};
		for (size_t i = 0; i < 4; i++)
			for (size_t j = 0; j < 4; j++)
				res[i * 4 + j] = static_cast<uint8_t>(h[i] >> (8 * j));
		return res;
	}

	void append_metadata_block(std::vector<uint8_t>& out, uint8_t type, bool last, std::span<const uint8_t> body) {
		out.push_back(static_cast<uint8_t>((last ? 0x80 : 0x00) | type));
		out.push_back(static_cast<uint8_t>(body.size() >> 16));
		out.push_back(static_cast<uint8_t>(body.size() >> 8));
		out.push_back(static_cast<uint8_t>(body.size()));
		out.insert(out.end(), body.begin(), body.end());
	}

	// "fLaC" plus the two metadata blocks that make the frames playable: STREAMINFO, and a
	// VORBIS_COMMENT carrying the loop in samples. The loop tags are the same LoopStart /
	// LoopEnd the Vorbis path writes -- the entry's own loop fields are byte offsets, and a
	// decoder that wants to loop within a frame needs the sample index as well.
	//
	// No SEEKTABLE block: the entry's own seek table already indexes the frames, and a second
	// one inside the payload would only be another thing to keep consistent with it.
	std::vector<uint8_t> flac_header_region(
		size_t channels, size_t samplingRate, uint64_t totalSamples,
		size_t minBlockSize, size_t maxBlockSize, size_t minFrameSize, size_t maxFrameSize,
		size_t loopStartBlockIndex, size_t loopEndBlockIndex,
		std::span<const uint8_t> rawAudio,
		const std::vector<std::string>& comments) {

		std::array<uint8_t, 34> info{};
		const auto put16 = [&info](size_t at, size_t v) {
			info[at] = static_cast<uint8_t>(v >> 8);
			info[at + 1] = static_cast<uint8_t>(v);
		};
		const auto put24 = [&info](size_t at, size_t v) {
			info[at] = static_cast<uint8_t>(v >> 16);
			info[at + 1] = static_cast<uint8_t>(v >> 8);
			info[at + 2] = static_cast<uint8_t>(v);
		};
		put16(0, minBlockSize);
		put16(2, maxBlockSize);
		put24(4, minFrameSize);
		put24(7, maxFrameSize);
		// 20 bits of sample rate, 3 of channel count, 5 of bit depth and 36 of length, packed
		// into eight bytes with no byte boundary between them.
		const auto packed = (static_cast<uint64_t>(samplingRate) << 44)
			| (static_cast<uint64_t>(channels - 1) << 41)
			| (static_cast<uint64_t>(16 - 1) << 36)
			| totalSamples;
		for (size_t i = 0; i < 8; i++)
			info[10 + i] = static_cast<uint8_t>(packed >> (56 - 8 * i));
		const auto digest = md5(rawAudio);
		std::memcpy(&info[18], digest.data(), digest.size());

		std::vector<uint8_t> comment;
		{
			const std::string vendor = std::format("scdtool (libFLAC {})", FLAC__VERSION_STRING);
			std::vector<std::string> tags;
			if (loopStartBlockIndex || loopEndBlockIndex) {
				tags.push_back(std::format("LoopStart={}", loopStartBlockIndex));
				tags.push_back(std::format("LoopEnd={}", loopEndBlockIndex));
			}
			tags.insert(tags.end(), comments.begin(), comments.end());
			// Vorbis comments are little-endian even inside FLAC, which is big-endian
			// everywhere else.
			const auto u32le = [&comment](uint32_t v) {
				for (int i = 0; i < 4; i++)
					comment.push_back(static_cast<uint8_t>(v >> (8 * i)));
			};
			u32le(static_cast<uint32_t>(vendor.size()));
			comment.insert(comment.end(), vendor.begin(), vendor.end());
			u32le(static_cast<uint32_t>(tags.size()));
			for (const auto& tag : tags) {
				u32le(static_cast<uint32_t>(tag.size()));
				comment.insert(comment.end(), tag.begin(), tag.end());
			}
		}

		std::vector<uint8_t> res{'f', 'L', 'a', 'C'};
		append_metadata_block(res, 0, false, info);
		append_metadata_block(res, 4, true, comment);
		return res;
	}

	const xivres::sound::sound_entry_ogg_header* codec_info(const xivres::sound::reader::sound_item& item) {
		if (item.Header->Format != xivres::sound::sound_entry_format::Ogg)
			return nullptr;
		if (item.ExtraData.size() < CodecInfoSize)
			return nullptr;
		const auto* const info = reinterpret_cast<const xivres::sound::sound_entry_ogg_header*>(item.ExtraData.data());
		if (info->HeaderSize != CodecInfoSize)
			return nullptr;
		if (CodecInfoSize + static_cast<size_t>(info->SeekTableSize) + info->VorbisHeaderSize > item.ExtraData.size())
			return nullptr;
		return info;
	}
}

xivres::sound::writer::sound_item substitute_codec::make_pcm_entry(
	const std::vector<int16_t>& samples,
	size_t channels,
	size_t samplingRate,
	size_t loopStartBlockIndex,
	size_t loopEndBlockIndex,
	const std::vector<uint8_t>& trailingChunks) {

	if (!channels)
		throw std::runtime_error("wav: no channels");

	const auto frameBytes = channels * sizeof(int16_t);
	std::vector<uint8_t> data(samples.size() * sizeof(int16_t));
	if (!data.empty())
		std::memcpy(data.data(), samples.data(), data.size());

	const auto dataBytes = data.size();
	const auto header = wave_header(channels, samplingRate, dataBytes, trailingChunks.size());
	const auto seekTable = linear_seek_table(dataBytes, frameBytes);

	// The caller has already truncated the audio at the loop end, so looping to the end of
	// the payload is the whole of the mapping -- which for linear PCM is exact, with no
	// rounding onto a frame or page boundary of any kind.
	const auto loopStartOffset = loopEndBlockIndex
		? (std::min)(loopStartBlockIndex * frameBytes, dataBytes)
		: size_t{0};
	const auto loopEndOffset = loopEndBlockIndex ? dataBytes : size_t{0};

	// After the audio rather than before it: the hook walks the chunks up to "data" and stops
	// decoding at that chunk's own length, so a chunk behind it is never played, and the loop
	// offsets and seek anchors -- byte offsets into the stream -- stay where they were.
	data.insert(data.end(), trailingChunks.begin(), trailingChunks.end());
	return assemble(header, std::move(data), seekTable, channels, samplingRate, loopStartOffset, loopEndOffset);
}

xivres::sound::writer::sound_item substitute_codec::make_flac_entry(
	const std::vector<int16_t>& samples,
	size_t channels,
	size_t samplingRate,
	size_t loopStartBlockIndex,
	size_t loopEndBlockIndex,
	size_t compressionLevel,
	const std::vector<std::string>& comments,
	std::string& reportOut) {

	if (!channels)
		throw std::runtime_error("flac: no channels");
	const auto frames = samples.size() / channels;

	// Two encodes, split at the loop start, so that a frame begins exactly there. The
	// alternative is to point the entry's loop field at the frame that *contains* the loop
	// start, which would move the loop up to a blocksize -- 93 ms at 44.1 kHz -- early on
	// every looping track. The Vorbis path takes the same trouble, flushing an Ogg page at
	// the loop start rather than rounding to the nearest one.
	const auto splitAt = loopEndBlockIndex && loopStartBlockIndex && loopStartBlockIndex < frames
		? loopStartBlockIndex
		: size_t{0};

	flac_capture head, tail;
	if (splitAt)
		encode_span(samples.data(), splitAt, channels, samplingRate, compressionLevel, head);
	encode_span(samples.data() + splitAt * channels, frames - splitAt, channels, samplingRate, compressionLevel, tail);

	std::vector<uint8_t> data;
	data.reserve(head.Bytes.size() + tail.Bytes.size() + 64);
	std::vector<std::pair<uint64_t, size_t>> starts;  // first sample -> byte offset in `data`
	starts.reserve(head.Frames.size() + tail.Frames.size());
	size_t minBlockSize = SIZE_MAX, maxBlockSize = 0, minFrameSize = SIZE_MAX, maxFrameSize = 0;
	size_t loopStartOffset = 0;
	uint64_t at = 0;
	for (const auto* capture : {&head, &tail}) {
		for (const auto& frame : capture->Frames) {
			if (splitAt && at == splitAt)
				loopStartOffset = data.size();
			starts.emplace_back(at, data.size());
			const auto rewritten = rewrite_frame_header(
				std::span(capture->Bytes).subspan(frame.Offset, frame.Bytes), at);
			data.insert(data.end(), rewritten.begin(), rewritten.end());
			minBlockSize = (std::min)(minBlockSize, frame.Samples);
			maxBlockSize = (std::max)(maxBlockSize, frame.Samples);
			minFrameSize = (std::min)(minFrameSize, rewritten.size());
			maxFrameSize = (std::max)(maxFrameSize, rewritten.size());
			at += frame.Samples;
		}
	}
	if (at != frames)
		throw std::runtime_error(std::format("flac: encoder wrote {} samples, expected {}", at, frames));
	if (minBlockSize == SIZE_MAX)
		minBlockSize = maxBlockSize = minFrameSize = maxFrameSize = 0;

	const auto header = flac_header_region(channels, samplingRate, frames,
		minBlockSize, maxBlockSize, minFrameSize, maxFrameSize,
		loopStartBlockIndex, loopEndBlockIndex,
		std::span(reinterpret_cast<const uint8_t*>(samples.data()), samples.size() * sizeof(int16_t)),
		comments);

	// Anchor k addresses sample frame k * SeekAnchorFrames as the frame that holds it, which
	// is as close as a variable-length payload gets to the linear table PCM writes: a decoder
	// starting there has to drop at most a blocksize of samples to land on the anchor.
	std::vector<uint32_t> seekTable;
	{
		size_t index = 0;
		seekTable.reserve(frames / SeekAnchorFrames + 2);
		for (uint64_t anchor = 0;; anchor += SeekAnchorFrames) {
			if (anchor >= frames || starts.empty()) {
				seekTable.push_back(static_cast<uint32_t>(data.size()));
				break;
			}
			while (index + 1 < starts.size() && starts[index + 1].first <= anchor)
				index++;
			seekTable.push_back(static_cast<uint32_t>(starts[index].second));
		}
	}

	const auto rawBytes = samples.size() * sizeof(int16_t);
	reportOut = std::format("level {}, {:.2f} bits/sample, {:.0f}% of raw PCM, {} frames",
		compressionLevel,
		samples.empty() ? 0. : static_cast<double>(data.size()) * 8. / static_cast<double>(samples.size()),
		rawBytes ? 100. * static_cast<double>(data.size()) / static_cast<double>(rawBytes) : 0.,
		head.Frames.size() + tail.Frames.size());

	const auto loopEndOffset = loopEndBlockIndex ? data.size() : size_t{0};
	return assemble(header, std::move(data), seekTable, channels, samplingRate,
		loopEndBlockIndex ? loopStartOffset : 0, loopEndOffset);
}

substitute_codec::payload substitute_codec::payload_of(const xivres::sound::reader::sound_item& item) {
	const auto* const info = codec_info(item);
	if (!info || info->Version != VersionNoObfuscation)
		return payload::Vorbis;
	const auto header = item.ExtraData.subspan(CodecInfoSize + info->SeekTableSize, info->VorbisHeaderSize);
	if (header.size() >= 4 && std::memcmp(header.data(), "RIFF", 4) == 0)
		return payload::Wave;
	if (header.size() >= 4 && std::memcmp(header.data(), "fLaC", 4) == 0)
		return payload::Flac;
	return payload::Vorbis;
}

substitute_codec::payload_info substitute_codec::inspect(const xivres::sound::reader::sound_item& item) {
	payload_info res{.Kind = payload_of(item)};
	if (res.Kind == payload::Vorbis)
		return res;

	const auto* const info = codec_info(item);
	const auto header = item.ExtraData.subspan(CodecInfoSize + info->SeekTableSize, info->VorbisHeaderSize);
	if (res.Kind == payload::Wave) {
		// The header this writes is a fixed 44 bytes with the fmt chunk first, so the fields
		// sit at known offsets; anything else did not come from here.
		if (header.size() < 44)
			throw std::runtime_error("wav payload: header is shorter than 44 bytes");
		res.Channels = header[22] | (static_cast<size_t>(header[23]) << 8);
		for (size_t i = 0; i < 4; i++)
			res.SamplingRate |= static_cast<size_t>(header[24 + i]) << (8 * i);
		// From the data chunk's own size, not the stream's: tags can follow the audio.
		size_t dataBytes = 0;
		for (size_t i = 0; i < 4; i++)
			dataBytes |= static_cast<size_t>(header[40 + i]) << (8 * i);
		dataBytes = (std::min)(dataBytes, item.Data.size());
		const auto frameBytes = res.Channels * sizeof(int16_t);
		res.TotalFrames = frameBytes ? dataBytes / frameBytes : 0;
		return res;
	}

	// STREAMINFO is the first metadata block after "fLaC", and its last 21 bytes pack the
	// rate, channel count, bit depth and length with no byte boundaries between them.
	if (header.size() < 4 + 4 + 34)
		throw std::runtime_error("flac payload: header does not reach the end of STREAMINFO");
	uint64_t packed = 0;
	for (size_t i = 0; i < 8; i++)
		packed = (packed << 8) | header[4 + 4 + 10 + i];
	res.SamplingRate = static_cast<size_t>(packed >> 44);
	res.Channels = static_cast<size_t>((packed >> 41) & 0x7) + 1;
	res.TotalFrames = packed & 0xFFFFFFFFFULL;
	return res;
}

namespace {

	// FLAC codes the frame's sample (or frame) number the way UTF-8 codes a code point, with
	// one difference that matters here: it runs to 36 bits rather than Unicode's 21, so a
	// lead byte of 0xFE and six continuation bytes is legal where UTF-8 stops at 0xFD. A
	// 36-bit sample index is about 100 hours at 96 kHz, so the widths above four bytes are
	// unreachable in practice and are handled only because the format defines them.
	std::optional<uint64_t> read_coded_number(std::span<const uint8_t> at) {
		if (at.empty())
			return std::nullopt;
		const auto lead = at[0];
		size_t extra;
		uint64_t value;
		if (lead < 0x80) { extra = 0; value = lead; }
		else if ((lead & 0xE0) == 0xC0) { extra = 1; value = lead & 0x1Fu; }
		else if ((lead & 0xF0) == 0xE0) { extra = 2; value = lead & 0x0Fu; }
		else if ((lead & 0xF8) == 0xF0) { extra = 3; value = lead & 0x07u; }
		else if ((lead & 0xFC) == 0xF8) { extra = 4; value = lead & 0x03u; }
		else if ((lead & 0xFE) == 0xFC) { extra = 5; value = lead & 0x01u; }
		else if (lead == 0xFE) { extra = 6; value = 0; }
		else return std::nullopt;      // 0xFF is a continuation of nothing
		if (at.size() <= extra)
			return std::nullopt;
		for (size_t i = 1; i <= extra; i++) {
			if ((at[i] & 0xC0) != 0x80)
				return std::nullopt;
			value = (value << 6) | (at[i] & 0x3Fu);
		}
		return value;
	}

	// The absolute index of the first sample of the frame beginning at this byte.
	//
	// Only a variable-blocksize frame is accepted. The blocking strategy is the low bit of the
	// second sync byte, and it decides what the coded number *means*: FFF9 states a sample
	// index, FFF8 states a frame number, and nothing downstream distinguishes them. Reading an
	// FFF8 frame here would return a number about a thousand times too small and look
	// plausible, so the sync is checked exactly rather than masked.
	std::optional<uint64_t> flac_frame_sample(std::span<const uint8_t> data, size_t offset) {
		if (offset + 5 > data.size())
			return std::nullopt;
		if (data[offset] != 0xFF || data[offset + 1] != 0xF9)
			return std::nullopt;
		return read_coded_number(data.subspan(offset + 4));
	}

}

std::optional<substitute_codec::loop_samples> substitute_codec::loop_in_samples(
	const xivres::sound::reader::sound_item& item) {

	const auto kind = payload_of(item);
	if (kind == payload::Vorbis)
		return std::nullopt;

	const auto loopStart = static_cast<size_t>(item.Header->LoopStartOffset);
	const auto loopEnd = static_cast<size_t>(item.Header->LoopEndOffset);
	if (!loopEnd)
		return loop_samples{};      // 0/0: the entry states no loop

	const auto inspected = inspect(item);
	if (kind == payload::Wave) {
		const auto frameBytes = inspected.Channels * sizeof(int16_t);
		if (!frameBytes)
			return std::nullopt;
		return loop_samples{loopStart / frameBytes, loopEnd / frameBytes};
	}

	const auto start = flac_frame_sample(item.Data, loopStart);
	if (!start)
		return std::nullopt;
	// The end offset is one past the last frame -- `make_flac_entry` truncates the audio at
	// the loop end, as the Vorbis path does -- so there is no frame header there to read and
	// the answer is the stream's own length. Anything short of that is a real frame boundary
	// and is read like the start.
	if (loopEnd >= item.Data.size())
		return loop_samples{*start, inspected.TotalFrames};
	const auto end = flac_frame_sample(item.Data, loopEnd);
	if (!end)
		return std::nullopt;
	return loop_samples{*start, *end};
}

std::vector<uint8_t> substitute_codec::payload_file(const xivres::sound::reader::sound_item& item) {
	const auto* const info = codec_info(item);
	if (!info || info->Version != VersionNoObfuscation)
		throw std::runtime_error("not a substituted-codec entry");
	const auto header = item.ExtraData.subspan(CodecInfoSize + info->SeekTableSize, info->VorbisHeaderSize);
	std::vector<uint8_t> res;
	res.reserve(header.size() + item.Data.size());
	res.insert(res.end(), header.begin(), header.end());
	res.insert(res.end(), item.Data.begin(), item.Data.end());
	return res;
}

const wchar_t* substitute_codec::payload_extension(payload p) {
	switch (p) {
		case payload::Wave: return L".wav";
		case payload::Flac: return L".flac";
		default: return L".ogg";
	}
}
