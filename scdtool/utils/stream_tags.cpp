#include "pch.h"
#include "stream_tags.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <ogg/ogg.h>

#include "win32_process.h"

namespace {

	std::string upper(std::string text) {
		std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		return text;
	}

	// ffprobe reports a few fields under its own names rather than the file's; these are the
	// Vorbis comment names for them, which is what every payload here stores.
	std::string canonical_key(const std::string& key) {
		auto k = upper(key);
		if (k == "TRACK")
			return "TRACKNUMBER";
		if (k == "DISC")
			return "DISCNUMBER";
		if (k == "ALBUM_ARTIST" || k == "ALBUM ARTIST")
			return "ALBUMARTIST";
		return k;
	}

	// Fields that describe the recording's file rather than its music. The loop ones would
	// contradict the replacement's own, which the Vorbis and FLAC paths write, and the
	// loudness ones would be wrong: the replacement is gain-matched to the game's own file,
	// so a player applying the album's ReplayGain to it would move it off that level again.
	bool is_file_field(const std::string& key) {
		return key == "LOOPSTART" || key == "LOOPEND" || key == "ENCODER" || key == "ENCODED_BY"
			|| key == "VENDOR" || key == "MAJOR_BRAND" || key == "MINOR_VERSION" || key == "COMPATIBLE_BRANDS"
			|| key.starts_with("REPLAYGAIN_") || key.starts_with("R128_")
			|| key == "ITUNNORM" || key == "ITUNSMPB";
	}

	std::string trimmed(std::string_view text) {
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
			text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
			text.remove_suffix(1);
		return std::string(text);
	}

	uint32_t u32le(const uint8_t* p) {
		return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
	}

	void put_u32le(std::vector<uint8_t>& out, uint32_t v) {
		for (int i = 0; i < 4; i++)
			out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}

	void put_synchsafe(std::vector<uint8_t>& out, size_t v) {
		if (v >= (1u << 28))
			throw std::runtime_error("id3: tag too large");
		for (int shift = 21; shift >= 0; shift -= 7)
			out.push_back(static_cast<uint8_t>((v >> shift) & 0x7F));
	}

	// Every page of an Ogg stream in `bytes`, as (offset, length).
	std::vector<std::pair<size_t, size_t>> ogg_pages(std::span<const uint8_t> bytes) {
		std::vector<std::pair<size_t, size_t>> res;
		for (size_t at = 0; at < bytes.size();) {
			if (at + 27 > bytes.size() || std::memcmp(&bytes[at], "OggS", 4) != 0)
				throw std::runtime_error(std::format("ogg: no page at byte {}", at));
			const size_t segments = bytes[at + 26];
			if (at + 27 + segments > bytes.size())
				throw std::runtime_error(std::format("ogg: page at byte {} is cut short", at));
			size_t length = 27 + segments;
			for (size_t i = 0; i < segments; i++)
				length += bytes[at + 27 + i];
			if (at + length > bytes.size())
				throw std::runtime_error(std::format("ogg: page at byte {} is cut short", at));
			res.emplace_back(at, length);
			at += length;
		}
		return res;
	}
}

std::vector<stream_tags::field> stream_tags::read(const std::filesystem::path& ffprobe, const std::filesystem::path& file) {
	nlohmann::json json;
	try {
		const auto out = run_process_capture_stdout(ffprobe, {
			L"-v", L"error",
			L"-select_streams", L"a:0",
			L"-show_entries", L"format_tags:stream_tags",
			L"-of", L"json",
			file.wstring(),
		});
		json = nlohmann::json::parse(std::string(out.begin(), out.end()), nullptr, false);
	} catch (const std::exception&) {
		return {};
	}
	if (json.is_discarded())
		return {};

	// Container tags first: they are where FLAC, MP3 and WAV keep theirs. An Ogg file keeps
	// them on the stream, and would otherwise show none.
	std::vector<std::vector<field>> parts(2);
	const auto collect = [](const nlohmann::json& tags, std::vector<field>& into) {
		if (!tags.is_object())
			return;
		for (const auto& [key, value] : tags.items()) {
			if (!value.is_string())
				continue;
			auto k = canonical_key(key);
			if (k.empty() || is_file_field(k))
				continue;
			// ffprobe reports a field the file states several times as one string, the values
			// joined by ';'. Split back, so that each value is compared on its own when two
			// recordings share some of them.
			const auto joined = value.get<std::string>();
			for (size_t begin = 0; begin <= joined.size();) {
				auto end = joined.find(';', begin);
				if (end == std::string::npos)
					end = joined.size();
				if (auto one = trimmed(std::string_view(joined).substr(begin, end - begin)); !one.empty())
					into.emplace_back(k, std::move(one));
				begin = end + 1;
			}
		}
	};
	if (const auto format = json.find("format"); format != json.end() && format->contains("tags"))
		collect(format->at("tags"), parts[0]);
	if (const auto streams = json.find("streams"); streams != json.end() && streams->is_array() && !streams->empty()
		&& streams->front().contains("tags"))
		collect(streams->front().at("tags"), parts[1]);
	return merge(parts);
}

std::vector<stream_tags::field> stream_tags::merge(const std::vector<std::vector<field>>& perRecording) {
	std::vector<std::string> keyOrder;
	std::map<std::string, std::vector<std::string>> values;
	for (const auto& recording : perRecording) {
		for (const auto& [key, value] : recording) {
			auto& list = values[key];
			if (list.empty())
				keyOrder.push_back(key);
			if (std::ranges::find(list, value) == list.end())
				list.push_back(value);
		}
	}
	std::vector<field> res;
	for (const auto& key : keyOrder)
		for (const auto& value : values[key])
			res.emplace_back(key, value);
	return res;
}

std::vector<std::string> stream_tags::vorbis_comments(const std::vector<field>& fields) {
	std::vector<std::string> res;
	res.reserve(fields.size());
	for (const auto& [key, value] : fields)
		res.push_back(key + "=" + value);
	return res;
}

xivres::sound::writer::sound_item stream_tags::with_vorbis_comments(
	xivres::sound::writer::sound_item entry,
	const std::vector<std::string>& comments) {

	if (comments.empty())
		return entry;
	if (entry.Header.Format != xivres::sound::sound_entry_format::Ogg)
		throw std::runtime_error("tags: not an Ogg entry");
	auto& extra = entry.ExtraData;
	if (extra.size() < sizeof(xivres::sound::sound_entry_ogg_header))
		throw std::runtime_error("tags: Ogg entry has no codec header");
	auto* info = reinterpret_cast<xivres::sound::sound_entry_ogg_header*>(extra.data());
	// Version 3 and a nonzero EncodeByte both scramble the header pages; the writers here
	// never produce either, and rewriting scrambled bytes would corrupt them.
	if (info->Version != 2 || info->EncodeByte != 0)
		throw std::runtime_error(std::format("tags: Ogg entry version {} / encode byte {} is obfuscated",
			int{info->Version}, int{info->EncodeByte}));
	const size_t headerAt = size_t{info->HeaderSize} + *info->SeekTableSize;
	const size_t headerSize = *info->VorbisHeaderSize;
	if (headerAt + headerSize != extra.size())
		throw std::runtime_error("tags: Ogg header pages do not end the codec header");
	const std::span<const uint8_t> oldHeader(extra.data() + headerAt, headerSize);
	const auto oldPages = ogg_pages(oldHeader);
	if (oldPages.empty())
		throw std::runtime_error("tags: Ogg entry has no header pages");

	// The three header packets, reassembled from however many pages hold them.
	ogg_sync_state oy{};
	ogg_sync_init(&oy);
	ogg_stream_state os{};
	const int serial = static_cast<int>(u32le(&oldHeader[14]));
	ogg_stream_init(&os, serial);
	std::vector<std::vector<uint8_t>> packets;
	{
		const auto buffer = ogg_sync_buffer(&oy, static_cast<long>(oldHeader.size()));
		std::memcpy(buffer, oldHeader.data(), oldHeader.size());
		ogg_sync_wrote(&oy, static_cast<long>(oldHeader.size()));
		ogg_page og{};
		ogg_packet op{};
		while (ogg_sync_pageout(&oy, &og) == 1) {
			ogg_stream_pagein(&os, &og);
			while (ogg_stream_packetout(&os, &op) == 1)
				packets.emplace_back(op.packet, op.packet + op.bytes);
		}
	}
	ogg_stream_clear(&os);
	ogg_sync_clear(&oy);
	if (packets.size() != 3 || packets[1].size() < 7 + 8 || packets[1][0] != 3 || std::memcmp(&packets[1][1], "vorbis", 6) != 0)
		throw std::runtime_error(std::format("tags: header pages hold {} packets, not identification, comment and setup",
			packets.size()));

	// The comment packet, rebuilt with the new comments after the ones it had: the vendor
	// string and the loop tags stay, in their place.
	const auto& old = packets[1];
	size_t at = 7;
	const auto vendorLength = u32le(&old[at]);
	at += 4;
	if (at + vendorLength + 4 > old.size())
		throw std::runtime_error("tags: comment header is cut short");
	std::vector<uint8_t> comment(old.begin(), old.begin() + static_cast<ptrdiff_t>(at + vendorLength));
	at += vendorLength;
	const auto oldCount = u32le(&old[at]);
	at += 4;
	std::vector<std::span<const uint8_t>> kept;
	for (uint32_t i = 0; i < oldCount; i++) {
		if (at + 4 > old.size())
			throw std::runtime_error("tags: comment header is cut short");
		const auto length = u32le(&old[at]);
		if (at + 4 + length > old.size())
			throw std::runtime_error("tags: comment header is cut short");
		kept.emplace_back(&old[at + 4], length);
		at += 4 + length;
	}
	put_u32le(comment, static_cast<uint32_t>(kept.size() + comments.size()));
	for (const auto& one : kept) {
		put_u32le(comment, static_cast<uint32_t>(one.size()));
		comment.insert(comment.end(), one.begin(), one.end());
	}
	for (const auto& one : comments) {
		put_u32le(comment, static_cast<uint32_t>(one.size()));
		comment.insert(comment.end(), one.begin(), one.end());
	}
	comment.push_back(1);  // framing bit
	packets[1] = std::move(comment);

	// Paged the way xivres pages the headers it encodes, so a replacement differs from an
	// untagged one only in the comment it carries.
	std::vector<uint8_t> newHeader;
	size_t newPageCount = 0;
	ogg_stream_init(&os, serial);
	for (size_t i = 0; i < packets.size(); i++) {
		ogg_packet op{};
		op.packet = packets[i].data();
		op.bytes = static_cast<long>(packets[i].size());
		op.b_o_s = i == 0;
		op.granulepos = 0;
		op.packetno = static_cast<ogg_int64_t>(i);
		ogg_stream_packetin(&os, &op);
	}
	for (ogg_page og{}; ogg_stream_flush_fill(&os, &og, 0) > 0; newPageCount++) {
		newHeader.insert(newHeader.end(), og.header, og.header + og.header_len);
		newHeader.insert(newHeader.end(), og.body, og.body + og.body_len);
	}
	ogg_stream_clear(&os);

	// Every audio page carries its sequence number, so a header that now takes a different
	// number of pages shifts all of them; a decoder takes a gap there for lost data.
	if (const auto shift = static_cast<int64_t>(newPageCount) - static_cast<int64_t>(oldPages.size())) {
		for (const auto& [offset, length] : ogg_pages(entry.Data)) {
			auto* page = &entry.Data[offset];
			const auto sequence = static_cast<uint32_t>(static_cast<int64_t>(u32le(page + 18)) + shift);
			for (int i = 0; i < 4; i++)
				page[18 + i] = static_cast<uint8_t>(sequence >> (8 * i));
			ogg_page og{};
			og.header = page;
			og.header_len = 27 + page[26];
			og.body = page + og.header_len;
			og.body_len = static_cast<long>(length) - og.header_len;
			ogg_page_checksum_set(&og);
		}
	}

	extra.resize(headerAt);
	extra.insert(extra.end(), newHeader.begin(), newHeader.end());
	info = reinterpret_cast<xivres::sound::sound_entry_ogg_header*>(extra.data());
	info->VorbisHeaderSize = static_cast<uint32_t>(newHeader.size());
	return entry;
}

std::vector<uint8_t> stream_tags::id3_riff_chunk(const std::vector<field>& fields) {
	if (fields.empty())
		return {};

	// The Vorbis names with an ID3v2.4 text frame of their own. Anything else becomes a
	// TXXX frame under its Vorbis name, which is how taggers carry fields ID3 has no name for.
	static const std::map<std::string, std::string> FrameOf{
		{"TITLE", "TIT2"}, {"ARTIST", "TPE1"}, {"ALBUM", "TALB"}, {"ALBUMARTIST", "TPE2"},
		{"TRACKNUMBER", "TRCK"}, {"DISCNUMBER", "TPOS"}, {"DATE", "TDRC"}, {"GENRE", "TCON"},
		{"COMPOSER", "TCOM"}, {"COPYRIGHT", "TCOP"}, {"PUBLISHER", "TPUB"}, {"ISRC", "TSRC"},
		{"COMMENT", "COMM"},
	};

	// One frame per key, its values as ID3v2.4's null-separated list.
	std::vector<std::string> keyOrder;
	std::map<std::string, std::vector<std::string>> values;
	for (const auto& [key, value] : fields) {
		if (!values.contains(key))
			keyOrder.push_back(key);
		values[key].push_back(value);
	}

	std::vector<uint8_t> frames;
	for (const auto& key : keyOrder) {
		const auto found = FrameOf.find(key);
		const std::string id = found == FrameOf.end() ? "TXXX" : found->second;
		std::vector<uint8_t> body{3};  // UTF-8
		if (id == "COMM") {
			// Language, an empty description, then one text: several comments become lines.
			body.insert(body.end(), {'u', 'n', 'd', 0});
			for (size_t i = 0; i < values[key].size(); i++) {
				if (i)
					body.push_back('\n');
				body.insert(body.end(), values[key][i].begin(), values[key][i].end());
			}
		} else {
			if (id == "TXXX") {
				body.insert(body.end(), key.begin(), key.end());
				body.push_back(0);
			}
			for (size_t i = 0; i < values[key].size(); i++) {
				if (i)
					body.push_back(0);
				body.insert(body.end(), values[key][i].begin(), values[key][i].end());
			}
		}
		frames.insert(frames.end(), id.begin(), id.end());
		put_synchsafe(frames, body.size());
		frames.insert(frames.end(), {0, 0});
		frames.insert(frames.end(), body.begin(), body.end());
	}

	std::vector<uint8_t> tag{'I', 'D', '3', 4, 0, 0};
	put_synchsafe(tag, frames.size());
	tag.insert(tag.end(), frames.begin(), frames.end());

	std::vector<uint8_t> chunk{'i', 'd', '3', ' '};
	put_u32le(chunk, static_cast<uint32_t>(tag.size()));
	chunk.insert(chunk.end(), tag.begin(), tag.end());
	if (tag.size() & 1)
		chunk.push_back(0);
	return chunk;
}
