#pragma once

#include "preset_model.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Whether a build mixes its copies of a recording the way the game's own file does, across
// every join of every item that plays more than one.
//
// A "copy" is one recording read at one lag: a preset segment, or one branch of an
// asplit/adelay sum inside a single segment's filter. Where two copies meet, the game's file
// fades one out and the other in, holds a level between them, or cuts; the build does
// whatever the preset told it to. Per 0.25 s window this measures how much of each copy the
// game plays and how much the build plays, and compares the two arrangements.
//
// Why a model rather than another score: a join that is wrong is *locally* wrong, and every
// whole-file measure averages it away -- BGM_EX5_Raid_26's pre-campaign build faded its
// re-entry in 1.1 s early, and read 0.98 on the log-mel score throughout. The windowed
// log-mel `joins` check in verify does see a join window, but it cannot tell "the build
// crossfaded where the game holds both copies at -3 dB" from "the recording is a different
// master here", because both lower a spectral match. Fitting the copies themselves can: the
// first changes the weights, the second does not.
//
// Ported from the calibrated prototype (scratch/_detector/detect.py), whose constants were
// set on 17 cases -- 11 items built before and after the join campaign that fixed them, three
// negative controls deliberately rebuilt wrong (the M5 sums), and six untouched controls --
// and recorded with each constant below. Change one and every figure quoted beside it is
// stale.

constexpr size_t DetectorRateHz = 44100;

// One recording read at one lag.
struct detector_copy {
	std::string Source;            // the preset's name for it
	std::filesystem::path File;    // the recording
	std::string Render;            // the waveform part of its filter, as an ffmpeg -af chain
	double C = 0.;                 // rendered-recording time = C + game time
	std::vector<size_t> ChannelMap;
	std::string Label;             // the asplit branch it is, if any

	std::string name() const;
	// Copies that play the same stretch of the same render are one copy: an M7/M14 layered
	// lead-in continues the segment it overlaps, and fitting the two separately would split
	// one waveform's weight between two identical columns.
	std::string key() const;
};

// Everything one target plays, and where its joins are.
struct detector_plan {
	std::vector<detector_copy> Copies;
	std::vector<double> Events;          // game seconds a copy starts; sorted
	std::vector<std::string> Notes;
	std::string NotAnalysable;           // why, when it is not
	// Whether the target mixes more than one copy at all; a single-copy target has nothing
	// for this to judge and is not reported.
	bool MultiCopy = false;
};

// `nativeRate` answers a recording's own sample rate: an adelay written in samples (`S`)
// counts them at that rate, not at the analysis rate.
detector_plan plan_copies(const std::vector<apply_segment>& segments, size_t gameChannels,
	const std::function<double(const std::filesystem::path&)>& nativeRate);

// Where to look: every event, clustered.
struct detector_region_span {
	double From = 0., To = 0.;
	std::vector<double> Joins;
};

std::vector<detector_region_span> detector_regions(const std::vector<double>& events, double endSeconds);

// Interleaved float audio at DetectorRateHz.
struct detector_audio {
	const float* Data = nullptr;
	size_t Frames = 0;
	size_t Channels = 0;
};

// A copy's render, paired with the copy.
struct detector_copy_audio {
	const detector_copy* Copy = nullptr;
	detector_audio Audio;
};

// A per-copy weight curve reduced to what a reader wants: its level either side, and where
// and how it moved from one to the other.
struct detector_curve {
	enum class kind { Unknown, Steady, In, Out };
	kind Kind = kind::Unknown;
	double Start = 0., End = 0., Mid = 0.;
	double T10 = 0., T50 = 0., T90 = 0.;
	std::string Shape;
};

struct detector_copy_result {
	std::string Name;
	detector_curve Game, Build;
	std::optional<double> Dt50;           // the build's 50 % point minus the game's
	bool KindMismatch = false;            // one fades out where the other fades in or holds
	double LagGameMs = 0., LagBuildMs = 0.;
	size_t LagWindowsGame = 0, LagWindowsBuild = 0;
	double LagRhoGame = 0., LagRhoBuild = 0.;
	double IdentFraction = 0.;
};

struct detector_region_result {
	double From = 0., To = 0.;
	std::vector<double> Joins;
	std::vector<std::string> Copies;
	double PeakShape = 0., PeakShapeAt = 0.;
	double PeakLevelDb = 0., PeakLevelAt = 0.;   // signed; the 1 s mean furthest from zero
	double Area = 0.;                           // shape excess integrated over the region, s
	double WrongSeconds = 0.;
	double Total = 0.;
	double RelLagMs = 0.;
	double R2Game = 0., R2Build = 0.;
	bool ShapeFlag = false, LevelFlag = false, LagFlag = false;
	std::vector<detector_copy_result> PerCopy;
	std::string Error;

	// The join flag. The lag flag is separate: a copy a few milliseconds out is heard as a
	// smear rather than as a wrong arrangement, and the two are fixed in different places.
	bool flagged() const { return Error.empty() && (ShapeFlag || LevelFlag); }
	// What is wrong, in words; empty when nothing is.
	std::string problem() const;
};

// `channels` is the analysis layout: the game's own channel count, 1 or 2.
detector_region_result analyse_join_region(const detector_audio& game, const detector_audio& build,
	size_t channels, const std::vector<detector_copy_audio>& copies, const detector_region_span& region);

// A raw f32le decode on disk, mapped read-only, and deleted when the last reference goes.
// The renders are the size of the recordings -- a five-minute album track is 106 MB at
// 44.1 kHz stereo -- and a credits roll is half an hour, so holding a run's worth in memory
// is not an option; the page cache decides what stays resident.
class mapped_audio_file {
	void* m_file = nullptr;
	void* m_mapping = nullptr;
	const float* m_data = nullptr;
	size_t m_count = 0;
	size_t m_channels = 1;
	std::filesystem::path m_path;

public:
	mapped_audio_file(std::filesystem::path path, size_t channels);
	~mapped_audio_file();
	mapped_audio_file(const mapped_audio_file&) = delete;
	mapped_audio_file& operator=(const mapped_audio_file&) = delete;

	[[nodiscard]] detector_audio audio() const { return {m_data, m_count / m_channels, m_channels}; }
};

// Decodes `input` through `filter` (may be empty) to DetectorRateHz with soxr, as raw f32le at
// `rawPath`, and maps it. `channels` is what the decode carries -- the input's own count.
std::shared_ptr<mapped_audio_file> decode_for_detector(const std::filesystem::path& ffmpeg,
	const std::filesystem::path& input, const std::string& filter, size_t channels,
	const std::filesystem::path& rawPath);
