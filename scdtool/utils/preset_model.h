#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What a MusicImportConfig item says a target is built from: its segments, the recording each
// source name resolves to under the OST pool, and the filter attached to each.
//
// Shared rather than kept inside `apply` because `verify` has to read a preset exactly the way
// `apply` built from it. The join detector models every copy of every recording the build
// mixed, at the offset the preset put it; a second reading of the same JSON that resolved one
// name differently -- a bare pattern searched across every album instead of the default one,
// say, which once sent 114 targets to the wrong release -- would compare the game against a
// build that was never made, and every finding it produced would be about that.

// One `inputFiles` slot of such a graph. A slot is usually a file out of the OST pool,
// but the old importer also writes an empty slot to mean the game's own entry, and that
// file does not exist until the build stages it -- so the slot has to survive resolution
// as an intention rather than as a path, and be filled in at render time.
struct apply_graph_input {
	std::filesystem::path Path;
	bool IsTarget = false;  // the game's own audio for the entry being built
};

// A source built by a filter graph rather than read from one file. Rendered to a temp
// file once, before anything else looks at it, so every treatment downstream -- offsets,
// per-channel decode, fades, loudness, the loop -- works on it unchanged.
struct apply_source_graph {
	std::vector<apply_graph_input> Inputs;       // one per `inputFiles` slot the graph reads
	std::string Description;                     // the -filter_complex argument
	std::string OutLabel;                        // the label to -map
};

// One source feeding one segment: which file, where in it the segment starts, and the
// filter chain the preset attached to that source.
struct apply_segment_source {
	std::filesystem::path Path;
	double Offset = 0.;       // seconds into the source that this segment begins at
	std::wstring Filter;
	// Set instead of `Path` when the preset builds this source from a graph; `Path` is
	// filled in with the rendered file before the build reads it.
	std::shared_ptr<apply_source_graph> Graph;
	// The reserved source name `"target"`: the game's own audio for this entry, rather
	// than anything out of the OST pool. It resolves to no file here because the file
	// does not exist until the build stages it, so the assembler substitutes it.
	//
	// What it is for is material the recording simply does not contain. A tail gap can be
	// filled by re-entering the same recording, but a *head* gap cannot -- BGM_EX4_Raid_10
	// and BGM_EX5_Ban_11 carry `adelay=5605` and `adelay=6665` because, as their comments
	// say, the game's file starts before the recording does, and no amount of re-entry
	// produces an intro the release does not have. The game's own opening is the only
	// source for it.
	bool IsTarget = false;
	// Whether the preset named this offset or it is the implicit zero. The generator
	// writes no offset at all when the match was already within 50ms, so an absent one
	// carries that much slack; a stated one was fitted and only lost precision to JSON.
	bool Stated = false;

	// `"exact": true` beside the offset: it was measured to the sample against this very
	// recording, so apply's own sample alignment -- one four-second window at the loop start,
	// which a remixed game file can pull milliseconds off (BGM_EX3_System_Title, 9 ms at
	// r 0.74) -- must not move it. Without it a stated offset is treated as a millisecond-
	// rounded record and refined, which is what most of presets/ needs.
	bool Exact = false;

	// `pcmHash` beside the offset: the CRC-32 of the recording's canonical decode (s32le at its
	// own rate and channel count) the offset was measured against, as 8 hex digits. Another
	// encode of the same recording decodes to other samples -- an MP3 without a LAME header
	// keeps its encoder delay, 23 ms on BGM_EX4_Event_13's -- so a stated offset is only as
	// good as this match. Empty where the preset does not say.
	std::string PcmHash;

	// Set from the preset file's `offsetsAreFitted`: every offset in that file is
	// already decided, the ones it leaves out included. See FixesAlignment.
	bool Fitted = false;

	// Whether this source's alignment is settled, so the onset alignment must leave it
	// alone. Two ways it can be: the file declares all of its offsets fitted, which is
	// what `presets/` says, or this offset is stated and non-zero, which is a fitted
	// offset written out.
	//
	// A stated *zero* is neither. In the old importer the field set where reading
	// begins -- zero being where it begins anyway -- and the onset alignment then ran
	// on top of it regardless, so an explicit zero was never a claim about alignment.
	bool FixesAlignment() const { return Fitted || (Stated && Offset != 0.); }
};

// A span of the output, in the *target's* timeline. Segments run back to back in the
// order given: `Length` is how long this one holds the output before the next one
// starts, and 0 means "until its source runs out", which is what the last segment of a
// preset always says. `CrossfadeSeconds` lets the previous segment carry on playing
// past its stated length, faded out underneath this one fading in -- which is how the
// game built the loop-outs these presets reproduce, so a hard cut is not a substitute.
struct apply_segment {
	std::map<std::string, apply_segment_source> Sources;
	std::vector<std::pair<std::string, size_t>> Channels;  // output channel -> (source name, channel in it)
	// `sourceThresholds`, keyed by source name; `"target"` names the game's own entry.
	std::map<std::string, double> Thresholds;

	// `crossfadeShape`: how this segment's crossfade with the previous one is curved.
	// Complementary linear ramps hold level where the two sides are correlated and sag
	// 3 dB where they are not; square-root ramps do the reverse. A crossfade exists
	// precisely where two segments carry *different* material, so uncorrelated is the
	// normal case and equal power is the default; `"linear"` opts out.
	//
	// Measured over all 95 crossfaded targets, the same items built both ways: the
	// envelope hole improves on 43 and worsens on 2, both by 0.1 dB, mean -0.84 dB,
	// with the weighted score unmoved. On BGM_EX5_Raid_22's join, which puts the
	// recording's ending against the same recording re-entered 144s earlier, the worst
	// point against the game's own file goes -5.3 dB to -2.7.
	bool CrossfadeEqualPower = true;
	double Length = 0.;
	double CrossfadeSeconds = 0.;
	// Where this segment begins in the target, when it does not simply follow the one
	// before it. That turns a sequence into a layering: several spans sounding at once
	// rather than one after another, which is what a canon is -- BGM_EX4_Event_15 is its
	// own recording entering three times over itself. Negative means "follow on".
	double StartSeconds = -1.;
	// A fade at this segment's own edges, in its own span -- not the crossfade with a
	// neighbour, which `CrossfadeSeconds` already covers and which only exists where two
	// segments meet. Ten of the hand-written filterComplex graphs are one window of one
	// recording with a fade at one or both ends and nothing else:
	//
	//     [0:a]atrim=114.889:217.103,asetpts=PTS-STARTPTS,afade=t=out:st=100.714:d=1
	//
	// `sourceFilters` cannot say that. Filters run on the whole source *before* the
	// offset trims it, so `st=100.714` would have to be rewritten to 217.103 -- against
	// the source's timeline rather than the window's -- and an entry whose offset differs
	// per album would need a different number in each. Negative means "not stated", which
	// leaves whatever the crossfade machinery decides.
	double FadeInSeconds = -1.;
	double FadeOutSeconds = -1.;
};

// One MusicImportConfig target, resolved: every game path it lists, and the segments
// that build each of them. A target that states no segments gets the one default span
// `apply` gives it -- its single source, channels 0 and 1, from the start.
struct config_target {
	std::vector<std::string> Paths;
	std::vector<apply_segment> Segments;
	std::string Note;          // the preset's own "# comment"
};

// The album directory a MusicImportConfig means by a name. The config names an album
// ("Stormblood"); the directory carries the patch version too ("4.0 - Stormblood"), so
// the name is matched as a suffix.
//
// With no name, this answers with the preset's *default* album -- the one flagged
// `default` in `searchDirectories`, or the first listed. That is the scope of a bare
// pattern, and only that: MusicImporter fills a pattern's absent directory in with the
// default and then skips any pattern whose directory is not the folder being scanned, so
// a bare name never reaches another album however many the preset lists. Searching all
// of them instead let `ENDWALKER_001`'s disc index "00000" land on `GL_00000.flac` in
// Growing Light, which Endwalker.json also lists; 114 targets resolved to a recording
// from the wrong release that way.
std::vector<std::filesystem::path> resolve_search_directories(
	const std::filesystem::path& ostDir,
	const nlohmann::json& config,
	const std::string& album = {});

// A MusicImportConfig source name is a list of alternatives -- a disc index, an OST
// stem, the track's title in either language -- and the first one that names exactly one
// file wins. Two files matching is an error rather than a coin flip, which is the rule
// the importer itself follows.
//
// An alternative may also be an object naming its own directory, which is how an entry
// reaches a track that lives on another album: a credits medley stitched from six
// releases names each of them explicitly rather than widening the search for all of them.
std::optional<std::filesystem::path> resolve_source_name(
	const std::filesystem::path& ostDir,
	const nlohmann::json& config,
	const std::vector<std::filesystem::path>& dirs,
	const nlohmann::json& patterns);

// The first game path a target names, for a diagnostic that has nothing else to
// identify it by.
std::string collect_config_target_path(const nlohmann::json& target);

// Resolves one MusicImportConfig target against the OST pool. Nothing when the target is
// disabled, names nothing to build, or a source does not resolve -- the last of which is
// appended to `unresolved` as (first path, reason). Throws where resolution itself fails,
// as a pattern naming two files does; the caller reports that the same way.
std::optional<config_target> read_config_target(
	const std::filesystem::path& ostDir,
	const nlohmann::json& config,
	const nlohmann::json& sourceSpec,
	const nlohmann::json& target,
	std::vector<std::pair<std::string, std::string>>& unresolved);

// Every preset file under a comma-separated list of files and directories, in release
// order: by the `name` each states ("Final Fantasy XIV - 2.5 - Before The Fall"), one that
// names none after every one that does, by filename. Order decides which album serves a
// target listed by more than one, and the two releases of a piece are rarely the same
// recording: by filename, A Realm Reborn would claim BGM_Ban_Ifrit from Before Meteor and
// come out 0.006 further from the game's own file.
std::vector<std::filesystem::path> release_ordered_presets(const std::vector<std::filesystem::path>& roots);
