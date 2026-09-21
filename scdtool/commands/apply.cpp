#include "pch.h"
#include "apply.h"

#include "utils/argactions.h"
#include "utils/audio_match.h"
#include "utils/lossless_vorbis.h"
#include "utils/misc.h"
#include "utils/substitute_codec.h"
#include "utils/win32_process.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace {
	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}

	// One engine-switched stem of a multichannel entry: a stereo album track, and the two
	// raw channels of the game's entry that carry it.
	struct apply_stem {
		std::filesystem::path SourcePath;
		double Score = 0.;
		double Offset = 0.;       // seconds the source is shifted relative to the game's timeline
		size_t LeftChannel = 0;
		size_t RightChannel = 1;
		bool Matched = false;     // false: these channels keep the game's own audio
		// Filled in while building, for the log line.
		double EffectiveOffset = 0.;
		double GainDb = 0.;
		double OnsetDb = 0.;
		double OnsetSeconds = 0.;
		double ShortfallSeconds = 0.;
		bool Deduced = false;
	};

	// One source feeding one segment: which file, where in it the segment starts, and the
	// filter chain the preset attached to that source.
	struct apply_segment_source {
		std::filesystem::path Path;
		double Offset = 0.;       // seconds into the source that this segment begins at
		std::wstring Filter;
		// Whether the preset named this offset or it is the implicit zero. The generator
		// writes no offset at all when the match was already within 50ms, so an absent one
		// carries that much slack; a stated one was fitted and only lost precision to JSON.
		bool Stated = false;
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

	struct apply_job {
		std::string TargetPath;   // path inside the game
		std::filesystem::path SourcePath;
		double Score = 0.;
		double Offset = 0.;       // seconds the source is shifted relative to the game's timeline
		std::vector<apply_stem> Stems;  // empty unless the entry is engine-switched stems
		std::vector<apply_segment> Segments;  // empty unless the entry needs more than one span
		std::wstring Filter;      // the preset's filter chain for this source, if it gave one
		// A preset is a record of decisions already taken: its offset was fitted against
		// this very file, so it is not re-derived. A gain or a lead-in silence, though, is
		// only written down when it was worth writing -- the generator dropped any gain
		// under a decibel -- so the absence of one is not an instruction to leave the level
		// alone. Measuring it where the preset is silent is what the matchset build did, and
		// without it BGM_Town_Uru_Day came out 0.6 dB off a game file it used to match
		// exactly. Only where the preset *does* say would measuring again apply it twice.
		bool FromPreset = false;
		std::string Note;         // the preset's own comment, echoed in the log line
	};

	// Loop points of the template entry, in samples.
	//
	// These are NOT the header's LoopStartOffset/LoopEndOffset, which are byte offsets
	// into the entry's data; the encoder wants the sample indices that the SCD stores
	// as the Ogg's LoopStart/LoopEnd comments, which is what decoding exposes. A track
	// that does not loop has both at 0, and 0 must stay 0 so the replacement does not
	// acquire a loop the original never had.
	std::pair<size_t, size_t> template_loop_points(const xivres::sound::reader::sound_item& item) {
		if (item.Header->Format != xivres::sound::sound_entry_format::Ogg)
			return {0, 0};
		const auto info = item.get_ogg_decoded();
		return {info.LoopStartBlockIndex, info.LoopEndBlockIndex};
	}

	int probe_sample_rate(const std::filesystem::path& ffprobe, const std::filesystem::path& file) {
		const auto bytes = run_process_capture_stdout(ffprobe, {
			L"-v", L"error",
			L"-select_streams", L"a:0",
			L"-show_entries", L"stream=sample_rate",
			L"-of", L"default=noprint_wrappers=1:nokey=1",
			file.wstring(),
		});
		std::string text(bytes.begin(), bytes.end());
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
			text.pop_back();
		if (text.empty())
			throw std::runtime_error(std::format("ffprobe reported no sample rate for {}", u8(file)));
		return std::stoi(text);
	}

	// Integrated loudness (LUFS) of a file, optionally restricted to a time range.
	//
	// EBU R128 rather than RMS: the game ships these tracks as background music that has
	// to sit at a consistent level against everything else, and integrated loudness is
	// the measure that actually tracks perceived level, gating out silence instead of
	// being dragged down by it.
	// `preFilter`, when given, runs before ebur128 -- used to measure one stereo stem of a
	// multichannel entry ("pan=stereo|c0=c0|c1=c2") rather than the whole interleaved mix,
	// whose loudness is the sum of every engine state at once and matches no album track.
	double measure_loudness(const std::filesystem::path& ffmpeg, const std::filesystem::path& file, double startSeconds, double durationSeconds, const std::wstring& preFilter = {}) {
		// -ss after -i: sample-accurate, which matters because the two sides of the
		// comparison must cover exactly the same musical span for the difference to mean
		// anything. The files are only minutes long, so the extra decode is cheap.
		std::vector<std::wstring> args{L"-v", L"info", L"-nostdin", L"-i", file.wstring()};
		if (startSeconds > 0)
			args.insert(args.end(), {L"-ss", std::to_wstring(startSeconds)});
		if (durationSeconds > 0)
			args.insert(args.end(), {L"-t", std::to_wstring(durationSeconds)});
		args.insert(args.end(), {L"-af", preFilter.empty() ? std::wstring(L"ebur128") : preFilter + L",ebur128", L"-f", L"null", L"-"});

		const auto bytes = run_process_capture_stderr(ffmpeg, args);
		const std::string text(bytes.begin(), bytes.end());

		// ebur128 emits one progress line per 100 ms, each also containing "I:", so the
		// first match is the running value at the very start -- which reads as silence
		// (-70 LUFS) and would silently produce a zero gain for every file. The summary at
		// the end is the only one that carries neither "TARGET:" nor "Threshold".
		std::istringstream lines(text);
		double value = std::numeric_limits<double>::quiet_NaN();
		for (std::string line; std::getline(lines, line);) {
			const auto label = line.find("I:");
			if (label == std::string::npos || line.find("LUFS") == std::string::npos)
				continue;
			if (line.find("TARGET:") != std::string::npos || line.find("Threshold") != std::string::npos)
				continue;
			const auto valueStart = line.find_first_of("-0123456789", label + 2);
			if (valueStart == std::string::npos)
				continue;
			// from_chars rather than stod: a line whose remainder is not a number is skipped
			// by checking the error code, so there is no exception to swallow here.
			const auto valueText = std::string_view(line).substr(valueStart);
			double parsed = 0.;
			if (std::from_chars(valueText.data(), valueText.data() + valueText.size(), parsed).ec == std::errc{})
				value = parsed;
		}
		if (std::isnan(value) || value <= -70.)
			throw std::runtime_error(std::format("ffmpeg reported no usable integrated loudness for {}", u8(file)));
		return value;
	}

	// Decodes the source to raw float32 PCM at the sample rate and channel count the
	// template uses.
	//
	// Deliberately not WAV: ffmpeg tags its PCM output WAVE_FORMAT_EXTENSIBLE (0xFFFE),
	// which xivres's WAV reader rejects, and going through WAV would also mean matching
	// up header layouts for no benefit. Matching the template's rate matters for more
	// than fidelity -- the loop points are sample indices, so a resample would silently
	// move the loop.
	std::vector<float> decode_source_to_floats(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& source,
		size_t channels,
		size_t samplingRate,
		const std::filesystem::path& rawPath,
		const std::wstring& filter = {}) {

		std::error_code ec;
		std::filesystem::remove(rawPath, ec);

		// soxr, matching MusicImporter: the SCD is lossy Vorbis, so the resample is part
		// of the audible chain and a cheap one would be the weak link.
		std::vector<std::wstring> args{
			L"-v", L"error",
			L"-i", source.wstring(),
			L"-map", L"0:a:0",
		};
		// The preset's own chain runs first, on the whole recording, which is what
		// MusicImportConfig's sourceFilters have always meant -- the offset trims after it.
		if (!filter.empty())
			args.insert(args.end(), {L"-af", filter});
		args.insert(args.end(), {
			L"-ac", std::to_wstring(channels),
			L"-ar", std::to_wstring(samplingRate),
			L"-resampler", L"soxr",
			L"-f", L"f32le",
			L"-y", rawPath.wstring(),
		});
		run_process_capture_stdout(ffmpeg, args);

		std::ifstream f(rawPath, std::ios::binary | std::ios::ate);
		if (!f)
			throw std::runtime_error(std::format("ffmpeg produced no output for {}", u8(source)));
		const auto size = static_cast<size_t>(f.tellg());
		f.seekg(0);

		std::vector<float> floats(size / sizeof(float));
		if (!floats.empty() && !f.read(reinterpret_cast<char*>(floats.data()), static_cast<std::streamsize>(floats.size() * sizeof(float))))
			throw std::runtime_error(std::format("Could not read decoded audio for {}", u8(source)));
		return floats;
	}

	// One raw channel of a file, as mono floats at the given rate.
	//
	// `pan=mono|c0=cN` indexes the decoded channel directly -- no channel-layout remapping
	// happens in this path, which matters because the game's 6-channel entries are stems
	// laid out in the asset's own order, not a 5.1 mix, and `-ac`/`-map_channel` would be
	// free to reinterpret them against a speaker layout they do not follow.
	// `filter` is a preset's own filter chain for this source, run *before* the channel is
	// picked out -- which is the order MusicImportConfig's sourceFilters have always meant:
	// they treat the whole recording, and the offset and channel selection come after.
	std::vector<float> decode_channel_to_floats(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& source,
		size_t channelIndex,
		size_t samplingRate,
		const std::filesystem::path& rawPath,
		const std::wstring& filter = {}) {

		std::error_code ec;
		std::filesystem::remove(rawPath, ec);

		run_process_capture_stdout(ffmpeg, {
			L"-v", L"error",
			L"-i", source.wstring(),
			L"-map", L"0:a:0",
			L"-af", filter.empty()
				? std::format(L"pan=mono|c0=c{}", channelIndex)
				: std::format(L"{},pan=mono|c0=c{}", filter, channelIndex),
			L"-ar", std::to_wstring(samplingRate),
			L"-resampler", L"soxr",
			L"-f", L"f32le",
			L"-y", rawPath.wstring(),
		});

		std::ifstream f(rawPath, std::ios::binary | std::ios::ate);
		if (!f)
			throw std::runtime_error(std::format("ffmpeg produced no output for channel {} of {}", channelIndex, u8(source)));
		const auto size = static_cast<size_t>(f.tellg());
		f.seekg(0);

		std::vector<float> floats(size / sizeof(float));
		if (!floats.empty() && !f.read(reinterpret_cast<char*>(floats.data()), static_cast<std::streamsize>(floats.size() * sizeof(float))))
			throw std::runtime_error(std::format("Could not read decoded channel {} of {}", channelIndex, u8(source)));
		return floats;
	}

	// Like decode_source_to_floats, but only the first few seconds -- used to sample a
	// track's own onset without paying to decode the whole thing.
	std::vector<float> decode_onset_to_floats(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& source,
		size_t channels,
		size_t samplingRate,
		double seconds,
		const std::filesystem::path& rawPath) {

		std::error_code ec;
		std::filesystem::remove(rawPath, ec);

		run_process_capture_stdout(ffmpeg, {
			L"-v", L"error",
			L"-i", source.wstring(),
			L"-t", xivres::util::unicode::convert<std::wstring>(std::format("{}", seconds)),
			L"-map", L"0:a:0",
			L"-ac", std::to_wstring(channels),
			L"-ar", std::to_wstring(samplingRate),
			L"-resampler", L"soxr",
			L"-f", L"f32le",
			L"-y", rawPath.wstring(),
		});

		std::ifstream f(rawPath, std::ios::binary | std::ios::ate);
		std::vector<float> floats;
		if (f) {
			const auto size = static_cast<size_t>(f.tellg());
			f.seekg(0);
			floats.resize(size / sizeof(float));
			if (!floats.empty() && !f.read(reinterpret_cast<char*>(floats.data()), static_cast<std::streamsize>(floats.size() * sizeof(float))))
				floats.clear();
		}
		return floats;
	}

	struct deduced_offset {
		double Seconds = 0.;
		double Score = -2.;
		size_t Candidates = 0;
		bool Deduced = false;
	};

	// Works out where in the source this target's content actually begins, rather than
	// trusting the offset the match recorded.
	//
	// Two reasons it is worth re-deriving. The recorded offset is measured against whatever
	// copy of the album the matcher saw, so a user's trimmed or differently-encoded file
	// invalidates it -- a re-encode alone shifts by a constant (FLAC vs the MP3 release
	// measured 27ms on every track tested), and hand-trimming shifts by arbitrary seconds.
	// And the matcher chooses on an RMS envelope, which is near-periodic over a loop, so on
	// a release structured intro + loop + loop it frequently locks onto a later pass: 65 of
	// 583 entries audited in presets-new were wrong this way.
	//
	// The envelope still proposes, because it is cheap and its peaks do include the right
	// answer -- every loop pass is a peak. The spectrum disposes, judged on the game file's
	// pre-loop intro. That region is the one that plays exactly once, so it is both the part
	// a late alignment destroys and the only part where the candidates genuinely differ;
	// judging on a whole-file mean dilutes the decision away almost entirely (measured:
	// 0.9959 vs 0.9944 across 240s, 0.9957 vs 0.8457 across the 2s that matter).
	deduced_offset deduce_offset(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& templateAudio,
		const std::filesystem::path& source,
		double introSeconds,
		double recordedOffset) {

		constexpr double EnvelopeRateHz = 200.;   // decode_envelope's default 80-sample hop at 16 kHz
		constexpr double MaxOffsetSeconds = 900.;
		constexpr double MinOverlapSeconds = 15.; // absolute, never a fraction of the target: a
		                                          // silence-padded 600s file holding 30s of audio
		                                          // can never meet a fractional floor and would
		                                          // otherwise score nothing at all
		constexpr double MinIntroSeconds = 1.5;   // shorter than this is a lead-in, not an opening
		constexpr double FallbackJudgeSeconds = 120.;
		constexpr double TieMargin = 0.03;
		constexpr double AcceptScore = 0.5;

		deduced_offset result{recordedOffset, -2., 0, false};

		const auto envTemplate = decode_envelope(ffmpeg, templateAudio);
		const auto envSource = decode_envelope(ffmpeg, source);
		auto candidates = envelope_offset_candidates(
			envTemplate, envSource, EnvelopeRateHz, MaxOffsetSeconds, MinOverlapSeconds);
		if (candidates.empty())
			return result;

		// The recorded offset is a candidate like any other, even when it is not a local
		// maximum of this particular envelope -- it was derived from a real measurement and
		// should have to lose on merit rather than by not being on the ballot.
		if (std::ranges::none_of(candidates, [&](const offset_candidate& c) {
			return std::abs(c.OffsetSeconds - recordedOffset) < 0.5;
		}))
			candidates.push_back({recordedOffset, -2., 0.});

		const auto specTemplate = decode_logmel(ffmpeg, templateAudio, 240.);
		const auto specSource = decode_logmel(ffmpeg, source, MaxOffsetSeconds + 240.);
		if (specTemplate.empty() || specSource.empty())
			return result;

		const auto judgeTo = introSeconds >= MinIntroSeconds ? introSeconds : FallbackJudgeSeconds;

		double bestScore = -2.;
		for (const auto& c : candidates)
			bestScore = (std::max)(bestScore, spectral_similarity_at(specTemplate, specSource, c.OffsetSeconds, 0., judgeTo));
		if (bestScore < AcceptScore)
			return result;

		// Among alignments the intro cannot separate -- a piece whose opening really is its
		// loop body -- take the one that reads the source earliest, which is the largest
		// offset in this sign convention (target frame t holds source content at t - offset,
		// so reading further into the source means a more negative offset). Later alignments
		// risk running out of source before the target ends.
		double chosen = 0.;
		double chosenScore = -2.;
		bool first = true;
		for (const auto& c : candidates) {
			const auto score = spectral_similarity_at(specTemplate, specSource, c.OffsetSeconds, 0., judgeTo);
			if (score < bestScore - TieMargin)
				continue;
			if (first || c.OffsetSeconds > chosen) {
				chosen = c.OffsetSeconds;
				chosenScore = score;
				first = false;
			}
		}
		if (first)
			return result;

		result.Seconds = chosen;
		result.Score = chosenScore;
		result.Candidates = candidates.size();
		result.Deduced = true;
		return result;
	}

	// Reproduces the target's own onset treatment (a fade-in, a held silence, whatever it
	// actually is) on the replacement, rather than guessing a generic fade -- a plain
	// trim+gain has no way to know such a thing exists, since it is something the game's
	// own sound team added on top of the OST master, not something present in the OST
	// recording at the matched splice point. Measured on real data (2026-09-19): about
	// 10% of a whole album's freshly matched targets carry an onset the game holds back or
	// fades in that the source does not, ranging from a ~150ms fade to over a second of
	// near-silence before a chime.
	//
	// Compares the rebased source's own amplitude against the target's, block by block,
	// from the very start; wherever the target reads meaningfully quieter, attenuates the
	// source to match -- never boosts, since a spurious "target is louder" reading on a
	// near-silent block is decode noise, not signal, and amplifying it would add hiss.
	// Stops as soon as a run of blocks needs no correction, so the ordinary case (no
	// authored onset) is left untouched rather than getting a fixed-length window applied
	// regardless.
	void apply_onset_correction(
		std::vector<float>& floats,
		const std::vector<float>& templateFloats,
		size_t channels,
		size_t samplingRate,
		double& appliedDb,
		double& appliedSeconds) {
		appliedDb = 0.;
		appliedSeconds = 0.;
		if (!channels)
			return;
		constexpr double BlockSeconds = 0.02;  // 20ms: short enough to localize a fast fade, long enough to average out sample noise
		const auto blockSamples = (std::max<size_t>)(1, static_cast<size_t>(BlockSeconds * static_cast<double>(samplingRate)));
		const auto blockFrames = blockSamples * channels;
		const auto totalBlocks = (std::min)(floats.size(), templateFloats.size()) / blockFrames;
		if (!totalBlocks)
			return;

		const auto blockRms = [&](const std::vector<float>& v, size_t block) {
			double sum = 0;
			const auto base = block * blockFrames;
			for (size_t i = 0; i < blockFrames; ++i)
				sum += static_cast<double>(v[base + i]) * static_cast<double>(v[base + i]);
			return std::sqrt(sum / static_cast<double>(blockFrames));
		};

		constexpr double CorrectionThreshold = 0.5;  // ~6 dB quieter or more counts as "an onset treatment is here"
		constexpr double MinSourceRms = 1e-5;         // floor to avoid a huge ratio off a near-zero denominator
		std::vector ratios(totalBlocks, 1.);
		size_t lastCorrected = 0;
		bool anyCorrection = false;
		for (size_t b = 0; b < totalBlocks; ++b) {
			const auto srcRms = blockRms(floats, b);
			const auto tplRms = blockRms(templateFloats, b);
			if (srcRms > MinSourceRms) {
				const auto raw = tplRms / srcRms;
				if (raw < CorrectionThreshold) {
					ratios[b] = std::clamp(raw, 0., 1.);
					lastCorrected = b;
					anyCorrection = true;
				}
			}
		}
		if (!anyCorrection)
			return;

		// Only the leading run through the last block that actually needed correction --
		// not the whole decoded onset window -- so the correction is as short as the real
		// treatment, not an arbitrary fixed length.
		const auto windowBlocks = lastCorrected + 1;

		// Linearly interpolate between block-center ratios so the correction ramps rather
		// than steps, which would otherwise click at every 20ms block boundary.
		double minRatio = 1.;
		for (size_t b = 0; b < windowBlocks; ++b) {
			const auto blockStart = b * blockFrames;
			const auto blockEnd = (std::min)(blockStart + blockFrames, floats.size());
			const auto ratioHere = ratios[b];
			const auto ratioNext = b + 1 < windowBlocks ? ratios[b + 1] : ratios[b];
			minRatio = (std::min)(minRatio, ratioHere);
			for (size_t i = blockStart; i < blockEnd; i += channels) {
				const auto t = static_cast<double>(i - blockStart) / static_cast<double>(blockEnd - blockStart);
				const auto g = ratioHere + (ratioNext - ratioHere) * t;
				for (size_t ch = 0; ch < channels && i + ch < floats.size(); ++ch)
					floats[i + ch] = static_cast<float>(floats[i + ch] * g);
			}
		}
		appliedDb = 20. * std::log10((std::max)(minRatio, 1e-6));
		appliedSeconds = static_cast<double>(windowBlocks * blockSamples) / static_cast<double>(samplingRate);
	}
}

	// Aligns the source to the game's file to the sample, after the offset search has got
	// within a few milliseconds.
	//
	// The loop points are indices into the game's own timeline and the game loops by
	// hard-cutting from the loop end back to the loop start, so an offset that is 10ms out
	// puts a different sample under each marker and the join clicks -- even when the markers
	// themselves are exactly right. Measured on BGM_EX5_BanFort_Mam_Good, whose markers were
	// correct: the seam jumped 0.548 against the game's own 0.060.
	//
	// Raw waveform correlation is the right instrument here and nowhere else in this file.
	// It is destroyed by misalignment beyond a few milliseconds -- which is exactly what
	// makes it able to resolve what an envelope at 5ms and a log-mel at 10ms cannot, once
	// they have got that close. Correlated around the loop start, because that is the
	// alignment that has to be right; a track with no loop is aligned a quarter of the way
	// in, away from any fade at either end.
	struct sample_alignment {
		double Seconds = 0.;
		double Correlation = -2.;
		bool Refined = false;
	};

	std::vector<float> decode_window_to_mono(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& source,
		double startSeconds,
		double durationSeconds,
		size_t samplingRate,
		const std::filesystem::path& rawPath) {

		std::error_code ec;
		std::filesystem::remove(rawPath, ec);

		// Output-side seek, deliberately. An input-side one lands on a container boundary --
		// on Ogg Vorbis it can be tens of samples out, which is the whole quantity being
		// measured here, and the error is invisible because both windows then agree with each
		// other about a position that is wrong. Decoding from the start and discarding costs
		// a second or two and is exact.
		std::vector<std::wstring> args{
			L"-v", L"error", L"-nostdin",
			L"-i", source.wstring(),
		};
		if (startSeconds > 0)
			args.insert(args.end(), {L"-ss", xivres::util::unicode::convert<std::wstring>(std::format("{:.6f}", startSeconds))});
		args.insert(args.end(), {
			L"-t", xivres::util::unicode::convert<std::wstring>(std::format("{:.6f}", durationSeconds)),
			L"-map", L"0:a:0",
			L"-ac", L"1",
			L"-ar", std::to_wstring(samplingRate),
			L"-resampler", L"soxr",
			L"-f", L"f32le",
			L"-y", rawPath.wstring(),
		});
		run_process_capture_stdout(ffmpeg, args);

		std::ifstream f(rawPath, std::ios::binary | std::ios::ate);
		std::vector<float> floats;
		if (f) {
			const auto size = static_cast<size_t>(f.tellg());
			f.seekg(0);
			floats.resize(size / sizeof(float));
			if (!floats.empty() && !f.read(reinterpret_cast<char*>(floats.data()), static_cast<std::streamsize>(floats.size() * sizeof(float))))
				floats.clear();
		}
		return floats;
	}

	sample_alignment refine_offset_to_samples(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& templateAudio,
		const std::filesystem::path& source,
		size_t samplingRate,
		double aroundSeconds,
		double coarseOffset,
		const std::function<std::filesystem::path(const wchar_t*, const wchar_t*)>& tempFile) {

		constexpr double HalfWindowSeconds = 2.0;
		constexpr double MaxShiftSeconds = 0.060;   // far past what the coarse search can be out by
		constexpr double MinCorrelation = 0.50;     // below this the window is not comparable at all

		sample_alignment out{.Seconds = coarseOffset};
		const auto from = aroundSeconds - HalfWindowSeconds;
		// The source plays at (game time - offset), so its window sits that much earlier.
		const auto sourceFrom = from - coarseOffset - MaxShiftSeconds;
		if (from < 0 || sourceFrom < 0)
			return out;

		const auto want = static_cast<size_t>(2 * HalfWindowSeconds * static_cast<double>(samplingRate));
		const auto shift = static_cast<size_t>(MaxShiftSeconds * static_cast<double>(samplingRate));
		const auto target = decode_window_to_mono(ffmpeg, templateAudio, from, 2 * HalfWindowSeconds,
			samplingRate, tempFile(L"scdtool_apply_align_t", L".f32"));
		const auto candidate = decode_window_to_mono(ffmpeg, source, sourceFrom,
			2 * HalfWindowSeconds + 2 * MaxShiftSeconds, samplingRate,
			tempFile(L"scdtool_apply_align_s", L".f32"));
		if (target.size() < want || candidate.size() < want + 2 * shift)
			return out;

		double targetNorm = 0.;
		for (size_t i = 0; i < want; ++i)
			targetNorm += static_cast<double>(target[i]) * target[i];
		targetNorm = std::sqrt(targetNorm);
		if (targetNorm <= 0)
			return out;

		double best = -2.;
		size_t bestLag = shift;
		for (size_t lag = 0; lag <= 2 * shift; ++lag) {
			double dot = 0., norm = 0.;
			for (size_t i = 0; i < want; ++i) {
				const auto v = static_cast<double>(candidate[lag + i]);
				dot += static_cast<double>(target[i]) * v;
				norm += v * v;
			}
			norm = std::sqrt(norm);
			if (norm <= 0)
				continue;
			if (const auto r = dot / (targetNorm * norm); r > best) {
				best = r;
				bestLag = lag;
			}
		}
		out.Correlation = best;
		if (best < MinCorrelation)
			return out;
		// lag == shift means the coarse offset was already right; anything else moves it.
		out.Seconds = coarseOffset - (static_cast<double>(bestLag) - static_cast<double>(shift)) / static_cast<double>(samplingRate);
		out.Refined = bestLag != shift;
		return out;
	}

	// Builds the interleaved audio for an entry whose channels are engine-switched stems.
	//
	// A 4- or 6-channel music entry is not a surround mix: it is two or three stereo stems
	// the engine crossfades between -- out of combat, in combat, and a transition cymbal --
	// and each stem is its own album track, with its own match offset and its own level. So
	// it cannot be built the way a stereo entry is, from one decode of one source, and it
	// also cannot be built from the resolved stems alone: a stem that did not resolve has
	// to keep the game's own audio, or that engine state would play back silent.
	//
	// The pairs are not sequential either. `discover_channel_pairing` in match.cpp finds
	// them by correlation, and 14 of the game's 21 multichannel entries pair up as
	// (0,2)(1,4)(3,5) or similar rather than (0,1)(2,3)(4,5), so the channel indices are
	// taken from the match and never assumed.
	// Lay a preset's segments onto the target's timeline and sum them into one buffer.
	//
	// Every segment is placed at the cumulative sum of the lengths before it, and the
	// overlaps are complementary linear ramps, so a crossfade region adds to unit gain
	// rather than dipping through it. That "keep playing and fade under" shape is the point
	// of the whole path: the 20 loop-out entries here are targets whose game file outlasts
	// its recording and ends by re-entering the same piece earlier on, and a hard cut at
	// the seam measures 0.939 against the game's own file where the crossfade measures
	// 0.993. The 7 credits rolls are the same machinery with a different source per segment.
	//
	// Level matching is per source per segment rather than once for the whole file: a
	// medley stitched from eight album tracks has eight different masters in it, and one
	// gain for the lot leaves most of them wrong.
	std::vector<float> build_segment_audio(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& templateAudio,
		const std::vector<apply_segment>& segments,
		size_t channels,
		size_t samplingRate,
		bool loudnessMatch,
		double maxGainDb,
		const std::function<std::filesystem::path(const wchar_t*, const wchar_t*)>& tempFile) {

		const auto rate = static_cast<double>(samplingRate);
		const auto toSamples = [rate](double seconds) {
			return static_cast<size_t>((std::max)(0LL, std::llround(seconds * rate)));
		};

		// Where each segment starts. One that names its own start is placed there and the
		// rest still follow on from it, so a layered entry and a sequenced one can be
		// described in the same list.
		std::vector<size_t> segmentStart(segments.size(), 0);
		for (size_t i = 0, cursor = 0; i < segments.size(); i++) {
			segmentStart[i] = segments[i].StartSeconds >= 0.
				? toSamples(segments[i].StartSeconds)
				: cursor;
			cursor = segmentStart[i] + toSamples(segments[i].Length);
		}

		std::vector<float> out;
		for (size_t i = 0; i < segments.size(); i++) {
			const auto& segment = segments[i];
			// Which mapped channels feed each output channel, and at what weight. Normally
			// one each; a mono entry -- which most Orchestrion rolls are -- is still
			// described by a stereo preset, because the recording it names is stereo, so it
			// folds instead of refusing. That is the same result the single-source path
			// gets from ffmpeg's `-ac 1`.
			std::vector<std::vector<std::pair<std::pair<std::string, size_t>, float>>> routing(channels);
			if (segment.Channels.size() == channels) {
				for (size_t ch = 0; ch < channels; ch++)
					routing[ch].emplace_back(segment.Channels[ch], 1.f);
			} else if (channels == 1) {
				for (const auto& mapped : segment.Channels)
					routing[0].emplace_back(mapped, 1.f / static_cast<float>(segment.Channels.size()));
			} else {
				throw std::runtime_error(std::format(
					"Segment {} maps {} channel(s) but the target entry has {}.",
					i, segment.Channels.size(), channels));
			}

			// One mono decode per (source, channel) the segment actually asks for, shared
			// between output channels that name the same pair.
			std::map<std::pair<std::string, size_t>, std::vector<float>> decoded;
			for (const auto& [name, channelIndex] : segment.Channels) {
				const auto key = std::make_pair(name, channelIndex);
				if (decoded.contains(key))
					continue;
				const auto source = segment.Sources.find(name);
				if (source == segment.Sources.end())
					throw std::runtime_error(std::format(
						"Segment {} maps a channel from source \"{}\", which it does not define.", i, name));
				decoded.emplace(key, decode_channel_to_floats(ffmpeg, source->second.Path, channelIndex,
					samplingRate, tempFile(L"scdtool_apply_seg", L".f32"), source->second.Filter));
			}

			// How much of this segment its sources can actually supply, from its own offset
			// on. A recording that stops short simply ends the segment early rather than
			// reading past its end.
			size_t available = (std::numeric_limits<size_t>::max)();
			for (const auto& [key, samples] : decoded) {
				const auto offsetSamples = toSamples(segment.Sources.at(key.first).Offset);
				available = (std::min)(available, samples.size() > offsetSamples ? samples.size() - offsetSamples : 0);
			}
			if (available == (std::numeric_limits<size_t>::max)())
				available = 0;

			// Its stated span, plus whatever tail the next segment needs to fade in over.
			const auto stated = segment.Length > 0. ? toSamples(segment.Length) : available;
			const auto tail = i + 1 < segments.size() ? toSamples(segments[i + 1].CrossfadeSeconds) : size_t{0};
			const auto render = (std::min)(stated + tail, available);
			if (!render)
				continue;

			// A stated fade wins over the one the crossfade machinery would have inferred:
			// the crossfade describes a join, and a segment that states its own fade is
			// describing its own edge, which is the stronger claim. Where neither is stated
			// this is unchanged -- fade in over the crossfade with the previous segment, fade
			// out over the one the next segment needs.
			const auto fadeIn = (std::min)(toSamples(segment.FadeInSeconds >= 0.
				? segment.FadeInSeconds : segment.CrossfadeSeconds), render);
			const auto fadeOut = (std::min)(segment.FadeOutSeconds >= 0.
				? toSamples(segment.FadeOutSeconds) : tail, render - fadeIn);

			// Per-source gain, measured over this segment's own span on both sides.
			std::map<std::string, double> gain;
			if (loudnessMatch) {
				const auto spanSeconds = static_cast<double>(stated) / rate;
				const auto startSeconds = static_cast<double>(segmentStart[i]) / rate;
				for (const auto& [name, source] : segment.Sources) {
					try {
						const auto templateLufs = measure_loudness(ffmpeg, templateAudio, startSeconds, spanSeconds);
						const auto sourceLufs = measure_loudness(ffmpeg, source.Path, source.Offset, spanSeconds);
						gain[name] = std::pow(10., std::clamp(templateLufs - sourceLufs, -maxGainDb, maxGainDb) / 20.);
					} catch (const std::exception&) {
						// Same rule as the single-source path: an unreadable measurement
						// costs the level match, never the file.
						gain[name] = 1.;
					}
				}
			}

			const auto end = segmentStart[i] + render;
			if (out.size() < end * channels)
				out.resize(end * channels, 0.f);

			for (size_t ch = 0; ch < channels; ch++) {
				for (const auto& [key, weight] : routing[ch]) {
				const auto& [name, channelIndex] = key;
				const auto& samples = decoded.at(key);
				const auto offsetSamples = toSamples(segment.Sources.at(name).Offset);
				const auto scale = weight * static_cast<float>(gain.empty() ? 1. : gain.at(name));
				for (size_t n = 0; n < render; n++) {
					auto value = samples[offsetSamples + n] * scale;
					if (n < fadeIn)
						value *= static_cast<float>(static_cast<double>(n + 1) / static_cast<double>(fadeIn + 1));
					else if (fadeOut && n >= render - fadeOut)
						value *= static_cast<float>(static_cast<double>(render - n) / static_cast<double>(fadeOut + 1));
					out[(segmentStart[i] + n) * channels + ch] += value;
				}
				}
			}
		}

		// Summed ramps cannot clip on their own, but two segments of the same loud master
		// overlapping can, and the encoder would fold the peaks over rather than refuse.
		if (const auto peak = out.empty() ? 0.f : std::abs(*std::ranges::max_element(out,
			[](float a, float b) { return std::abs(a) < std::abs(b); })); peak > 1.f) {
			for (auto& v : out)
				v /= peak;
		}
		return out;
	}

	std::vector<float> build_stem_audio(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& templateAudio,
		std::vector<apply_stem>& stems,
		size_t channels,
		size_t samplingRate,
		size_t templateRate,
		size_t templateLoopStart,
		size_t loopStart,
		size_t loopEnd,
		bool autoOffset,
		bool loudnessMatch,
		double maxGainDb,
		bool onsetMatch,
		const std::function<std::filesystem::path(const wchar_t*, const wchar_t*)>& tempFile) {

		// Start from the game's own audio, channel by channel. Decoded one channel at a
		// time rather than as one interleaved decode because `pan=mono|c0=cN` is the only
		// form known to index the asset's raw channels; anything routed through a speaker
		// layout is free to reorder stems that follow no layout at all.
		std::vector<std::vector<float>> templateChannels(channels);
		size_t templateSamples = 0;
		for (size_t ch = 0; ch < channels; ++ch) {
			templateChannels[ch] = decode_channel_to_floats(ffmpeg, templateAudio, ch, samplingRate, tempFile(L"scdtool_apply_tplch", L".f32"));
			templateSamples = (std::max)(templateSamples, templateChannels[ch].size());
		}
		if (!templateSamples)
			throw std::runtime_error("template entry decoded to no audio");

		// The output runs exactly as long as the file it replaces. A stereo build has no
		// reason to insist on that -- it takes the source's own length -- but here the
		// channels come from several recordings of different lengths, and the game's own
		// timeline is the only thing that says where all of them end together.
		std::vector<float> out(templateSamples * channels, 0.f);
		for (size_t ch = 0; ch < channels; ++ch)
			for (size_t i = 0; i < templateChannels[ch].size(); ++i)
				out[i * channels + ch] = templateChannels[ch][i];

		constexpr double OnsetWindowSeconds = 3.0;
		const auto onsetWindow = (std::min)(templateSamples, static_cast<size_t>(OnsetWindowSeconds * static_cast<double>(samplingRate)));

		for (auto& stem : stems) {
			if (!stem.Matched)
				continue;
			stem.EffectiveOffset = stem.Offset;

			// This stem of the game's file on its own, for the offset deduction and nothing
			// else. Deducing against the whole entry would compare the source against every
			// engine state summed together, which is precisely the mono downmix that made
			// these targets look unmatchable before stems were scored individually.
			const auto stemAudio = tempFile(L"scdtool_apply_stem", L".wav");
			run_process_capture_stdout(ffmpeg, {
				L"-v", L"error",
				L"-i", templateAudio.wstring(),
				L"-af", std::format(L"pan=stereo|c0=c{}|c1=c{}", stem.LeftChannel, stem.RightChannel),
				L"-c:a", L"pcm_s16le",
				L"-y", stemAudio.wstring(),
			});
			// Which of the source's two channels goes into which of the game's two is not
			// in the match: `discover_channel_pairing` reports the pair sorted, correlation
			// cannot tell the two apart, and each stem is scored as a mono downmix. Decided
			// by measurement instead (scratch/stem_channel_order.py, signed side-signal
			// correlation over every stem in the game): 35 of 42 say the lower raw channel
			// index is the left channel, 0 say the reverse, and the remaining 7 are
			// near-mono stems where the assignment makes no audible difference.
			auto left = decode_channel_to_floats(ffmpeg, stem.SourcePath, 0, samplingRate, tempFile(L"scdtool_apply_stem_l", L".f32"));
			std::vector<float> right;
			try {
				right = decode_channel_to_floats(ffmpeg, stem.SourcePath, 1, samplingRate, tempFile(L"scdtool_apply_stem_r", L".f32"));
			} catch (const std::exception&) {
				right = left;  // a mono release feeds both sides of the stem
			}

			if (autoOffset) {
				try {
					if (const auto deduced = deduce_offset(ffmpeg, stemAudio, stem.SourcePath,
							static_cast<double>(templateLoopStart) / static_cast<double>(templateRate), stem.Offset);
						deduced.Deduced) {
						// Take the deduction only if it does not cover less of the target
						// than the offset already on record. A stem is scored as a mono
						// downmix of two channels of a file whose other channels are the
						// same piece in another arrangement, so the deduction has more
						// near-identical alignments to choose between than a stereo entry
						// does, and picking a later loop pass is not a small error here:
						// on BGM_Con_Bahamut it moved ARR_FFXIV_117 from -171.6s to
						// -349.9s and left 133s of the calm stem playing silence.
						const auto covered = [&](double offset) {
							return static_cast<double>((std::max)(left.size(), right.size())) / static_cast<double>(samplingRate) + offset;
						};
						if (covered(deduced.Seconds) >= covered(stem.Offset) - 0.5) {
							stem.Deduced = std::abs(deduced.Seconds - stem.Offset) > 0.05;
							stem.EffectiveOffset = deduced.Seconds;
						}
					}
				} catch (const std::exception&) {
					// As in the stereo path: a failed deduction keeps the recorded offset
					// rather than losing the stem.
				}
			}

			// Rebase onto the game's timeline while laying the stem out: the source sample
			// at (i - offset) is what plays at game sample i. Writing straight into a
			// target-length buffer does the padding and the trimming in one pass, and
			// records how far the recording actually reaches.
			const auto offsetSamples = std::llround(stem.EffectiveOffset * static_cast<double>(samplingRate));
			std::vector<float> stereo(templateSamples * 2, 0.f);
			size_t reached = 0;
			for (size_t i = 0; i < templateSamples; ++i) {
				const auto sourceIndex = static_cast<long long>(i) - offsetSamples;
				if (sourceIndex < 0)
					continue;  // the game's file starts before the recording does: hold silence
				const auto si = static_cast<size_t>(sourceIndex);
				if (si >= left.size() && si >= right.size())
					break;
				stereo[i * 2] = si < left.size() ? left[si] : 0.f;
				stereo[i * 2 + 1] = si < right.size() ? right[si] : 0.f;
				reached = i + 1;
			}
			// Everything past the loop end is unreachable, so a shortfall only counts up to
			// there -- which rarely helps: a looping entry's loop end sits within about a
			// second of its own length.
			if (const auto needed = loopEnd > loopStart ? loopEnd : templateSamples; reached < needed)
				stem.ShortfallSeconds = static_cast<double>(needed - reached) / static_cast<double>(samplingRate);

			// Level-match this stem against the same two channels of the file it replaces.
			// Measuring the whole entry would read every engine state at once, which is
			// louder than any one of them and would pull every stem down.
			if (loudnessMatch && loopEnd > loopStart) {
				const auto spanSeconds = static_cast<double>(loopEnd - loopStart) / static_cast<double>(samplingRate);
				const auto templateStartSeconds = static_cast<double>(templateLoopStart) / static_cast<double>(templateRate);
				const auto sourceStartSeconds = static_cast<double>(loopStart) / static_cast<double>(samplingRate) - stem.EffectiveOffset;
				try {
					const auto templateLufs = measure_loudness(ffmpeg, templateAudio, templateStartSeconds, spanSeconds,
						std::format(L"pan=stereo|c0=c{}|c1=c{}", stem.LeftChannel, stem.RightChannel));
					const auto sourceLufs = measure_loudness(ffmpeg, stem.SourcePath, sourceStartSeconds, spanSeconds);
					stem.GainDb = std::clamp(templateLufs - sourceLufs, -maxGainDb, maxGainDb);
					const auto gain = std::pow(10., stem.GainDb / 20.);
					for (auto& v : stereo)
						v = static_cast<float>(v * gain);
					// Backing off uniformly rather than letting the encoder fold peaks over,
					// as the stereo path does -- but per stem, since one loud stem must not
					// quieten the others.
					if (const auto peak = std::abs(*std::ranges::max_element(stereo, [](float a, float b) { return std::abs(a) < std::abs(b); }));
						peak > 1.f) {
						for (auto& v : stereo)
							v = v / peak;
						stem.GainDb += 20. * std::log10(1. / static_cast<double>(peak));
					}
				} catch (const std::exception&) {
					stem.GainDb = 0.;
				}
			}

			// The onset window comes straight out of the template channels decoded above,
			// so reproducing a stem's authored fade-in costs no extra decode.
			if (onsetMatch && onsetWindow) {
				std::vector<float> templateOnset(onsetWindow * 2, 0.f);
				const auto& templateLeft = templateChannels[stem.LeftChannel];
				const auto& templateRight = templateChannels[stem.RightChannel];
				for (size_t i = 0; i < onsetWindow; ++i) {
					templateOnset[i * 2] = i < templateLeft.size() ? templateLeft[i] : 0.f;
					templateOnset[i * 2 + 1] = i < templateRight.size() ? templateRight[i] : 0.f;
				}
				apply_onset_correction(stereo, templateOnset, 2, samplingRate, stem.OnsetDb, stem.OnsetSeconds);
			}

			for (size_t i = 0; i < templateSamples; ++i) {
				out[i * channels + stem.LeftChannel] = stereo[i * 2];
				out[i * channels + stem.RightChannel] = stereo[i * 2 + 1];
			}
		}

		return out;
	}

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
		const std::string& album = {}) {

		const auto search = config.find("searchDirectories");
		if (search == config.end() || !search->is_object())
			return {ostDir};

		std::string wanted = album;
		if (wanted.empty()) {
			for (const auto& [name, spec] : search->items()) {
				if (wanted.empty() || (spec.is_object() && spec.value("default", false)))
					wanted = name;
				if (spec.is_object() && spec.value("default", false))
					break;
			}
		} else if (!search->contains(album)) {
			// Naming a directory the preset never declared would reach a release the user was
			// never told this preset needs, so it resolves to nothing instead.
			return {};
		}
		if (wanted.empty())
			return {};

		const auto lower = [](std::string text) {
			std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		};
		const auto target = lower(wanted);
		std::vector<std::filesystem::path> dirs;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(ostDir, ec)) {
			if (!entry.is_directory())
				continue;
			const auto name = lower(u8(entry.path().filename()));
			if (name == target || (name.size() > target.size() && name.ends_with(target)))
				dirs.push_back(entry.path());
		}
		return dirs;
	}

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
		const nlohmann::json& patterns) {

		// (pattern, the directories to look in -- empty meaning the album's own)
		std::vector<std::pair<std::string, std::vector<std::filesystem::path>>> alternatives;
		const auto add = [&](const nlohmann::json& one) {
			if (one.is_string()) {
				alternatives.emplace_back(one.get<std::string>(), dirs);
			} else if (one.is_object() && one.contains("pattern")) {
				auto scoped = dirs;
				if (const auto directory = one.find("directory"); directory != one.end() && directory->is_string())
					scoped = resolve_search_directories(ostDir, config, directory->get<std::string>());
				alternatives.emplace_back(one.at("pattern").get<std::string>(), std::move(scoped));
			}
		};

		if (patterns.is_object() && patterns.contains("inputFiles")) {
			const auto& files = patterns.at("inputFiles");
			if (files.is_array() && !files.empty()) {
				// Only the first entry: this path replaces one stream, so a list of files to
				// join is not something it can honour, and taking the first silently would be
				// worse than the miss that not resolving produces.
				if (files.size() > 1)
					return std::nullopt;
				for (const auto& one : files.front().is_array() ? files.front() : nlohmann::json::array({files.front()}))
					add(one);
			}
		} else if (patterns.is_array()) {
			for (const auto& one : patterns)
				add(one);
		} else {
			add(patterns);
		}

		for (const auto& [pattern, where] : alternatives) {
			std::regex re;
			try {
				re = std::regex(pattern, std::regex::icase);
			} catch (const std::regex_error&) {
				continue;
			}
			std::vector<std::filesystem::path> hits;
			for (const auto& dir : where) {
				std::error_code ec;
				// Recursive: a release with more tracks than one disc holds keeps the rest in
				// a subdirectory, and 131 targets resolved to nothing while this only looked
				// at the album's top level.
				for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
					if (!entry.is_regular_file())
						continue;
					if (std::regex_search(u8(entry.path().filename()), re))
						hits.push_back(entry.path());
				}
			}
			if (hits.size() == 1)
				return hits.front();
			// The same track in two encodings is one track, and the album's own lossless copy
			// is the one to take -- an .mp3 beside a .flac is a convenience copy, not a rival.
			if (hits.size() > 1) {
				std::vector<std::filesystem::path> lossless;
				for (const auto& hit : hits) {
					const auto extension = hit.extension();
					if (extension == L".flac" || extension == L".wav")
						lossless.push_back(hit);
				}
				if (lossless.size() == 1)
					return lossless.front();
				throw std::runtime_error(std::format("\"{}\" names {} files; it has to name one.", pattern, hits.size()));
			}
		}
		return std::nullopt;
	}

	// The first game path a target names, for a diagnostic that has nothing else to
	// identify it by.
	std::string collect_config_target_path(const nlohmann::json& target) {
		if (const auto path = target.find("path"); path != target.end()) {
			if (path->is_string())
				return path->get<std::string>();
			if (path->is_array() && !path->empty() && path->front().is_string())
				return path->front().get<std::string>();
		}
		return "(unnamed target)";
	}

	// Turn one MusicImportConfig target into jobs -- one per game path it lists, since a
	// target may name several .scd files that carry the same music.
	void collect_config_target(
		const std::filesystem::path& ostDir,
		const nlohmann::json& config,
		const nlohmann::json& sourceSpec,
		const nlohmann::json& target,
		std::vector<apply_job>& jobs,
		std::vector<std::pair<std::string, std::string>>& unresolved) {

		if (target.value("enable", true) == false)
			return;
		// A target that needs no offset and no filter says so by leaving `segments` out
		// altogether -- it plays its source from the start, whole. 54 targets, most of them
		// Orchestrion rolls, are written that way, and skipping them for want of the key
		// lost every one.
		const auto segmentsJson = target.find("segments");
		const auto hasSegments = segmentsJson != target.end() && segmentsJson->is_array() && !segmentsJson->empty();
		if (!hasSegments && target.contains("segments"))
			return;   // present but empty: the entry says nothing to build

		std::vector<std::string> paths;
		if (const auto path = target.find("path"); path != target.end()) {
			if (path->is_string())
				paths.push_back(path->get<std::string>());
			else if (path->is_array())
				for (const auto& one : *path)
					if (one.is_string())
						paths.push_back(one.get<std::string>());
		}
		if (paths.empty())
			return;

		// "source" is either one list of alternatives -- the implicit name "source" -- or
		// an object of named ones, which is what a multi-source segment refers to.
		std::map<std::string, nlohmann::json> named;
		if (sourceSpec.is_object())
			for (const auto& [name, patterns] : sourceSpec.items())
				named.emplace(name, patterns);
		else
			named.emplace("source", sourceSpec);

		const auto dirs = resolve_search_directories(ostDir, config);
		std::map<std::string, std::filesystem::path> resolved;
		for (const auto& [name, patterns] : named) {
			// A source may carry a `filterComplex`: an ffmpeg graph building it from several
			// inputs, layered rather than sequenced. Segments cannot express that -- the three
			// copies of one recording at 0s, 75.195s and 150.390s that BGM_EX4_Event_15 is
			// made of all sound at once. Ignoring the field and reading the graph's first input
			// as if it were the whole source would build something confidently wrong, so the
			// entry is declined and named instead.
			if (patterns.is_object() && patterns.contains("filterComplex")) {
				unresolved.emplace_back(paths.front(), std::format(
					"source \"{}\" is built by a filterComplex, which segments cannot express", name));
				return;
			}
			const auto file = resolve_source_name(ostDir, config, dirs, patterns);
			if (!file) {
				unresolved.emplace_back(paths.front(), std::format("no file for \"{}\"", name));
				return;
			}
			resolved.emplace(name, *file);
		}

		// Every name a segment uses has to be one the item defines. Two entries referred to
		// their recording by its stem while declaring it as a bare array -- which names it
		// `source` -- and the offset attached to the stem name was quietly dropped, leaving
		// BGM_EX5_Boss_Battle03 a second out of step and 0.10 worse than the previous build.
		// Silence is the wrong answer to that, so it is reported and the entry left alone.
		if (hasSegments) {
			for (const auto& segmentJson : *segmentsJson) {
				std::vector<std::string> used;
				for (const auto& key : {"sourceOffsets", "sourceFilters"})
					if (const auto section = segmentJson.find(key); section != segmentJson.end() && section->is_object())
						for (const auto& [name, _spec] : section->items())
							used.push_back(name);
				if (const auto channels = segmentJson.find("channels"); channels != segmentJson.end() && channels->is_array())
					for (const auto& channel : *channels)
						used.push_back(channel.value("source", std::string("source")));
				for (const auto& name : used) {
					if (name == "target" || resolved.contains(name))
						continue;
					unresolved.emplace_back(paths.front(), std::format(
						"segment names source \"{}\", which the item does not define", name));
					return;
				}
			}
		}

		std::vector<apply_segment> segments;
		if (!hasSegments) {
			// One default span covering the whole of the single source it names. More than
			// one source with no segments to route them through says nothing about which
			// channel each feeds, so there is nothing to build.
			if (resolved.size() != 1)
				return;
			apply_segment segment;
			segment.Sources.emplace(resolved.begin()->first, apply_segment_source{.Path = resolved.begin()->second});
			// Two entries, taken in order: the single-source path below reads only the source
			// and the offset from this, and derives the channel count from the game's own file.
			segment.Channels.emplace_back(resolved.begin()->first, 0);
			segment.Channels.emplace_back(resolved.begin()->first, 1);
			segments.push_back(std::move(segment));
		}
		static const nlohmann::json NoSegments = nlohmann::json::array();
		for (const auto& segmentJson : hasSegments ? *segmentsJson : NoSegments) {
			apply_segment segment{
				.Length = segmentJson.value("length", 0.),
				.CrossfadeSeconds = segmentJson.value("crossfadeSeconds", 0.),
				.StartSeconds = segmentJson.value("startSeconds", -1.),
				.FadeInSeconds = segmentJson.value("fadeInSeconds", -1.),
				.FadeOutSeconds = segmentJson.value("fadeOutSeconds", -1.),
			};
			for (const auto& [name, path] : resolved)
				segment.Sources.emplace(name, apply_segment_source{.Path = path});
			if (const auto offsets = segmentJson.find("sourceOffsets"); offsets != segmentJson.end() && offsets->is_object()) {
				for (const auto& [name, spec] : offsets->items()) {
					if (const auto source = segment.Sources.find(name); source != segment.Sources.end()) {
						source->second.Offset = spec.is_object() ? spec.value("offset", 0.) : spec.get<double>();
						source->second.Stated = true;
					}
				}
			}
			if (const auto filters = segmentJson.find("sourceFilters"); filters != segmentJson.end() && filters->is_object()) {
				for (const auto& [name, filter] : filters->items()) {
					if (const auto source = segment.Sources.find(name); source != segment.Sources.end() && filter.is_string())
						source->second.Filter = xivres::util::unicode::convert<std::wstring>(filter.get<std::string>());
				}
			}
			if (const auto channels = segmentJson.find("channels"); channels != segmentJson.end() && channels->is_array()) {
				for (const auto& channel : *channels)
					segment.Channels.emplace_back(channel.value("source", std::string("source")),
						channel.value("channel", size_t{0}));
			}
			if (segment.Channels.empty())
				return;
			segments.push_back(std::move(segment));
		}
		if (segments.empty())
			return;

		// One span of one recording, its channels taken in order, is exactly what the
		// single-source path already builds -- and that path carries the treatments a
		// segment assembly has no way to reach: the sample-accurate alignment around the
		// loop point, which a preset's millisecond-rounded offset has lost by the time it
		// is written down. So the assembler is kept for what only it can do.
		//
		// But only when the segment asks for nothing that path cannot honour. It reads the
		// source and the offset and nothing else, so a lone segment stating a length, a
		// start, or a fade was having that silently dropped: a window of 40s came out as the
		// whole 49.29s recording, with no error. No entry in presets/ or presets-manual/
		// states any of these on a single identity-mapped segment, so this only ever turns
		// away the shapes that were being mis-built.
		const auto& first = segments.front();
		const auto shaped = first.Length > 0. || first.StartSeconds >= 0.
			|| first.CrossfadeSeconds > 0. || first.FadeInSeconds >= 0. || first.FadeOutSeconds >= 0.;
		const auto plain = segments.size() == 1 && first.Sources.size() == 1 && !shaped;
		bool sequential = plain;
		if (plain) {
			for (size_t ch = 0; ch < segments.front().Channels.size(); ch++)
				sequential = sequential && segments.front().Channels[ch].second == ch;
		}

		for (const auto& path : paths) {
			if (sequential) {
				const auto& only = segments.front().Sources.begin()->second;
				jobs.push_back({
					.TargetPath = path,
					.SourcePath = only.Path,
					.Score = 1.,
					// Opposite signs. A MusicImportConfig offset says where in the recording
					// this target begins, so it counts forward into the source; the matchset's
					// says how far the recording sits from the game's timeline, so a positive
					// one pads. Carried across unchanged, a field track keyed at +168.705s got
					// 168s of silence in front of the whole recording instead of starting there.
					.Offset = -only.Offset,
					.Filter = only.Filter,
					.FromPreset = true,
					.Note = target.value("# comment", std::string{}),
				});
				continue;
			}
			jobs.push_back({
				.TargetPath = path,
				.Score = 1.,
				.Segments = segments,
				.FromPreset = true,
				.Note = target.value("# comment", std::string{}),
			});
		}
	}

namespace {
	// The 16-bit samples every format here is built from.
	//
	// 16-bit because that is all the game's decoder emits, whatever the stream behind it was,
	// so quantising once up front is the step the decoder was going to take anyway. A looping
	// entry is only ever heard up to its loop end -- the game cuts back from there -- so the
	// audio stops there too, which is what the libvorbis path's block loop does and what
	// --verify measures the written length against.
	std::vector<int16_t> quantise_pcm16(const std::vector<float>& floats, size_t channels, size_t loopEndBlockIndex) {
		auto frames = channels ? floats.size() / channels : 0;
		if (loopEndBlockIndex && loopEndBlockIndex < frames)
			frames = loopEndBlockIndex;

		constexpr double FullScale = 32767.;
		std::vector<int16_t> pcm(frames * channels);
		for (size_t i = 0; i < pcm.size(); i++) {
			const auto v = std::lround(static_cast<double>(floats[i]) * FullScale);
			pcm[i] = static_cast<int16_t>(std::clamp<long>(v, -32768, 32767));
		}
		return pcm;
	}

	// Builds the sound entry as *lossless* Vorbis: the buffer goes through scdtool's own
	// encoder, whose output decodes back to bit-identical 16-bit PCM, and the Ogg stream it
	// produces is wrapped rather than re-encoded.
	xivres::sound::writer::sound_item make_lossless_ogg_entry(
		const std::vector<int16_t>& pcm,
		size_t channels,
		size_t samplingRate,
		size_t loopStartBlockIndex,
		size_t loopEndBlockIndex,
		std::span<const uint32_t> markIndices,
		std::string& reportOut) {

		lossless_vorbis::options opts;
		// The game's decoder rounds on some paths and truncates on others -- the 3-or-more
		// channel path truncates throughout -- so aim for the value that survives either.
		opts.Rounding = lossless_vorbis::rounding::Either;
		if (loopStartBlockIndex || loopEndBlockIndex) {
			opts.Comments.push_back(std::format("LoopStart={}", loopStartBlockIndex));
			opts.Comments.push_back(std::format("LoopEnd={}", loopEndBlockIndex));
		}

		const auto encoded = lossless_vorbis::encode(pcm, channels, samplingRate, opts);
		if (!encoded.Exact)
			throw std::runtime_error(std::format(
				"lossless: {} of {} samples would not decode back exactly (worst {:.3f} LSB)",
				encoded.Mismatches, pcm.size(), encoded.WorstErrorLsb));
		reportOut = std::format("{:.2f} bits/sample, {} levels x {} rungs, {} pass(es)",
			encoded.BitsPerSample, encoded.Levels, encoded.Stages, encoded.Iterations);

		auto entry = xivres::sound::writer::sound_item::make_from_ogg(
			xivres::memory_stream(encoded.Ogg).as_linear_reader<uint8_t>());
		if (!markIndices.empty())
			entry.set_mark_chunks(static_cast<uint32_t>(loopStartBlockIndex),
				static_cast<uint32_t>(loopEndBlockIndex), markIndices);
		return entry;
	}

	// What --audio-format asked for: which encoder builds the entry, and the one number that
	// encoder's own scale takes.
	struct audio_format {
		enum class codec {
			OggVorbis,
			LosslessVorbis,
			Flac,
			Pcm,
		};

		codec Codec = codec::OggVorbis;
		float OggQuality = 1.f;    // libvorbis's -0.1 to 1.0, a tenth of oggenc's
		size_t FlacLevel = 5;      // libFLAC's 0 to 8
	};

	// codec[:setting]. Malformed and out-of-range values are refused rather than clamped:
	// silently encoding at a quality nobody asked for is worse than not encoding, and a whole
	// run of it is expensive to discover afterwards.
	//
	// `option` is the spelling the value arrived under, so the message names the flag the
	// caller actually typed.
	audio_format parse_audio_format(const std::string& spec, const std::string& option) {
		constexpr auto grammar = R"(ogg, ogg:<quality -1 to 10>, ogg:lossless, flac, flac:<level 0 to 8>, or wav)";
		const auto colon = spec.find(':');
		const auto name = spec.substr(0, colon);
		const auto hasSetting = colon != std::string::npos;
		const auto setting = hasSetting ? spec.substr(colon + 1) : std::string();

		audio_format res;
		if (name == "wav") {
			if (hasSetting)
				throw std::runtime_error(std::format(
					R"({}: "wav" is raw PCM and takes no setting, not "{}")", option, setting));
			res.Codec = audio_format::codec::Pcm;
			return res;
		}

		if (name == "flac") {
			// An integer, because libFLAC's levels are names for preset combinations of
			// settings rather than points on a continuum -- 5.5 would mean nothing.
			res.Codec = audio_format::codec::Flac;
			if (!hasSetting)
				return res;
			if (setting.empty() || setting.find_first_not_of("0123456789") != std::string::npos)
				throw std::runtime_error(std::format(
					R"({}: expected a whole number from 0 to 8 after "flac:", not "{}")", option, setting));
			const auto level = std::stoul(setting);
			if (level > 8)
				throw std::runtime_error(std::format(
					"{}: FLAC compression level {} is outside the 0 to 8 the encoder accepts", option, setting));
			res.FlacLevel = level;
			return res;
		}

		if (name != "ogg")
			throw std::runtime_error(std::format(
				R"({}: expected {}, not "{}")", option, grammar, spec));

		// Quality is oggenc's scale, -1 to 10, because that is the one people know; libvorbis
		// itself takes -0.1 to 1.0 and the two differ only by a factor of ten. "lossless" sits
		// at the end of the same axis rather than on a flag of its own: it is a choice about
		// how the entry is encoded, which is what this option is for.
		if (!hasSetting)
			return res;
		if (setting == "lossless") {
			res.Codec = audio_format::codec::LosslessVorbis;
			return res;
		}
		size_t consumed = 0;
		double value;
		try {
			value = std::stod(setting, &consumed);
		} catch (const std::exception&) {
			consumed = 0;
			value = 0;
		}
		if (!consumed || consumed != setting.size())
			throw std::runtime_error(std::format(
				R"({}: expected a number from -1 to 10 or "lossless" after "ogg:", not "{}")", option, setting));
		if (value < -1. || value > 10.)
			throw std::runtime_error(std::format(
				"{}: quality {} is outside the -1 to 10 the encoder accepts", option, setting));
		res.OggQuality = static_cast<float>(value / 10.);
		return res;
	}
}

int cmd_apply(const std::vector<std::string>& args) {
	argparse::ArgumentParser parser("scdtool apply");
	try {
		parser
			.add_description("Rewrite game .scd music files from the matched sources of a preset produced by `scdtool match`.")
			.add_epilog(
				"Only entries whose matchInfo.status is \"matched\" and whose score reaches --min-score are\n"
				"written; everything else is left for manual review. Each output .scd is built from the\n"
				"game's own file as a template, so the surrounding tables, every other sound entry, and the\n"
				"track's loop points are preserved, and only the audio is replaced. Output files are written\n"
				"under --output-dir using the target's game-relative path.\n"
				"\n"
				"--audio-format names the codec and its setting as codec[:setting]:\n"
				"\n"
				"  ogg            Ogg Vorbis at quality 10, which is the default for the whole option\n"
				"  ogg:<quality>  Ogg Vorbis, -1 to 10 on oggenc's scale; libvorbis is handed a tenth\n"
				"  ogg:lossless   the Vorbis encoder built into this tool, whose output decodes back\n"
				"                 to bit-identical 16-bit PCM -- which is all the game's decoder emits\n"
				"  flac           FLAC at compression level 5, libFLAC's own default\n"
				"  flac:<level>   FLAC, 0 (fastest) to 8 (smallest)\n"
				"  wav            raw interleaved 16-bit PCM\n"
				"\n"
				"ogg and ogg:lossless produce files the game plays as it ships. flac and wav do not:\n"
				"they keep the entry's format 6 and its whole layout -- seek table, byte-offset loop\n"
				"fields, MARK chunk, every other entry of the .scd -- but the stream inside it is a\n"
				"FLAC file or a RIFF/WAVE one, which only a decoder-substitution hook can read.\n"
				"\n"
				"Sizes, measured on one stereo track at the game's own 44.1 kHz (44.0 MB of raw\n"
				"samples, against 4.9 MB for the file the game ships): ogg 13.3 MB, flac 22.0 MB,\n"
				"ogg:lossless 37.9 MB, wav 44.0 MB. Six-channel stems compress worse -- flac came out\n"
				"at 0.64x raw PCM on the one measured. Only ogg:lossless is slow, a few seconds to\n"
				"half a minute an entry; the rest keep up with the decode that feeds them. Pair any\n"
				"of the three large ones with --sampling-rate keep: at 96 kHz they cost roughly twice\n"
				"as much for precision the decoder cannot carry.");
		parser.add_argument("--game").required().help(R"(game installation path, or :global/:china/:korea to autodetect)");
		parser.add_argument("--ost").required().help("directory the preset's source paths are relative to");
		parser.add_argument("--preset").required().help("a matchset JSON produced by `scdtool match`, or a MusicImportConfig preset (or a directory of them)");
		parser.add_argument("--output-dir").required().help("directory to write replacement .scd files into");
		parser.add_argument("--ffmpeg").default_value(std::string("ffmpeg")).help("path to ffmpeg executable");
		parser.add_argument("--ffprobe").default_value(std::string("ffprobe")).help("path to ffprobe executable");
		parser.add_argument("--sampling-rate").default_value(std::string("auto")).help(R"(output sample rate: "auto" (highest of the game file and the source), "keep" (the game file's), or an integer)");
		parser.add_argument("--entry-index").default_value(0u).scan<'u', uint32_t>().help("sound entry index to replace (default: 0)");
		parser.add_argument("--audio-format").default_value(std::string("ogg")).help(R"(what the entry's audio is, as codec[:setting]: "ogg", "ogg:<-1 to 10>", "ogg:lossless", "flac", "flac:<0 to 8>" or "wav" (default: ogg, which is quality 10))");
		// The flag was --ogg-quality while Ogg Vorbis was the only thing it could produce, and
		// its values were bare -- "10", "lossless". Both still work, and mean the same as
		// "ogg:10" and "ogg:lossless", so a script written against it keeps running.
		parser.add_argument("--ogg-quality").help(R"(the former spelling of --audio-format, whose value is an Ogg Vorbis setting on its own: "10", "lossless")");
		parser.add_argument("--min-score").default_value(0.95).scan<'g', double>().help("only rewrite entries matched at or above this correlation score");
		parser.add_argument("--dry-run").default_value(false).implicit_value(true).help("list what would be written without writing anything");
		parser.add_argument("--verify").default_value(false).implicit_value(true).help("re-read each written file and check its loop points and length survived the round trip (slower)");
		parser.add_argument("--loudness-match").default_value(true).implicit_value(true).help("gain-match each replacement to the loudness of the loop region of the file it replaces; --no-loudness-match disables");
		parser.add_argument("--no-loudness-match").default_value(false).implicit_value(true).help("disable --loudness-match");
		parser.add_argument("--max-gain").default_value(12.0).scan<'g', double>().help("clamp on loudness matching gain, in dB");
		parser.add_argument("--onset-match").default_value(true).implicit_value(true).help("reproduce a fade-in or held silence the game's own file has at its start but the OST source does not; --no-onset-match disables");
		parser.add_argument("--no-onset-match").default_value(false).implicit_value(true).help("disable --onset-match");
		parser.add_argument("--auto-offset").default_value(true).implicit_value(true).help("re-derive each match's source offset against the game's own file instead of trusting the recorded one, judged on the pre-loop intro; --no-auto-offset uses the recorded offset verbatim");
		parser.add_argument("--no-auto-offset").default_value(false).implicit_value(true).help("disable --auto-offset");
		parser.add_argument("--emit-original").default_value(false).implicit_value(true).help("also write the game's own file next to each replacement as <name>.orig.scd, for A/B comparison");
		parser.parse_args(args);
	} catch (const std::exception& e) {
		std::cerr
			<< "Error parsing arguments. Use `apply -h` to show help.\n"
			<< e.what() << '\n';
		return -1;
	}

	std::vector<std::filesystem::path> tempFiles;
	const auto cleanupTempFiles = [&] {
		for (const auto& p : tempFiles) {
			std::error_code ec;
			std::filesystem::remove(p, ec);
		}
	};

	try {
		const auto gameSpec = parser.get<std::string>("--game");
		const auto ostDir = argactions::path(parser.get<std::string>("--ost"));
		const auto presetPath = argactions::path(parser.get<std::string>("--preset"));
		const auto outputDir = argactions::path(parser.get<std::string>("--output-dir"));
		const auto ffmpegPath = argactions::path(parser.get<std::string>("--ffmpeg"));
		const auto ffprobePath = argactions::path(parser.get<std::string>("--ffprobe"));
		const auto samplingRateSpec = parser.get<std::string>("--sampling-rate");
		const auto entryIndex = parser.get<uint32_t>("--entry-index");
		// The old spelling carries an Ogg Vorbis setting with no codec in front of it, which
		// is the same thing the new one says once "ogg:" is put back on the front.
		if (parser.is_used("--ogg-quality") && parser.is_used("--audio-format"))
			throw std::runtime_error("--audio-format and --ogg-quality both set the same thing; give one of them.");
		const auto audioFormat = parser.is_used("--ogg-quality")
			? parse_audio_format("ogg:" + parser.get<std::string>("--ogg-quality"), "--ogg-quality")
			: parse_audio_format(parser.get<std::string>("--audio-format"), "--audio-format");
		// Whether the entry ends up holding a stream the game's own decoder can read. Two
		// things turn on it further down: the six-channel permutation, which is a fact about
		// Vorbis rather than about the audio, and how --verify reads the file back.
		const auto encodesVorbis = audioFormat.Codec == audio_format::codec::OggVorbis
			|| audioFormat.Codec == audio_format::codec::LosslessVorbis;
		const auto minScore = parser.get<double>("--min-score");
		const auto dryRun = parser.get<bool>("--dry-run");
		const auto verify = parser.get<bool>("--verify");
		const auto loudnessMatch = parser.get<bool>("--loudness-match") && !parser.get<bool>("--no-loudness-match");
		const auto onsetMatch = parser.get<bool>("--onset-match") && !parser.get<bool>("--no-onset-match");
		const auto autoOffset = parser.get<bool>("--auto-offset") && !parser.get<bool>("--no-auto-offset");
		const auto emitOriginal = parser.get<bool>("--emit-original");
		const auto maxGainDb = parser.get<double>("--max-gain");

		const xivres::installation installation(argactions::installation_root(gameSpec));

		// --preset takes either form. A MusicImportConfig is the format this tool emits and
		// the one the importer reads, and it can say things a matchset cannot -- segments,
		// crossfades, a different recording per span -- so pointing at the presets directory
		// builds everything from the same files that ship, with no conversion step between.
		std::vector<std::filesystem::path> presetPaths;
		if (std::filesystem::is_directory(presetPath)) {
			// In release order, which each preset states in its `name` -- "Final Fantasy XIV
			// - 2.5 - Before The Fall". Order decides which album serves a target listed by
			// more than one, and the two releases of a piece are rarely the same recording:
			// by filename, A Realm Reborn would claim BGM_Ban_Ifrit from Before Meteor and
			// come out 0.006 further from the game's own file.
			std::vector<std::pair<std::string, std::filesystem::path>> ordered;
			for (const auto& entry : std::filesystem::directory_iterator(presetPath)) {
				if (!entry.is_regular_file() || entry.path().extension() != L".json")
					continue;
				std::string name;
				try {
					std::ifstream f(entry.path(), std::ios::binary);
					nlohmann::json head;
					f >> head;
					name = head.value("name", std::string{});
				} catch (const std::exception&) {
					// Unreadable here is reported properly when it is loaded below.
				}
				// A preset that names no release sorts by its filename, after every one that does.
				ordered.emplace_back(name.empty() ? "ÿ" + u8(entry.path().filename()) : name, entry.path());
			}
			std::ranges::sort(ordered);
			for (auto& [_name, path] : ordered)
				presetPaths.push_back(std::move(path));
			if (presetPaths.empty())
				throw std::runtime_error(std::format("No .json presets in {}", u8(presetPath)));
		} else {
			presetPaths.push_back(presetPath);
		}

		std::vector<apply_job> jobs;
		size_t skippedUnmatched = 0;
		std::vector<std::pair<std::string, std::string>> unresolvedSources;
		std::set<std::string> seenTargets;

		for (const auto& onePresetPath : presetPaths) {
		nlohmann::json preset;
		{
			std::ifstream f(onePresetPath, std::ios::binary);
			if (!f)
				throw std::runtime_error(std::format("Could not open preset file: {}", u8(onePresetPath)));
			f >> preset;
		}
		if (!preset.contains("items") || !preset["items"].is_array())
			throw std::runtime_error(std::format("{} has no \"items\" array.", u8(onePresetPath)));

		// A MusicImportConfig announces itself by naming the albums it needs.
		if (preset.contains("searchDirectories")) {
			const auto before = jobs.size();
			for (const auto& item : preset.at("items")) {
				const auto source = item.find("source");
				const auto target = item.find("target");
				if (source == item.end() || target == item.end())
					continue;
				for (const auto& one : target->is_array() ? *target : nlohmann::json::array({*target})) {
					try {
						collect_config_target(ostDir, preset, *source, one, jobs, unresolvedSources);
					} catch (const std::exception& e) {
						unresolvedSources.emplace_back(collect_config_target_path(one), e.what());
					}
				}
			}
			// Albums overlap: a track reissued on a later release is listed by both, so that
			// owning either one is enough, and the first listing that resolves is the one
			// used. Only this preset's own additions are weighed: a sweep over the whole list
			// would meet every job kept by an earlier preset a second time and drop it as a
			// duplicate of itself, which cost 1384 of 1546 entries before it was caught.
			auto write = jobs.begin() + static_cast<ptrdiff_t>(before);
			for (auto read = write; read != jobs.end(); ++read) {
				if (seenTargets.insert(read->TargetPath).second)
					*write++ = std::move(*read);
			}
			jobs.erase(write, jobs.end());
			continue;
		}

		for (const auto& item : preset.at("items")) {
			const auto info = item.find("matchInfo");
			if (info == item.end()) {
				skippedUnmatched++;
				continue;
			}

			const auto target = item.find("target");
			if (target == item.end() || !target->is_object())
				continue;
			const auto path = target->find("path");
			if (path == target->end() || !path->is_string())
				continue;

			// An engine-switched entry has no single source and no single score: each stem
			// is its own album track, matched on its own. Judge them one at a time and keep
			// the game's own audio for whichever did not resolve -- the alternative, gating
			// the whole entry on its weakest stem, throws away the two good stems of a
			// three-stem file because the cymbal layer is ambiguous, which is most of them.
			if (const auto stems = info->find("stems"); stems != info->end() && stems->is_array() && !stems->empty()) {
				apply_job job{.TargetPath = path->get<std::string>()};
				for (const auto& stem : *stems) {
					const auto channels = stem.find("channels");
					const auto source = stem.find("source");
					if (channels == stem.end() || !channels->is_array() || channels->size() != 2
						|| source == stem.end() || !source->is_string())
						continue;
					const auto stemScore = stem.value("score", 0.);
					auto first = channels->at(0).get<size_t>();
					auto second = channels->at(1).get<size_t>();
					// Lower raw index is the left channel; see build_stem_audio.
					if (first > second)
						std::swap(first, second);
					job.Stems.push_back({
						.SourcePath = ostDir / argactions::path(source->get<std::string>()),
						.Score = stemScore,
						.Offset = stem.value("offset", 0.),
						.LeftChannel = first,
						.RightChannel = second,
						.Matched = stem.value("status", "") == "matched" && stemScore >= minScore,
					});
					job.Score = (std::max)(job.Score, job.Stems.back().Matched ? stemScore : 0.);
				}
				if (std::ranges::none_of(job.Stems, [](const apply_stem& stem) { return stem.Matched; })) {
					skippedUnmatched++;
					continue;
				}
				jobs.push_back(std::move(job));
				continue;
			}

			if (info->value("status", "") != "matched") {
				skippedUnmatched++;
				continue;
			}
			const auto score = info->value("score", 0.);
			if (score < minScore) {
				skippedUnmatched++;
				continue;
			}

			// "file" is the exact OST file that was scored, relative to --ost.
			const auto file = info->find("file");
			if (file == info->end() || !file->is_string())
				continue;

			jobs.push_back({
				.TargetPath = path->get<std::string>(),
				.SourcePath = ostDir / argactions::path(file->get<std::string>()),
				.Score = score,
				.Offset = info->value("offset", 0.),
			});
		}
		}

		std::cerr << std::format("{} entr(ies) to rewrite, {} skipped (unmatched or below --min-score).",
			jobs.size(), skippedUnmatched) << '\n';
		// A target listed by several albums only has to resolve in one of them, so a miss
		// is only worth reporting when nothing ended up covering that target at all.
		{
			std::set<std::string> covered;
			for (const auto& job : jobs)
				covered.insert(job.TargetPath);
			std::erase_if(unresolvedSources, [&](const auto& entry) { return covered.contains(entry.first); });
			if (!unresolvedSources.empty()) {
				std::cerr << std::format("{} target(s) left uncovered for want of a source file:", unresolvedSources.size()) << '\n';
				for (const auto& [target, reason] : unresolvedSources)
					std::cerr << std::format("   {} <- {}", target, reason) << '\n';
			}
		}
		if (jobs.empty()) {
			std::cerr << "Nothing to do.\n";
			return 0;
		}

		if (dryRun) {
			for (const auto& job : jobs) {
				if (!job.Segments.empty()) {
					std::cerr << std::format("  would write {} <- {} segment(s)", job.TargetPath, job.Segments.size());
					for (const auto& segment : job.Segments) {
						std::cerr << "\n     ";
						for (const auto& [name, source] : segment.Sources)
							std::cerr << std::format(" {} @{:+.3f}s", u8(source.Path.filename()), source.Offset);
						if (segment.StartSeconds >= 0.)
							std::cerr << std::format(" at {:.3f}s", segment.StartSeconds);
						if (segment.Length > 0.)
							std::cerr << std::format(" for {:.3f}s", segment.Length);
						if (segment.CrossfadeSeconds > 0.)
							std::cerr << std::format(" fading in over {:.1f}s", segment.CrossfadeSeconds);
						if (segment.FadeInSeconds >= 0.)
							std::cerr << std::format(" fade in {:.3f}s", segment.FadeInSeconds);
						if (segment.FadeOutSeconds >= 0.)
							std::cerr << std::format(" fade out {:.3f}s", segment.FadeOutSeconds);
					}
					std::cerr << '\n';
					continue;
				}
				if (job.Stems.empty()) {
					std::cerr << std::format("  would write {} <- {} (score {:.3f})", job.TargetPath, u8(job.SourcePath), job.Score) << '\n';
					continue;
				}
				std::cerr << std::format("  would write {} <- {} stem(s)", job.TargetPath, job.Stems.size());
				for (const auto& stem : job.Stems)
					std::cerr << std::format("\n      channels {},{} <- {}", stem.LeftChannel, stem.RightChannel,
						stem.Matched ? std::format("{} (score {:.3f})", u8(stem.SourcePath), stem.Score)
						             : std::format("the game's own audio (best was {} at {:.3f})", u8(stem.SourcePath), stem.Score));
				std::cerr << '\n';
			}
			return 0;
		}

		const auto tempDir = std::filesystem::temp_directory_path();
		std::atomic<uint32_t> tempFileCounter{0};
		std::atomic<size_t> writtenCount{0};
		std::atomic<size_t> failedCount{0};
		std::mutex logMutex;

		// Each job decodes and holds a whole track as float32 in memory (hundreds of MB for
		// a long one), so the worker count is kept well below the core count to avoid
		// exhausting RAM -- hence an explicit cap instead of parallel_for's default.
		constexpr size_t MaxDecodeThreads = 4;
		parallel_for(jobs.size(), [&](size_t index) {
			const auto& job = jobs[index];
			// One entry that cannot be built must not cost a run of seventeen hundred: it is
			// reported where it happened and counted again at the end, so a failure partway
			// through a long build is neither fatal nor lost in the scrollback.
			try {

			// Temp files are released when this job ends, not when the whole run does.
			// Each job stages the template's audio and a raw decode of the source, which at
			// 96kHz is hundreds of megabytes; holding all of them until the end meant a
			// 1793-target batch wrote 220 GB into the temp directory and filled the drive.
			std::vector<std::filesystem::path> jobTemps;
			struct temp_sweeper {
				std::vector<std::filesystem::path>& Paths;
				~temp_sweeper() {
					for (const auto& path : Paths) {
						std::error_code ec;
						std::filesystem::remove(path, ec);
					}
				}
			} const releaseJobTemps{jobTemps};
			const auto keepTemp = [&jobTemps](std::filesystem::path path) {
				jobTemps.emplace_back(std::move(path));
				return jobTemps.back();
			};

			// Neither a stem job nor a segmented one has a single source: each stem carries
			// its own and only the resolved ones are read, and a segment's sources were
			// checked for existence when the preset was resolved.
			if (!job.Segments.empty()) {
				for (const auto& segment : job.Segments)
					for (const auto& [name, source] : segment.Sources)
						if (!std::filesystem::exists(source.Path))
							throw std::runtime_error(std::format("Source file not found: {}", u8(source.Path)));
			} else if (job.Stems.empty()) {
				if (!std::filesystem::exists(job.SourcePath))
					throw std::runtime_error(std::format("Source file not found: {}", u8(job.SourcePath)));
			} else {
				for (const auto& stem : job.Stems)
					if (stem.Matched && !std::filesystem::exists(stem.SourcePath))
						throw std::runtime_error(std::format("Source file not found: {}", u8(stem.SourcePath)));
			}

			const auto templateStream = installation.get_file(job.TargetPath);
			const xivres::sound::reader templateScd(templateStream);
			if (templateScd.sound_item_count() <= entryIndex)
				throw std::runtime_error(std::format("{} has only {} sound entries, cannot replace index {}.",
					job.TargetPath, templateScd.sound_item_count(), entryIndex));

			const auto templateItem = templateScd.read_sound_item(entryIndex);
			if (templateItem.Header->Format != xivres::sound::sound_entry_format::Ogg
				&& templateItem.Header->Format != xivres::sound::sound_entry_format::WaveFormatPcm)
				throw std::runtime_error(std::format("{} entry {} is neither Ogg nor PCM; refusing to replace it.",
					job.TargetPath, entryIndex));

			const auto [templateLoopStart, templateLoopEnd] = template_loop_points(templateItem);

			const auto channels = static_cast<size_t>(templateItem.Header->ChannelCount);
			const auto templateRate = static_cast<size_t>(templateItem.Header->SamplingRate);
			if (!channels || !templateRate)
				throw std::runtime_error(std::format("{} has no channels or sample rate.", job.TargetPath));

			// A 4- or 6-channel entry is a set of engine-switched stems, not a surround mix.
			// With per-stem matches in hand build_stem_audio can reproduce them; without
			// them, a single stereo source would collapse the calm/battle switching into
			// one state, so refuse rather than silently produce a file that behaves
			// differently in game.
			if (channels > 2 && job.Stems.empty() && job.Segments.empty()) {
				const auto lock = std::scoped_lock(logMutex);
				std::cerr << std::format("  SKIPPED {}: template has {} channels (engine-switched stems); a single stereo source cannot reproduce them",
					job.TargetPath, channels) << '\n';
				return;
			}

			// Staged once, unconditionally: the offset deduction below aligns against it,
			// the loudness measurement compares to it, and the onset check samples it, so
			// it is wanted whatever the flags say.
			const auto templateAudio = tempDir / std::format(L"scdtool_apply_src_{}.ogg", tempFileCounter.fetch_add(1));
			keepTemp(templateAudio);
			{
				std::ofstream f(templateAudio, std::ios::binary);
				if (!f)
					throw std::runtime_error(std::format("could not stage template audio for {}", job.TargetPath));
				const auto ogg = templateItem.get_ogg_file();
				f.write(reinterpret_cast<const char*>(ogg.data()), static_cast<std::streamsize>(ogg.size()));
			}

			// The SCD must be Ogg Vorbis, so a source that is lossless and high-rate can
			// only lose by being forced down to the game file's rate. Take the highest rate
			// available across the two, as MusicImporter's
			// SamplingRate_UseHighestAvailable did -- which is why its output was 96 kHz
			// where the game shipped 44.1 kHz.
			size_t samplingRate;
			if (samplingRateSpec == "auto") {
				samplingRate = templateRate;
				// A stem entry has no single source: take the highest rate across the ones
				// that actually resolved, so a 96 kHz stem is not resampled down to meet a
				// 44.1 kHz one sharing the same file.
				if (!job.Segments.empty()) {
					for (const auto& segment : job.Segments)
						for (const auto& [name, source] : segment.Sources)
							samplingRate = (std::max)(samplingRate, static_cast<size_t>(probe_sample_rate(ffprobePath, source.Path)));
				} else if (job.Stems.empty()) {
					samplingRate = (std::max)(samplingRate, static_cast<size_t>(probe_sample_rate(ffprobePath, job.SourcePath)));
				} else {
					for (const auto& stem : job.Stems)
						if (stem.Matched)
							samplingRate = (std::max)(samplingRate, static_cast<size_t>(probe_sample_rate(ffprobePath, stem.SourcePath)));
				}
			} else if (samplingRateSpec == "keep")
				samplingRate = templateRate;
			else
				samplingRate = static_cast<size_t>(std::stoul(samplingRateSpec));

			// Loop points are sample indices, so they only survive a rate change by being
			// rescaled -- otherwise a 96 kHz encode would move the loop to roughly the
			// wrong half of the track.
			const auto scaleLoop = [&](size_t value) {
				return static_cast<size_t>(static_cast<uint64_t>(value) * samplingRate / templateRate);
			};
			const auto loopStart = scaleLoop(templateLoopStart);
			const auto loopEnd = scaleLoop(templateLoopEnd);

			// Everything from here to the encode differs between a single-source entry and
			// an engine-switched one, but they converge on the same four values: the audio,
			// where it was taken from, and the loop it has to carry.
			std::vector<float> floats;
			// A local copy: build_stem_audio fills in what each stem actually resolved to,
			// which the log line below reports, and `jobs` is shared across workers.
			auto stems = job.Stems;
			auto effectiveOffset = job.Offset;
			deduced_offset deduced;
			sample_alignment aligned;
			double offsetBeforeAlignment = job.Offset;
			size_t paddingAdded = 0;
			size_t trimmedAway = 0;
			double gainDb = 0.;
			double requestedGainDb = 0.;
			bool gainLimited = false;
			double onsetDb = 0.;
			double onsetSeconds = 0.;
			size_t totalSamples = 0;
			size_t newLoopStart = 0;
			size_t newLoopEnd = 0;

			if (!job.Segments.empty()) {
				const auto tempFile = [&](const wchar_t* prefix, const wchar_t* extension) {
					return keepTemp(tempDir / std::format(L"{}_{}{}", prefix,
						tempFileCounter.fetch_add(1), extension));
				};
				// A preset's offsets, lengths and crossfades were fitted against this very file
				// and carry a checksum of the recording they were fitted to, so they are used as
				// written -- re-deriving them here would throw away the one thing the preset knows
				// that the matcher does not, which is where the seams go.
				floats = build_segment_audio(ffmpegPath, templateAudio, job.Segments, channels,
					samplingRate, loudnessMatch, maxGainDb, tempFile);

				totalSamples = floats.size() / channels;
				newLoopStart = loopStart;
				newLoopEnd = (std::min)(loopEnd, totalSamples);
				if (newLoopStart >= totalSamples) {
					newLoopStart = 0;
					newLoopEnd = 0;
				}
			} else if (!job.Stems.empty()) {
				const auto tempFile = [&](const wchar_t* prefix, const wchar_t* extension) {
					return keepTemp(tempDir / std::format(L"{}_{}{}", prefix,
						tempFileCounter.fetch_add(1), extension));
				};
				// Each stem carries its own offset, gain and onset correction, so the
				// job-level values stay at their defaults and the per-stem ones are
				// reported individually below.
				floats = build_stem_audio(ffmpegPath, templateAudio, stems, channels, samplingRate,
					templateRate, templateLoopStart, loopStart, loopEnd,
					autoOffset, loudnessMatch, maxGainDb, onsetMatch, tempFile);

				totalSamples = floats.size() / channels;
				newLoopStart = loopStart;
				newLoopEnd = (std::min)(loopEnd, totalSamples);
				if (newLoopStart >= totalSamples) {
					newLoopStart = 0;
					newLoopEnd = 0;
				}
			} else {
				// Re-derive the offset against the game's own file. See deduce_offset: the
				// recorded value was measured against a different copy of the album than the one
				// in front of us, and the envelope that produced it cannot tell one loop pass
				// from another. A failure here is never fatal -- the recorded offset still
				// works for an untrimmed library, which is the common case.
				if (autoOffset && !job.FromPreset) {
					try {
						deduced = deduce_offset(ffmpegPath, templateAudio, job.SourcePath,
							static_cast<double>(templateLoopStart) / static_cast<double>(templateRate),
							job.Offset);
						if (deduced.Deduced)
							effectiveOffset = deduced.Seconds;
					} catch (const std::exception&) {
						deduced = {};
					}
				}

					// ...then to the sample. The coarse offset is good to a few milliseconds,
				// which is not good enough for a loop point.
				offsetBeforeAlignment = effectiveOffset;
				{
					const auto tempFile = [&](const wchar_t* prefix, const wchar_t* extension) {
						auto path = tempDir / std::format(L"{}_{}{}", prefix, tempFileCounter.fetch_add(1), extension);
						return keepTemp(std::move(path));
					};
					// Around the loop start, which is the alignment that has to be exact.
					// A track with no loop is aligned a little way in, clear of any fade.
					const auto around = (std::max)(2.5,
						static_cast<double>(templateLoopStart) / static_cast<double>(templateRate));
					try {
						aligned = refine_offset_to_samples(ffmpegPath, templateAudio, job.SourcePath,
							samplingRate, around, effectiveOffset, tempFile);
						// Unbounded, as it is for a matchset. A preset's offset is a millisecond-rounded
						// record of a fit, not a ceiling on how far the truth can be from it, and holding
						// the search to 5ms of it left 134 targets unaligned, two of them measurably worse
						// than the build that refined them freely. The search has its own guards: a 60ms
						// window and a correlation floor.
						if (aligned.Refined)
							effectiveOffset = aligned.Seconds;
					} catch (const std::exception&) {
						aligned = {};
					}
				}

				const auto rawPath = tempDir / std::format(L"scdtool_apply_{}.f32", tempFileCounter.fetch_add(1));
				keepTemp(rawPath);
				floats = decode_source_to_floats(ffmpegPath, job.SourcePath, channels, samplingRate, rawPath, job.Filter);

				// Rebase the source onto the game's timeline before anything else.
				//
				// A match at offset o means game time t holds the source's content at t - o: the
				// OST track carries an intro the game file does not, or lacks one it has. The
				// loop points below are expressed in the game's timeline, so without this the
				// loop lands wherever the OST's own lead-in happens to put it -- off by the
				// offset, typically 0.5-2s, which is enough to move the loop off its phrase
				// boundary and produce an audible seam.
				if (const auto offsetSamples = std::llround(effectiveOffset * static_cast<double>(samplingRate))) {
					if (offsetSamples > 0) {
						// The game file starts before the OST track does; the missing lead-in is
						// unrecoverable, so pad with silence rather than shifting the music.
						const auto pad = static_cast<size_t>(offsetSamples) * channels;
						floats.insert(floats.begin(), pad, 0.f);
						paddingAdded = pad / channels;
					} else {
						// The OST track starts before the game file does; drop its extra lead-in.
						const auto drop = (std::min)(static_cast<size_t>(-offsetSamples), floats.size() / channels);
						floats.erase(floats.begin(), floats.begin() + static_cast<ptrdiff_t>(drop * channels));
						trimmedAway = drop;
					}
				}

				// The loop points are sample indices, so they survive an unchanged sample rate.
				// Clamp rather than emit a loop past the end of the new audio.
				totalSamples = floats.size() / channels;
				newLoopStart = loopStart;
				newLoopEnd = (std::min)(loopEnd, totalSamples);
				if (newLoopStart >= totalSamples) {
					newLoopStart = 0;
					newLoopEnd = 0;
				}

				// Match the replacement's level to the file it replaces, measured over the same
				// musical span on both sides. Without this the swapped track sits at the OST
				// master's level, which is usually hotter than the game's own mix and would
				// stand out against every other track in game.
				const auto presetSetsGain = job.Filter.find(L"volume=") != std::wstring::npos;
				if (loudnessMatch && !presetSetsGain && newLoopEnd > newLoopStart) {
					const auto spanSeconds = static_cast<double>(newLoopEnd - newLoopStart) / static_cast<double>(samplingRate);
					const auto templateStartSeconds = static_cast<double>(templateLoopStart) / static_cast<double>(templateRate);

					// The source still has to be measured at the position the rebased output
					// took its samples from, which is offset by the match offset.
					const auto sourceStartSeconds = static_cast<double>(newLoopStart) / static_cast<double>(samplingRate) - effectiveOffset;

					try {
						const auto templateLufs = measure_loudness(ffmpegPath, templateAudio, templateStartSeconds, spanSeconds);
						const auto sourceLufs = measure_loudness(ffmpegPath, job.SourcePath, sourceStartSeconds, spanSeconds);
						gainDb = std::clamp(templateLufs - sourceLufs, -maxGainDb, maxGainDb);

						const auto requestedDb = gainDb;
						const auto gain = std::pow(10., gainDb / 20.);
						for (auto& v : floats)
							v = static_cast<float>(v * gain);

						// Applying the gain must not clip; if it would, back off uniformly rather
						// than letting the encoder fold peaks over.
						if (const auto peak = floats.empty() ? 0.f : *std::ranges::max_element(floats, [](float a, float b) { return std::abs(a) < std::abs(b); });
							std::abs(peak) > 1.f) {
							const auto scale = 1.f / std::abs(peak);
							for (auto& v : floats)
								v = v * scale;
							gainDb += 20. * std::log10(static_cast<double>(scale));
							gainLimited = true;
							requestedGainDb = requestedDb;
						}
					} catch (const std::exception&) {
						// A missing or unreadable measurement must not lose the whole file; the
						// replacement is still correct, just not level-matched.
						gainDb = 0.;
						gainLimited = false;
					}
				}

				// Reproduce whatever onset treatment the game's own file has (a fade-in, a
				// held silence) that the OST recording does not, per apply_onset_correction.
				const auto presetSetsOnset = job.Filter.find(L"adelay=") != std::wstring::npos
					|| job.Filter.find(L"afade=t=in") != std::wstring::npos;
				if (onsetMatch && !presetSetsOnset) {
					constexpr double OnsetWindowSeconds = 3.0;  // longest observed real case was ~1.3s; ample margin
					const auto onsetRawPath = tempDir / std::format(L"scdtool_apply_onset_{}.f32", tempFileCounter.fetch_add(1));
					keepTemp(onsetRawPath);
					try {
						const auto templateOnset = decode_onset_to_floats(ffmpegPath, templateAudio, channels, samplingRate, OnsetWindowSeconds, onsetRawPath);
						if (!templateOnset.empty())
							apply_onset_correction(floats, templateOnset, channels, samplingRate, onsetDb, onsetSeconds);
					} catch (const std::exception&) {
						// Same principle as the loudness-match catch above: a failed onset
						// check must not lose the whole file.
						onsetDb = 0.;
						onsetSeconds = 0.;
					}
				}

			}

			// The encoder's channel i is a *Vorbis* channel, and for 6 channels Vorbis's
			// own order (FL, FC, FR, BL, BR, LFE) is not the order a decoder hands back
			// (FL, FR, FC, LFE, BL, BR). Everything above works in the decoded order,
			// because that is what `pan=mono|c0=cN` and the matcher's channel pairing both
			// see, so the buffer has to be permuted back on the way into the encoder or the
			// stems land in each other's channels. Measured: without this, a stem left
			// untouched read back at -0.001 correlation against the game's own file.
			//
			// The permutation is exactly the `sequentialToFfmpegChannelIndexMap` the
			// hand-written presets carry -- [0, 2, 1, 4, 5, 3] -- which is what that field
			// has always meant. 1, 2 and 4 channels need no permutation (Vorbis's mono,
			// stereo and quad orders are the decoder's), which is why this never surfaced
			// while `apply` refused anything above stereo.
			//
			// Only for a Vorbis payload. WAV and FLAC both order their channels the way the
			// decoder hands them back -- FL, FR, FC, LFE, BL, BR -- which is the order
			// everything above already works in, so permuting for them would be the bug this
			// permutation exists to fix.
			if (channels == 6 && encodesVorbis) {
				constexpr size_t VorbisToDecodedChannel[6] = {0, 2, 1, 4, 5, 3};
				std::vector<float> reordered(floats.size());
				for (size_t i = 0; i < totalSamples; ++i)
					for (size_t v = 0; v < 6; ++v)
						reordered[i * 6 + v] = floats[i * 6 + VorbisToDecodedChannel[v]];
				floats = std::move(reordered);
			}

			// Carry the entry's MARK chunk across. It holds musical cue points -- one
			// Orchestrion roll has 41 of them, one every four seconds -- and since the
			// replacement is laid on the game's own timeline the positions still mean what
			// they meant; only the sample rate has to be followed. 17 of the game's music
			// entries carry one and every replacement built before this dropped it, because
			// a freshly encoded entry has no aux chunks and the writer clears the flag to
			// match.
			std::vector<uint32_t> markIndices;
			for (const auto& aux : templateItem.AuxChunks) {
				if (std::memcmp(aux->Name, xivres::sound::sound_entry_aux_chunk::Name_Mark, sizeof aux->Name) != 0)
					continue;
				const auto& mark = aux->Data.Mark;
				for (size_t i = 0, count = *mark.Count; i < count; i++) {
					const auto scaled = static_cast<uint64_t>(*mark.SampleBlockIndices[i])
						* samplingRate / templateRate;
					// A mark past the end of the new audio would point outside the stream; the
					// recording being shorter than the game's own file is common enough that
					// this has to drop rather than clamp, which would pile them on the last frame.
					if (scaled < totalSamples)
						markIndices.push_back(static_cast<uint32_t>(scaled));
				}
			}

			std::string encodeReport;
			xivres::sound::writer::sound_item newEntry;
			switch (audioFormat.Codec) {
				case audio_format::codec::LosslessVorbis:
					newEntry = make_lossless_ogg_entry(quantise_pcm16(floats, channels, newLoopEnd),
						channels, samplingRate, newLoopStart, newLoopEnd, markIndices, encodeReport);
					break;

				case audio_format::codec::Flac:
					newEntry = substitute_codec::make_flac_entry(quantise_pcm16(floats, channels, newLoopEnd),
						channels, samplingRate, newLoopStart, newLoopEnd, audioFormat.FlacLevel, encodeReport);
					break;

				case audio_format::codec::Pcm:
					newEntry = substitute_codec::make_pcm_entry(quantise_pcm16(floats, channels, newLoopEnd),
						channels, samplingRate, newLoopStart, newLoopEnd);
					break;

				default:
					newEntry = xivres::sound::writer::sound_item::make_from_ogg_encode(
						channels,
						samplingRate,
						newLoopStart,
						newLoopEnd,
						xivres::memory_stream(xivres::util::span_cast<const uint8_t>(floats)).as_linear_reader<uint8_t>(),
						{},
						markIndices,
						audioFormat.OggQuality);
					break;
			}
			// The two Vorbis paths attach the marks themselves, on their way through the
			// encoder; the substituted-codec builders take no part in it, because a MARK chunk
			// is a property of the entry rather than of the stream inside it.
			if (!markIndices.empty() && !encodesVorbis)
				newEntry.set_mark_chunks(static_cast<uint32_t>(newLoopStart),
					static_cast<uint32_t>(newLoopEnd), markIndices);

			auto newScd = xivres::sound::writer();
			newScd.set_table_1(templateScd.read_table_1());
			newScd.set_table_2(templateScd.read_table_2());
			newScd.set_table_4(templateScd.read_table_4());
			newScd.set_table_5(templateScd.read_table_5());
			for (size_t i = 0; i < templateScd.sound_item_count(); i++) {
				if (i == entryIndex)
					newScd.set_sound_item(i, newEntry);
				else
					newScd.set_sound_item(i, xivres::sound::writer::sound_item::make_from_reader_sound_item(templateScd.read_sound_item(i)));
			}

			const auto result = newScd.export_to_bytes();
			const auto outputPath = outputDir / argactions::path(job.TargetPath);
			std::filesystem::create_directories(outputPath.parent_path());
			{
				std::ofstream f(outputPath, std::ios::binary);
				if (!f)
					throw std::runtime_error(std::format("Could not create {}", u8(outputPath)));
				f.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size()));
				if (!f)
					throw std::runtime_error(std::format("Could not write {}", u8(outputPath)));
			}

			// The game's own file, byte for byte, beside the replacement -- so any later
			// comparison reads the two from one directory instead of having to reach back
			// into the installation for the other half of every pair.
			if (emitOriginal) {
				auto origPath = outputPath;
				origPath.replace_extension(L".orig.scd");
				std::vector<uint8_t> bytes(static_cast<size_t>(templateStream->size()));
				[[maybe_unused]] const auto read = templateStream->read(0, bytes.data(), static_cast<std::streamsize>(bytes.size()));
				std::ofstream f(origPath, std::ios::binary);
				if (!f)
					throw std::runtime_error(std::format("Could not create {}", u8(origPath)));
				f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				if (!f)
					throw std::runtime_error(std::format("Could not write {}", u8(origPath)));
			}

			// Read the file back and confirm the audio is there and the loop survived.
			// Encoding is the one step that can silently produce a file the game will not
			// loop correctly, and it is far cheaper to catch here than in-game.
			if (verify) {
				// Reopening a file this thread has just closed can still lose a race with
				// whatever scans new files on Windows, which surfaces as a sharing violation
				// on a file that is perfectly good. Retry briefly rather than failing the run
				// over it: measured 8 of 747 on one pass, every one of them fine on reread.
				std::shared_ptr<xivres::file_stream> checkStream;
				for (int attempt = 0; ; ++attempt) {
					try {
						checkStream = std::make_shared<xivres::file_stream>(outputPath);
						break;
					} catch (const std::exception&) {
						if (attempt >= 5)
							throw;
						std::this_thread::sleep_for(std::chrono::milliseconds(50 << attempt));
					}
				}
				const xivres::sound::reader checkScd(checkStream);
				if (checkScd.sound_item_count() <= entryIndex)
					throw std::runtime_error(std::format("verification failed for {}: wrote {} sound entries", u8(outputPath), checkScd.sound_item_count()));
				const auto checkItem = checkScd.read_sound_item(entryIndex);

				// A substituted-codec entry has no Vorbis stream to decode, so it is checked
				// against what its payload says about itself: the header region the hook reads
				// has to describe the audio that follows it, and the entry's loop fields --
				// byte offsets, here -- have to address that audio rather than run past it.
				if (!encodesVorbis) {
					const auto payload = substitute_codec::inspect(checkItem);
					const auto expectedSamples = newLoopEnd > 0 && newLoopEnd < totalSamples ? newLoopEnd : totalSamples;
					if (payload.Kind == substitute_codec::payload::Vorbis)
						throw std::runtime_error(std::format("verification failed for {}: the entry did not read back as a substituted codec", u8(outputPath)));
					if (payload.Channels != channels || payload.SamplingRate != samplingRate)
						throw std::runtime_error(std::format("verification failed for {}: payload says {} ch at {} Hz, expected {} at {}",
							u8(outputPath), payload.Channels, payload.SamplingRate, channels, samplingRate));
					if (payload.TotalFrames != expectedSamples)
						throw std::runtime_error(std::format("verification failed for {}: payload holds {} samples, expected {}",
							u8(outputPath), payload.TotalFrames, expectedSamples));
					const auto loopEndOffset = static_cast<size_t>(checkItem.Header->LoopEndOffset);
					if (loopEndOffset > checkItem.Data.size() || checkItem.Header->LoopStartOffset > loopEndOffset)
						throw std::runtime_error(std::format("verification failed for {}: loop fields {}-{} do not address the {}-byte payload",
							u8(outputPath), static_cast<size_t>(checkItem.Header->LoopStartOffset), loopEndOffset, checkItem.Data.size()));
					// Raw PCM is the one payload whose byte offsets map back to samples
					// without decoding anything, so the loop can be checked exactly.
					if (payload.Kind == substitute_codec::payload::Wave) {
						const auto frameBytes = channels * sizeof(int16_t);
						const auto readLoopStart = static_cast<size_t>(checkItem.Header->LoopStartOffset) / frameBytes;
						const auto readLoopEnd = loopEndOffset / frameBytes;
						if (readLoopStart != newLoopStart || readLoopEnd != (newLoopEnd ? expectedSamples : 0))
							throw std::runtime_error(std::format("verification failed for {}: loop read back as {}-{}, expected {}-{}",
								u8(outputPath), readLoopStart, readLoopEnd, newLoopStart, newLoopEnd));
					}
				} else {
					const auto checkInfo = checkItem.get_ogg_decoded();
					const auto checkSamples = checkInfo.Data.size() / sizeof(float) / (checkInfo.Channels ? checkInfo.Channels : 1);
					// The encoder shifts loop points onto an Ogg page boundary, by the stream's
					// priming offset, so an exact match is not expected -- only a small one.
					// A larger difference would mean the loop actually moved.
					constexpr size_t LoopToleranceSamples = 8192;
					const auto startDelta = checkInfo.LoopStartBlockIndex > newLoopStart ? checkInfo.LoopStartBlockIndex - newLoopStart : newLoopStart - checkInfo.LoopStartBlockIndex;
					const auto endDelta = checkInfo.LoopEndBlockIndex > newLoopEnd ? checkInfo.LoopEndBlockIndex - newLoopEnd : newLoopEnd - checkInfo.LoopEndBlockIndex;
					if (startDelta > LoopToleranceSamples || endDelta > LoopToleranceSamples)
						throw std::runtime_error(std::format("verification failed for {}: loop read back as {}-{}, expected {}-{}",
							u8(outputPath), checkInfo.LoopStartBlockIndex, checkInfo.LoopEndBlockIndex, newLoopStart, newLoopEnd));
					// A looping entry is encoded only up to its loop end: everything past that
					// point is unreachable, because the game jumps back to the loop start. So
					// the expected length is the loop end, not the whole source track.
					const auto expectedSamples = newLoopEnd > 0 && newLoopEnd < totalSamples ? newLoopEnd : totalSamples;
					const auto samplesDelta = checkSamples > expectedSamples ? checkSamples - expectedSamples : expectedSamples - checkSamples;
					if (samplesDelta > LoopToleranceSamples)
						throw std::runtime_error(std::format("verification failed for {}: {} samples read back, expected about {}",
							u8(outputPath), checkSamples, expectedSamples));
				}
			}

			{
				const auto lock = std::scoped_lock(logMutex);
				++writtenCount;
				if (!stems.empty()) {
					std::cerr << std::format("  {} <- {} stem(s), loop {}-{}", job.TargetPath, stems.size(), newLoopStart, newLoopEnd);
					for (const auto& stem : stems) {
						if (!stem.Matched) {
							std::cerr << std::format("\n      channels {},{} kept from the game's own file (best candidate {} scored {:.3f})",
								stem.LeftChannel, stem.RightChannel, u8(stem.SourcePath), stem.Score);
							continue;
						}
						std::cerr << std::format("\n      channels {},{} <- {} (score {:.3f}, offset {:+.3f}s{}, gain {:+.1f} dB{}{})",
							stem.LeftChannel, stem.RightChannel, u8(stem.SourcePath), stem.Score, stem.EffectiveOffset,
							stem.Deduced ? std::format(" [deduced, was {:+.3f}s]", stem.Offset) : "",
							stem.GainDb,
							stem.OnsetSeconds > 0. ? std::format(", onset corrected {:.1f} dB over {:.2f}s", stem.OnsetDb, stem.OnsetSeconds) : "",
							// A stem that runs out before the loop end leaves that engine
							// state silent for the rest of the track, which is audible in a
							// way a shortfall past the loop end is not.
							stem.ShortfallSeconds > 0.5 ? std::format(", SHORT by {:.1f}s", stem.ShortfallSeconds) : "");
					}
					if (!encodeReport.empty())
						std::cerr << std::format("\n      {}", encodeReport);
					std::cerr << '\n';
					return;
				}
				// A segmented job has no single source, so name what it was actually built from;
				// it was logging an empty path, which left the shipped listing unable to say what
				// 30-odd of its entries came from.
				std::string built;
				for (const auto& segment : job.Segments) {
					for (const auto& [name, source] : segment.Sources) {
						const auto file = u8(source.Path.filename());
						if (built.find(file) == std::string::npos)
							built += (built.empty() ? "" : "+") + file;
					}
				}
				std::cerr << std::format("  {} <- {} (score {:.3f}, offset {:+.3f}s{}{}, trim {} pad {} samples, loop {}-{}, gain {:+.1f} dB{}{})",
					job.TargetPath, job.Segments.empty() ? u8(job.SourcePath) : built, job.Score, effectiveOffset,
					deduced.Deduced && std::abs(effectiveOffset - job.Offset) > 0.05
						? std::format(" [deduced, was {:+.3f}s, intro {:.3f} over {} candidates]",
							job.Offset, deduced.Score, deduced.Candidates)
						: "",
					aligned.Correlation > -2.
						? std::format(" [sample-aligned {:+.0f}, r {:.3f}]",
							(aligned.Seconds - offsetBeforeAlignment) * static_cast<double>(samplingRate), aligned.Correlation)
						: " [not sample-aligned]",
					trimmedAway, paddingAdded,
					newLoopStart, newLoopEnd, gainDb,
					gainLimited ? std::format(", peak-limited from {:+.1f} dB", requestedGainDb) : "",
					onsetSeconds > 0. ? std::format(", onset corrected {:.1f} dB over {:.2f}s", onsetDb, onsetSeconds) : "")
					// What the encode itself cost, for the formats that have something to say
					// about it -- the libvorbis path's quality number is already in the command
					// line, but how big a lossless or FLAC entry came out is not.
					<< (encodeReport.empty() ? std::string() : std::format("\n      {}", encodeReport)) << '\n';
			}
			} catch (const std::exception& e) {
				const auto lock = std::scoped_lock(logMutex);
				++failedCount;
				std::cerr << std::format("  FAILED {}: {}", job.TargetPath, e.what()) << '\n';
			}
		}, MaxDecodeThreads);

		cleanupTempFiles();
		std::cerr << std::format("Done. Wrote {} file(s) under {}{}.", writtenCount.load(), u8(outputDir),
			failedCount ? std::format("; {} entr(ies) could not be built", failedCount.load()) : "") << '\n';
		return 0;

	} catch (const std::exception& e) {
		cleanupTempFiles();
		std::cerr
			<< "Error processing data.\n"
			<< e.what() << '\n';
		return -1;
	}
}
