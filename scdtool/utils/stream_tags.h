#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <xivres/sound.h>

// The album's own tags, carried into the replacement: TITLE, ARTIST, ALBUM and the rest,
// read off each recording an entry is built from. They go where each payload keeps tags
// natively -- the Vorbis comment header of an Ogg stream, the VORBIS_COMMENT block of a FLAC
// one, an ID3v2 chunk trailing a WAV one -- so any player that opens the extracted stream
// shows what the music is, and the game, which reads none of it, plays exactly what it did.
namespace stream_tags {
	// One field as a Vorbis comment names it: an upper-case key and one value. A key may
	// repeat, which is how a Vorbis comment carries several values of one field.
	using field = std::pair<std::string, std::string>;

	// The recording's tags, container and stream alike, with keys upper-cased and ffprobe's
	// renamings undone (track -> TRACKNUMBER, disc -> DISCNUMBER, album_artist -> ALBUMARTIST).
	// Loop and encoder fields are left out: they describe that file, not the music. Empty if
	// ffprobe cannot read the file.
	std::vector<field> read(const std::filesystem::path& ffprobe, const std::filesystem::path& file);

	// Several recordings' tags as one set, the recordings given in the order the entry plays
	// them. Each key keeps every distinct value once, in that order; keys come in the order
	// they first appear.
	std::vector<field> merge(const std::vector<std::vector<field>>& perRecording);

	// "KEY=value" strings, as a Vorbis comment header stores them.
	std::vector<std::string> vorbis_comments(const std::vector<field>& fields);

	// The same Ogg entry with these comments added to its comment header. Only the header
	// pages are rebuilt: the audio pages keep every byte, so the loop offsets, seek table and
	// MARK chunk still point where they did, and only a change in how many pages the header
	// takes renumbers them.
	xivres::sound::writer::sound_item with_vorbis_comments(
		xivres::sound::writer::sound_item entry,
		const std::vector<std::string>& comments);

	// A RIFF "id3 " chunk holding the fields as an ID3v2.4 tag, padded to an even length.
	// Empty when there are no fields.
	std::vector<uint8_t> id3_riff_chunk(const std::vector<field>& fields);
}
