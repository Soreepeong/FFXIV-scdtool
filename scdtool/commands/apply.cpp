#include "pch.h"
#include "apply.h"

#include "utils/argactions.h"
#include "utils/audio_match.h"
#include "utils/filter_graph.h"
#include "utils/hca_payload.h"
#include "utils/lossless_vorbis.h"
#include "utils/misc.h"
#include "utils/preset_model.h"
#include "utils/substitute_codec.h"
#include "utils/verify_audio.h"
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

	// How loud counts as "the music has started" when a source with no stated offset is
	// aligned onto the target by onset. The old importer's own default, and the value the
	// 21 presets that override it were correcting: a quiet opening is already past 0.1
	// before it is audible, and a noisy one reaches it during the room tone.
	constexpr double DefaultOnsetThreshold = 0.1;

	struct apply_job {
		std::string TargetPath;   // path inside the game
		std::filesystem::path SourcePath;
		double Score = 0.;
		double Offset = 0.;       // seconds the source is shifted relative to the game's timeline
		std::vector<apply_stem> Stems;  // empty unless the entry is engine-switched stems
		std::vector<apply_segment> Segments;  // empty unless the entry needs more than one span
		std::wstring Filter;      // the preset's filter chain for this source, if it gave one
		// A preset is a record of decisions already taken: an offset it states was fitted
		// against this very file, so it is not re-derived (see OffsetStated for the offset
		// it does not state). A gain or a lead-in silence, though, is only written down when
		// it was worth writing -- the generator dropped any gain under a decibel -- so the
		// absence of one is not an instruction to leave the level alone. Measuring it where
		// the preset is silent is what the matchset build did, and without it
		// BGM_Town_Uru_Day came out 0.6 dB off a game file it used to match exactly. Only where the preset *does* say would measuring again apply it twice.
		bool FromPreset = false;
		// Whether that preset fixed the offset. `FromPreset` alone is not the question the
		// offset deduction has to answer: an item that states one has had it fitted against
		// this very file and must be left alone, but most of the hand-written items state
		// none at all, and their implicit zero is an absence of a decision rather than a
		// decision to start at zero. Re-deriving where nothing was fixed is the only way
		// those entries get an offset at all. See apply_segment_source::FixesAlignment.
		bool OffsetStated = false;
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
		// The span is cut by atrim inside the chain: sample-accurate, which matters because
		// the two sides of the comparison must cover exactly the same musical span for the
		// difference to mean anything. Not -ss/-t after -i: those only trim what is muxed,
		// so ebur128 would integrate everything from the top of the file to the span's end
		// -- measured on BGM_EX3_Ban_14 at 403.26s for 10s, -11.8 LUFS against -15.2.
		// Not -ss before -i either: an input seek into Ogg lands up to ~20 ms off.
		//
		// `preFilter` runs first: a preset's source filter works in recording time (an
		// afade's st=, an adelay), so it has to see the recording from its top, as it does
		// when the audio is decoded for real.
		std::wstring chain;
		if (!preFilter.empty())
			chain += preFilter + L",";
		if (startSeconds > 0 || durationSeconds > 0) {
			chain += L"atrim";
			if (startSeconds > 0)
				chain += std::format(L"=start={:.6f}", startSeconds);
			if (durationSeconds > 0)
				chain += std::format(L"{}duration={:.6f}", startSeconds > 0 ? L":" : L"=", durationSeconds);
			chain += L",";
		}
		chain += L"ebur128";
		std::vector<std::wstring> args{L"-v", L"info", L"-nostdin"};
		// Stop decoding at the span's end; an input -to seeks nothing, so it cannot land off.
		// Not under a filter, which may move audio later than where the decode would stop.
		if (durationSeconds > 0 && preFilter.empty())
			args.insert(args.end(), {L"-to", std::format(L"{:.6f}", startSeconds + durationSeconds + 1.)});
		args.insert(args.end(), {L"-i", file.wstring(), L"-af", chain, L"-f", L"null", L"-"});

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
	// Runs a source's filter graph and writes the result where the rest of the build can read
	// it as an ordinary file. Rendered once per graph and kept, not re-run per channel: the
	// graphs that need this mix several inputs, so decoding one twice would cost the whole
	// mix twice, and a `volume=...:eval=frame` ramp has to give both channels the same curve.
	//
	// Written as 32-bit float wav rather than a raw stream so the sample rate and channel
	// count survive -- everything downstream opens this by path and asks ffmpeg what it is.
	std::filesystem::path render_source_graph(
		const std::filesystem::path& ffmpeg,
		const apply_source_graph& graph,
		const std::filesystem::path& templateAudio,
		const std::filesystem::path& outPath) {

		std::error_code ec;
		std::filesystem::remove(outPath, ec);

		std::vector<std::wstring> args{L"-v", L"error"};
		for (const auto& input : graph.Inputs) {
			// The game's own entry, staged by the caller. Resolved here rather than earlier
			// because there is one staged file per target and one graph shared by all of an
			// item's targets, so the substitution belongs to the render, not to the preset.
			const auto& file = input.IsTarget ? templateAudio : input.Path;
			if (file.empty())
				throw std::runtime_error("a filter graph input was never resolved to a file");
			args.emplace_back(L"-i");
			args.emplace_back(file.wstring());
		}
		args.emplace_back(L"-filter_complex");
		args.emplace_back(xivres::util::unicode::convert<std::wstring>(graph.Description));
		args.emplace_back(L"-map");
		args.emplace_back(std::format(L"[{}]", xivres::util::unicode::convert<std::wstring>(graph.OutLabel)));
		args.insert(args.end(), {L"-c:a", L"pcm_f32le", L"-y", outPath.wstring()});
		run_process_capture_stdout(ffmpeg, args);

		if (!std::filesystem::exists(outPath))
			throw std::runtime_error("the filter graph produced no audio");
		return outPath;
	}

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
		constexpr double MinOverlapFloorSeconds = 15.; // never a fraction of the target: a
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
		// A target shorter than the floor cannot overlap by it however it is aligned, and
		// asking for the impossible yields no candidates at all rather than a worse one.
		const auto minOverlapSeconds = (std::min)(MinOverlapFloorSeconds,
			static_cast<double>(envTemplate.size()) / EnvelopeRateHz);
		auto candidates = envelope_offset_candidates(
			envTemplate, envSource, EnvelopeRateHz, MaxOffsetSeconds, minOverlapSeconds);
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

	// Vorbis's own six-channel order is FL, FC, FR, BL, BR, LFE; a decoder hands back
	// FL, FR, FC, LFE, BL, BR. So sequential channel i is decoded channel this[i], which is
	// what every hand-written preset carries as `sequentialToFfmpegChannelIndexMap`. One,
	// two and four channels are the same order either way.
	constexpr size_t VorbisToDecodedChannel[6] = {0, 2, 1, 4, 5, 3};

	// The first sample at or above `threshold`, searching from `from`.
	//
	// The raw signed value, not its magnitude, which is what the old importer compared and
	// so is what the thresholds in the hand-written presets were tuned against: 0.4 on the
	// Crystal Tower stems, 0.03 on two quiet field themes, 0.01 on Skylords. Comparing
	// magnitudes instead would fire up to half a cycle earlier and would make every one of
	// those numbers mean something slightly different from what its author measured.
	std::optional<size_t> first_sample_above(const std::vector<float>& samples, size_t from, float threshold) {
		for (size_t i = from; i < samples.size(); ++i)
			if (samples[i] >= threshold)
				return i;
		return std::nullopt;
	}

	// How long the file runs, in seconds, or 0 if ffprobe will not say. Wanted for one
	// reason: a target with no loop has no loop end to truncate the render at, so without
	// this nothing bounds it and the replacement runs to the end of the recording.
	double probe_duration(const std::filesystem::path& ffprobe, const std::filesystem::path& file) {
		const auto bytes = run_process_capture_stdout(ffprobe, {
			L"-v", L"error",
			L"-select_streams", L"a:0",
			L"-show_entries", L"format=duration",
			L"-of", L"default=noprint_wrappers=1:nokey=1",
			file.wstring(),
		});
		try {
			return std::stod(std::string(bytes.begin(), bytes.end()));
		} catch (const std::exception&) {
			return 0.;
		}
	}

	// Lay a preset's segments onto the target's timeline and sum them into one buffer.
	//
	// Every segment is placed at the cumulative sum of the lengths before it, and the
	// overlaps are complementary ramps -- equal-power by default, linear where the preset
	// says both sides are the same signal -- so a crossfade holds its level rather than
	// dipping through it. That "keep playing and fade under" shape is the point
	// of the whole path: the 20 loop-out entries here are targets whose game file outlasts
	// its recording and ends by re-entering the same piece earlier on, and a hard cut at
	// the seam measures 0.939 against the game's own file where the crossfade measures
	// 0.993. The 7 credits rolls are the same machinery with a different source per segment.
	//
	// Level matching is per recording rather than once for the whole file: a medley
	// stitched from eight album tracks has eight different masters in it, and one gain for
	// the lot leaves most of them wrong. See the measurement below for why it is not per
	// segment either.
	std::vector<float> build_segment_audio(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& ffprobe,
		const std::filesystem::path& templateAudio,
		const std::vector<apply_segment>& segments,
		size_t channels,
		size_t samplingRate,
		// Where the target stops being heard: its loop end, or its length when it does not loop.
		size_t targetEnd,
		bool loudnessMatch,
		double maxGainDb,
		const std::function<std::filesystem::path(const wchar_t*, const wchar_t*)>& tempFile,
		// Where the onset alignment put each source it moved, for the caller to report.
		std::vector<std::pair<std::string, double>>* alignments = nullptr,
		// The level each recording was given, in dB, and the whole-file back-off a peak over
		// full scale forced (as a final entry named "peak"), for the caller to report.
		std::vector<std::pair<std::string, double>>* gains = nullptr) {

		// Which decoded channel entry i of a segment's `channels` list describes. The list
		// is in sequential order and every buffer here is in decoded order; see
		// VorbisToDecodedChannel. Identity below six channels.
		const auto seat = [channels](size_t sequential) {
			return channels == 6 && sequential < 6 ? VorbisToDecodedChannel[sequential] : sequential;
		};

		const auto rate = static_cast<double>(samplingRate);
		const auto toSamples = [rate](double seconds) {
			return static_cast<size_t>((std::max)(0LL, std::llround(seconds * rate)));
		};

		// Any source the preset builds from a graph, rendered to a file first. Keyed on the
		// graph itself, so a source named by several segments is rendered once.
		std::map<const apply_source_graph*, std::filesystem::path> rendered;
		const auto fileFor = [&](const apply_segment_source& source) {
			// The game's own entry, already staged for the loudness and onset checks.
			if (source.IsTarget)
				return templateAudio;
			if (!source.Graph)
				return source.Path;
			const auto key = source.Graph.get();
			if (const auto it = rendered.find(key); it != rendered.end())
				return it->second;
			return rendered.emplace(key, render_source_graph(ffmpeg, *source.Graph, templateAudio,
				tempFile(L"scdtool_apply_graph", L".wav"))).first->second;
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

		// One gain per recording (file and filter), measured over the longest span any
		// segment reads from it, on both sides. Per segment was fragile: a short span is a
		// noisy reading, and one that does not hold what the game plays there -- a misplaced
		// re-entry -- asks for the full --max-gain, whose peaks then pull the whole file down
		// (BGM_ORCH_899's 3.5s re-entry: +12 dB, and the file came out 9 dB quiet). A medley
		// of different recordings still gets one gain each.
		//
		// The recording is measured through its own filter, since that is what plays, so a
		// volume= the preset states is where the match starts rather than a second gain on
		// top of it. Generated presets carry apply's own measured gain that way for importers
		// that do not level-match; measured without the filter, it was applied twice. Nor is
		// it kept as the final level: those were measured by the -ss-after--i reading, and
		// keeping them left ORCH_252 2.5 dB and ORCH_899 2.4 dB further from the game than
		// matching through them, with nothing the other way across 115 stitched targets.
		std::map<std::pair<std::filesystem::path, std::wstring>, double> gainOf;
		if (loudnessMatch) {
			struct longest_span { size_t Segment = 0; std::string Name; size_t From = 0; size_t Span = 0; };
			std::map<std::pair<std::filesystem::path, std::wstring>, longest_span> longest;
			std::map<std::filesystem::path, size_t> recordingLength;
			for (size_t i = 0; i < segments.size(); i++) {
				for (const auto& [name, source] : segments[i].Sources) {
					if (source.IsTarget)
						continue;
					// Only what this segment plays. Its source list is the item's whole list,
					// so a medley's every recording is "in" every segment: measured there,
					// ARR_FFXIV_115 took its longest span from the part of
					// BGM_System_EndCredit01 that ARR_FFXIV_003 plays, and came out -10.2 dB.
					if (std::ranges::none_of(segments[i].Channels, [&](const auto& c) { return c.first == name; }))
						continue;
					const auto file = fileFor(source);
					if (!recordingLength.contains(file))
						recordingLength.emplace(file, toSamples(probe_duration(ffprobe, file)));
					const auto from = toSamples(source.Offset);
					// As far as the segment states, the target runs and the recording lasts.
					auto span = segments[i].Length > 0. ? toSamples(segments[i].Length) : (std::numeric_limits<size_t>::max)();
					span = (std::min)(span, targetEnd > segmentStart[i] ? targetEnd - segmentStart[i] : 0);
					if (const auto length = recordingLength.at(file); length)
						span = (std::min)(span, length > from ? length - from : 0);
					auto& best = longest[{file, source.Filter}];
					if (span > best.Span)
						best = {i, name, from, span};
				}
			}
			for (const auto& [key, best] : longest) {
				// Both sides as they will be heard: of the game's file, only the channels this
				// recording feeds -- not a stem another recording or "target" supplies -- and the
				// recording mixed the way the routing below mixes it, mono fold included.
				// Measured whole, a 6-channel entry's reference was all three stems at once, and
				// a mono entry's stereo recording read 3-6 dB louder than its own fold: the 68
				// mono targets of a 175-target check sat 3.95 dB under the game (stereo: 0.29).
				const auto& seg = segments[best.Segment];
				std::map<size_t, std::vector<std::pair<size_t, double>>> feeds;  // output -> (recording channel, weight)
				if (seg.Channels.size() == channels) {
					for (size_t ci = 0; ci < channels; ci++)
						if (seg.Channels[ci].first == best.Name)
							feeds[seat(ci)].emplace_back(seg.Channels[ci].second, 1.);
				} else if (channels == 1) {
					for (const auto& [name, channelIndex] : seg.Channels)
						if (name == best.Name)
							feeds[0].emplace_back(channelIndex, 1. / static_cast<double>(seg.Channels.size()));
				}
				const auto identity = channels == 2 && feeds.size() == 2
					&& feeds[0] == std::vector<std::pair<size_t, double>>{{0, 1.}}
					&& feeds[1] == std::vector<std::pair<size_t, double>>{{1, 1.}};
				std::wstring templatePan, sourcePan;
				if (!identity && !feeds.empty()) {
					const auto layout = feeds.size() == 1 ? std::wstring(L"mono")
						: feeds.size() == 2 ? std::wstring(L"stereo") : std::format(L"{}c", feeds.size());
					templatePan = sourcePan = L"pan=" + layout;
					size_t k = 0;
					for (const auto& [output, inputs] : feeds) {
						templatePan += std::format(L"|c{}=c{}", k, output);
						sourcePan += std::format(L"|c{}=", k);
						for (size_t j = 0; j < inputs.size(); j++)
							sourcePan += std::format(L"{}{:.6f}*c{}", j ? L"+" : L"", inputs[j].second, inputs[j].first);
						k++;
					}
				}
				// A mono entry is compared against the game's file folded to mono: an
				// Orchestrion roll is built mono even where the game ships it in stereo,
				// because the game folds it on playback.
				if (channels == 1)
					templatePan = L"aformat=channel_layouts=mono";
				const auto sourceChain = sourcePan.empty() ? key.second
					: key.second.empty() ? sourcePan : key.second + L"," + sourcePan;
				try {
					const auto spanSeconds = static_cast<double>(best.Span) / rate;
					const auto templateLufs = measure_loudness(ffmpeg, templateAudio,
						static_cast<double>(segmentStart[best.Segment]) / rate, spanSeconds, templatePan);
					const auto sourceLufs = measure_loudness(ffmpeg, key.first,
						static_cast<double>(best.From) / rate, spanSeconds, sourceChain);
					const auto db = std::clamp(templateLufs - sourceLufs, -maxGainDb, maxGainDb);
					gainOf.emplace(key, std::pow(10., db / 20.));
					if (gains)
						gains->emplace_back(best.Name, db);
				} catch (const std::exception&) {
					// Same rule as the single-source path: an unreadable measurement costs
					// the level match, never the file.
				}
			}
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
					routing[seat(ch)].emplace_back(segment.Channels[ch], 1.f);
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
				// `target` is the game's own entry, whose channels the preset names in the
				// same sequential order; an OST track's two channels are its own and need no
				// translation. A mono entry reads every `target` channel as the game's file
				// folded to mono, so the routing's average of them is that fold: a preset
				// naming `target` channels 0 and 1 of a mono file read channel 1 as silence
				// and came out 6 dB quiet there (BGM_ORCH_489).
				const auto targetFold = source->second.IsTarget && channels == 1;
				const auto& filter = source->second.Filter;
				decoded.emplace(key, decode_channel_to_floats(ffmpeg, fileFor(source->second),
					targetFold ? 0 : source->second.IsTarget ? seat(channelIndex) : channelIndex,
					samplingRate, tempFile(L"scdtool_apply_seg", L".f32"),
					!targetFold ? filter : filter.empty() ? std::wstring(L"aformat=channel_layouts=mono")
						: filter + L",aformat=channel_layouts=mono"));
			}

			// Where each source is read from, in its own samples. Normally its stated
			// offset; where it states none, the offset that lands its first audible sample
			// on the first audible sample of the target channel it feeds.
			//
			// Signed, because the answer can be before the recording's start -- the game's
			// file may open ahead of the release -- and that is leading silence rather than
			// an error. Measured once per source and applied to all of its channels, from
			// the first output channel that names it, which is what the importer did.
			std::map<std::string, ptrdiff_t> start;
			for (const auto& [name, source] : segment.Sources)
				start.emplace(name, static_cast<ptrdiff_t>(toSamples(source.Offset)));

			std::map<size_t, std::vector<float>> targetChannel;  // decoded on demand, cached
			std::set<std::string> aligned;
			for (size_t ci = 0; ci < segment.Channels.size(); ++ci) {
				const auto& [name, channelIndex] = segment.Channels[ci];
				const auto source = segment.Sources.find(name);
				if (source == segment.Sources.end() || source->second.IsTarget || source->second.FixesAlignment())
					continue;
				if (aligned.contains(name))
					continue;  // an earlier output channel already placed this source
				aligned.insert(name);

				// The target channel this one feeds. A mono entry described by a stereo
				// preset has fewer channels than the segment lists, and both of its listed
				// channels fold into the one it has.
				const auto tc = (std::min)(seat(ci), channels - 1);
				try {
					if (!targetChannel.contains(tc))
						targetChannel.emplace(tc, decode_channel_to_floats(ffmpeg, templateAudio, tc,
							samplingRate, tempFile(L"scdtool_apply_tplch", L".f32")));

					const auto threshold = [&](const std::string& who) {
						const auto it = segment.Thresholds.find(who);
						return static_cast<float>(it == segment.Thresholds.end() ? DefaultOnsetThreshold : it->second);
					};
					// The target is searched from this segment's own start, not from the
					// top of the file: a later segment is matched against the stretch of
					// the target it actually covers.
					const auto before = start.at(name);
					const auto from = (std::max)(ptrdiff_t{0}, before);
					const auto targetFirst = first_sample_above(targetChannel.at(tc), segmentStart[i], threshold("target"));
					const auto sourceFirst = first_sample_above(decoded.at(segment.Channels[ci]),
						static_cast<size_t>(from), threshold(name));
					if (targetFirst && sourceFirst) {
						// Each onset counts from where its own reading begins, so the shift
						// is the difference between two like quantities rather than between
						// two unrelated origins. Both starts are zero for an item's first
						// segment, which is why taking them as absolute indices looked right
						// until a second segment moved one of them.
						const auto targetRelative = static_cast<ptrdiff_t>(*targetFirst) - static_cast<ptrdiff_t>(segmentStart[i]);
						const auto sourceRelative = static_cast<ptrdiff_t>(*sourceFirst) - from;
						start.at(name) = before + sourceRelative - targetRelative;
						if (alignments && start.at(name) != before)
							alignments->emplace_back(name, static_cast<double>(start.at(name)) / rate);
					}
				} catch (const std::exception&) {
					// The same rule the loudness and onset checks follow: a measurement that
					// cannot be made costs the alignment, never the entry.
				}
			}

			// How much of this segment its sources can actually supply, from where each is
			// read from. A recording that stops short simply ends the segment early rather
			// than reading past its end; one read from before its start contributes the
			// leading silence too.
			size_t available = (std::numeric_limits<size_t>::max)();
			for (const auto& [key, samples] : decoded) {
				const auto from = start.at(key.first);
				const auto reach = static_cast<ptrdiff_t>(samples.size()) - from;
				available = (std::min)(available, reach > 0 ? static_cast<size_t>(reach) : 0);
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
			const auto statedIn = segment.FadeInSeconds >= 0.;
			const auto statedOut = segment.FadeOutSeconds >= 0.;
			const auto fadeIn = (std::min)(toSamples(statedIn
				? segment.FadeInSeconds : segment.CrossfadeSeconds), render);
			const auto fadeOut = (std::min)(statedOut
				? toSamples(segment.FadeOutSeconds) : tail, render - fadeIn);

			// Whether each edge is half of a crossfade or a fade in its own right, which
			// decides the curve. A crossfade joins two segments carrying *different*
			// material, and two uncorrelated signals on complementary linear ramps sum to
			// 1/sqrt(2) at the midpoint -- a 3 dB sag exactly where the join is. Square-root
			// ramps sum to constant power instead. A stated fade has no second signal
			// holding the power up and stays linear.
			//
			// Measured on BGM_EX5_Raid_22's join against the game's own file: worst point
			// -5.3 dB -> -2.7, mean -2.3 -> -1.8. Moving the join does not help; four
			// positions were built and the sag moves with it.
			// Two halves of one join, so they have to agree: the fade-out belongs to the
			// crossfade the *next* segment describes, and follows that segment's choice.
			const auto powerIn = !statedIn && segment.CrossfadeSeconds > 0. && segment.CrossfadeEqualPower;
			const auto powerOut = !statedOut && tail > 0
				&& i + 1 < segments.size() && segments[i + 1].CrossfadeEqualPower;

			std::map<std::string, double> gain;
			for (const auto& [name, source] : segment.Sources) {
				const auto it = source.IsTarget ? gainOf.end() : gainOf.find({fileFor(source), source.Filter});
				gain[name] = it == gainOf.end() ? 1. : it->second;
			}
			// A gain that would clip what this segment plays is held to its own peak, as the
			// single-source and stem paths do, rather than left to the whole-file back-off
			// below -- which quietened every segment of a medley for one loud one:
			// BGM_System_EndCredit01's ARR_FFXIV_003 at +3.8 dB cost all three 2.2 dB.
			//
			// Only as far as the target runs: a last segment states no length and renders to the
			// end of its recording, and a peak past the loop end -- which nobody hears -- held
			// BGM_Event_Tanoshii1's re-entry at -5.9 dB under a filter that lifts late material.
			const auto heard = targetEnd > segmentStart[i] ? (std::min)(render, targetEnd - segmentStart[i]) : render;
			//
			// The peak of what this recording puts on each output channel, through the
			// routing -- not of its raw channels: on a mono entry the fold of a decorrelated
			// stereo recording peaks lower than either channel (BGM_ORCH_899: 2.16 against
			// 2.64, a 1.7 dB tighter hold than needed).
			for (auto& [name, g] : gain) {
				if (g <= 1.)
					continue;
				const auto from = start.at(name);
				float peak = 0.f;
				for (size_t ch = 0; ch < channels; ch++) {
					std::vector<std::pair<const std::vector<float>*, float>> inputs;
					for (const auto& [key, weight] : routing[ch])
						if (key.first == name)
							inputs.emplace_back(&decoded.at(key), weight);
					if (inputs.empty())
						continue;
					for (size_t n = 0; n < heard; n++) {
						const auto at = from + static_cast<ptrdiff_t>(n);
						float v = 0.f;
						for (const auto& [samples, weight] : inputs)
							if (at >= 0 && at < static_cast<ptrdiff_t>(samples->size()))
								v += weight * (*samples)[static_cast<size_t>(at)];
						peak = (std::max)(peak, std::abs(v));
					}
				}
				if (peak > 0.f && g * peak > 1.) {
					g = 1. / peak;
					if (gains)
						gains->emplace_back(name + " held", 20. * std::log10(g));
				}
			}

			const auto end = segmentStart[i] + render;
			if (out.size() < end * channels)
				out.resize(end * channels, 0.f);

			for (size_t ch = 0; ch < channels; ch++) {
				for (const auto& [key, weight] : routing[ch]) {
				const auto& [name, channelIndex] = key;
				const auto& samples = decoded.at(key);
				const auto from = start.at(name);
				const auto scale = weight * static_cast<float>(gain.empty() ? 1. : gain.at(name));
				for (size_t n = 0; n < render; n++) {
					// Before the recording begins, or past where it ends, is silence. Only
					// an alignment that reaches back before the source's start can produce
					// the first, and `available` already bounds the second.
					const auto at = from + static_cast<ptrdiff_t>(n);
					auto value = at >= 0 && static_cast<size_t>(at) < samples.size()
						? samples[static_cast<size_t>(at)] * scale : 0.f;
					if (n < fadeIn) {
						const auto w = static_cast<double>(n + 1) / static_cast<double>(fadeIn + 1);
						value *= static_cast<float>(powerIn ? std::sqrt(w) : w);
					} else if (fadeOut && n >= render - fadeOut) {
						const auto w = static_cast<double>(render - n) / static_cast<double>(fadeOut + 1);
						value *= static_cast<float>(powerOut ? std::sqrt(w) : w);
					}
					out[(segmentStart[i] + n) * channels + ch] += value;
				}
				}
			}
		}

		// Summed ramps cannot clip on their own, but two segments of the same loud master
		// overlapping can, and the encoder would fold the peaks over rather than refuse.
		// Judged only where the target runs: an unbounded last segment renders on to the
		// end of its recording, and a peak out there cost BGM_ORCH_899 4.1 dB everywhere.
		const auto heardEnd = (std::min)(out.size(), targetEnd < out.size() / channels ? targetEnd * channels : out.size());
		if (const auto peak = heardEnd == 0 ? 0.f : std::abs(*std::ranges::max_element(out.begin(), out.begin() + static_cast<ptrdiff_t>(heardEnd),
			[](float a, float b) { return std::abs(a) < std::abs(b); })); peak > 1.f) {
			for (auto& v : out)
				v /= peak;
			if (gains)
				gains->emplace_back("peak", -20. * std::log10(static_cast<double>(peak)));
		}
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

	// Turn one MusicImportConfig target into jobs -- one per game path it lists, since a
	// target may name several .scd files that carry the same music. The reading itself is
	// preset_model's, which `verify` shares, so the two cannot disagree about what a preset
	// builds; what is decided here is only which of apply's two build paths gets it.
	void collect_config_target(
		const std::filesystem::path& ostDir,
		const nlohmann::json& config,
		const nlohmann::json& sourceSpec,
		const nlohmann::json& target,
		std::vector<apply_job>& jobs,
		std::vector<std::pair<std::string, std::string>>& unresolved) {

		auto read = read_config_target(ostDir, config, sourceSpec, target, unresolved);
		if (!read)
			return;
		const auto& paths = read->Paths;
		const auto& segments = read->Segments;

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
			|| first.CrossfadeSeconds > 0. || first.FadeInSeconds >= 0. || first.FadeOutSeconds >= 0.
			// The fast path opens `Path`, which for a graph-built source is only its first
			// input -- so it would build one recording where the preset asked for a mix.
			|| std::ranges::any_of(first.Sources, [](const auto& kv) {
				return kv.second.Graph != nullptr || kv.second.IsTarget;
			});
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
					.OffsetStated = only.FixesAlignment(),
					.Note = read->Note,
				});
				continue;
			}
			jobs.push_back({
				.TargetPath = path,
				.Score = 1.,
				.Segments = segments,
				.FromPreset = true,
				.Note = read->Note,
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
	// Make the loop close on itself.
	//
	// The game's own file loops seamlessly because it was authored to: the sample before its
	// loop end continues into the sample at its loop start. A replacement inherits the two
	// loop *points* and not that property -- it is a different master placed at the same two
	// instants, and where the loop returns mid-phrase at full level the join opens up.
	// Measured against the game's own files across 1658 looping targets, 245 of ours click
	// where theirs does not, and those are the ones whose two ends sit at the same level
	// (1.4 dB apart, against 9.6 dB for the ones that read clean) -- that is, the ones where
	// there is loud music on both sides of the join and phase is the whole story.
	//
	// The standard loop crossfade fixes it, and exactly rather than approximately: over the
	// last n frames the output fades from x(t) to x(t - period), so the final frame is
	// x(loopStart - 1), which continues into x(loopStart) because they are adjacent samples of
	// one recording. The material it blends in is a second pass of the same music, so where
	// the loop really is seamless the blend changes nothing audible.
	//
	// Only when the seam is actually bad, because the blend is not free: it doubles the last
	// few milliseconds of the loop, and on the 903 targets whose join is already clean that
	// would be damage in exchange for nothing.
	struct seam_fix {
		double Before = 0.;
		double After = 0.;
		size_t Frames = 0;
		bool Applied = false;
	};

	seam_fix close_loop_seam(std::vector<float>& floats, size_t channels, size_t samplingRate,
		size_t loopStart, size_t loopEnd, double threshold, double maxSeconds) {

		seam_fix res;
		if (!channels || !samplingRate || loopEnd <= loopStart || maxSeconds <= 0.)
			return res;
		const auto frames = floats.size() / channels;
		if (loopEnd > frames)
			return res;

		// The metric judges one signal. For mono and stereo that is the channels summed: a
		// stereo pair is heard as one thing, and the metric was calibrated that way over
		// 1658 looping targets. Above two channels it is not -- those are two or three
		// independent stems the engine switches between, only one audible at a time -- so
		// each channel is judged on its own.
		const auto measure = [&](size_t channel) {
			std::vector<float> one(frames);
			for (size_t i = 0; i < frames; i++) {
				if (channels <= 2) {
					auto sum = 0.f;
					for (size_t c = 0; c < channels; c++)
						sum += floats[i * channels + c];
					one[i] = sum / static_cast<float>(channels);
				} else {
					one[i] = floats[i * channels + channel];
				}
			}
			return loop_seam_ratio(one, loopStart, loopEnd, samplingRate);
		};

		// The worst channel decides: a click in the stem that is playing is audible whatever
		// the silent ones are doing. Reads `floats`, so calling it after the blend measures
		// the blend.
		const auto worst = [&] {
			auto found = measure(0);
			for (size_t c = 1; channels > 2 && c < channels; c++) {
				const auto here = measure(c);
				if (here.Valid && (!found.Valid || here.Ratio > found.Ratio))
					found = here;
			}
			return found;
		};

		const auto before = worst();
		if (!before.Valid)
			return res;
		res.Before = before.Ratio;
		if (before.Ratio <= threshold)
			return res;

		// The blend reads a whole period earlier, so it cannot reach further back than the
		// loop start, and a quarter of the loop is as much of it as is reasonable to double.
		const auto period = loopEnd - loopStart;
		auto n = static_cast<size_t>(maxSeconds * static_cast<double>(samplingRate));
		n = (std::min)({n, loopStart, period / 4});
		if (n < 8)
			return res;

		for (size_t i = 0; i < n; i++) {
			// Linear rather than equal-power: the two sides are the same music one period
			// apart and therefore correlated, and an equal-power curve would bulge wherever
			// they agree. The weight reaches exactly 1 on the last frame, which is what makes
			// the final sample x(loopStart - 1) rather than nearly it.
			const auto w = static_cast<float>(i + 1) / static_cast<float>(n);
			const auto at = loopEnd - n + i;
			for (size_t c = 0; c < channels; c++)
				floats[at * channels + c] = floats[at * channels + c] * (1.f - w)
					+ floats[(at - period) * channels + c] * w;
		}

		// Measured the same way as `before`, from the buffer the blend just wrote.
		const auto after = worst();
		res.After = after.Valid ? after.Ratio : res.Before;
		res.Frames = n;
		res.Applied = true;
		return res;
	}

	// The last `seconds` of a file. Input-side -ss, because output-side seeking is what
	// corrupts a read deep into a file -- the same trap the loudness measurement hit.
	std::vector<float> decode_tail_to_floats(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& source,
		size_t channels,
		size_t samplingRate,
		double seconds,
		double duration,
		const std::filesystem::path& rawPath) {

		std::error_code ec;
		std::filesystem::remove(rawPath, ec);
		const auto from = (std::max)(0., duration - seconds);

		run_process_capture_stdout(ffmpeg, {
			L"-v", L"error",
			L"-ss", xivres::util::unicode::convert<std::wstring>(std::format("{:.6f}", from)),
			L"-i", source.wstring(),
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

	void reverse_frames(std::vector<float>& v, size_t channels) {
		if (channels < 1)
			return;
		const auto frames = v.size() / channels;
		for (size_t i = 0, j = frames ? frames - 1 : 0; i < j; ++i, --j)
			for (size_t c = 0; c < channels; ++c)
				std::swap(v[i * channels + c], v[j * channels + c]);
	}

	// Reproduce a fade-out the game's own file ends with and the recording does not.
	//
	// Truncating a non-looping replacement to the game's length is right, but a hard cut
	// where the game faded is worse than the overrun it replaces. This is the onset
	// correction's problem seen from the other end, so it is the onset correction's code:
	// both buffers are reversed by frame, `apply_onset_correction` treats the fade as an
	// attack, and the result is reversed back. Reusing it rather than writing the mirror
	// keeps one calibration -- the 20 ms blocks, the ~6 dB threshold and the interpolation
	// between block centres are all things that were tuned once.
	void apply_tail_correction(
		std::vector<float>& floats,
		const std::vector<float>& templateTail,
		size_t channels,
		size_t samplingRate,
		double& appliedDb,
		double& appliedSeconds) {

		appliedDb = 0.;
		appliedSeconds = 0.;
		if (!channels || floats.empty() || templateTail.empty())
			return;

		const auto frames = (std::min)(floats.size(), templateTail.size()) / channels;
		if (!frames)
			return;

		std::vector<float> ours(floats.end() - static_cast<ptrdiff_t>(frames * channels), floats.end());
		std::vector<float> theirs(templateTail.end() - static_cast<ptrdiff_t>(frames * channels), templateTail.end());
		reverse_frames(ours, channels);
		reverse_frames(theirs, channels);

		apply_onset_correction(ours, theirs, channels, samplingRate, appliedDb, appliedSeconds);
		if (appliedSeconds <= 0.)
			return;

		reverse_frames(ours, channels);
		std::copy(ours.begin(), ours.end(), floats.end() - static_cast<ptrdiff_t>(frames * channels));
	}

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

	// A native format-1 entry: interleaved 16-bit PCM, no wrapper, nothing to substitute.
	// The game reads this format as it ships, unlike the RIFF payload `wav` produces, which
	// keeps the entry at format 6 for a hook to reinterpret.
	xivres::sound::writer::sound_item make_native_pcm_entry(
		const std::vector<int16_t>& samples,
		size_t channels,
		size_t samplingRate,
		size_t loopStartBlockIndex,
		size_t loopEndBlockIndex) {

		const auto frameBytes = channels * sizeof(int16_t);
		std::vector<uint8_t> data(samples.size() * sizeof(int16_t));
		if (!samples.empty())
			std::memcpy(data.data(), samples.data(), data.size());

		// A PCM entry describes itself with a WAVEFORMATEX in its extra data, and its readers
		// require exactly that: `get_wav_header()` asserts the extra data is one of these plus
		// cbSize bytes, so an entry written without it throws the moment anything reads it
		// back -- which is how the first one written here was refused by `verify`.
		const xivres::sound::wave_format_ex format{
			.wFormatTag = xivres::sound::wave_format_tag::Pcm,
			.nChannels = static_cast<uint16_t>(channels),
			.nSamplesPerSec = static_cast<uint32_t>(samplingRate),
			.nAvgBytesPerSec = static_cast<uint32_t>(samplingRate * frameBytes),
			.nBlockAlign = static_cast<uint16_t>(frameBytes),
			.wBitsPerSample = 16,
			.cbSize = 0,
		};
		std::vector<uint8_t> extra(sizeof format);
		std::memcpy(extra.data(), &format, sizeof format);

		return {
			.Header = {
				.StreamSize = static_cast<uint32_t>(data.size()),
				.ChannelCount = static_cast<uint32_t>(channels),
				.SamplingRate = static_cast<uint32_t>(samplingRate),
				.Format = xivres::sound::sound_entry_format::WaveFormatPcm,
				// Byte offsets, as every entry states them; for linear PCM that is the sample
				// index times the frame size, which is the one case where the two agree.
				.LoopStartOffset = static_cast<uint32_t>(loopStartBlockIndex * frameBytes),
				.LoopEndOffset = static_cast<uint32_t>(loopEndBlockIndex * frameBytes),
				.StreamOffset = static_cast<uint32_t>(extra.size()),
				.Flags = xivres::sound::sound_entry_flags::None,
			},
			.ExtraData = std::move(extra),
			.Data = std::move(data),
		};
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
		// So the encoder can end a page there; see the option's comment for why the seek
		// wants a page start rather than a byte in the middle of one.
		opts.LoopStartSample = loopEndBlockIndex ? loopStartBlockIndex : 0;
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
			NativePcm,
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
		constexpr auto grammar = R"(ogg, ogg:<quality -1 to 10>, ogg:lossless, flac, flac:<level 0 to 8>, wav, or pcm)";
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

		if (name == "pcm") {
			if (!setting.empty())
				throw std::invalid_argument(std::format(R"({}: "pcm" takes no setting)", option));
			res.Codec = audio_format::codec::NativePcm;
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
				"  pcm            16-bit PCM in the game's own format 1, which it plays unmodified\n"
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
		parser.add_argument("--audio-format").default_value(std::string("ogg")).help(R"(what the entry's audio is, as codec[:setting]: "ogg", "ogg:<-1 to 10>", "ogg:lossless", "flac", "flac:<0 to 8>", "wav" or "pcm" (default: ogg, which is quality 10))");
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
		parser.add_argument("--loop-crossfade").default_value(0.030).scan<'g', double>().help("blend this many seconds of the loop's tail with the same music one loop-period earlier, so the loop point joins cleanly; 0 disables");
		parser.add_argument("--loop-seam-threshold").default_value(1.0).scan<'g', double>().help("only blend when the loop seam measures above this -- the jump at the loop point against the motion either side of it, where about 1 is where it starts to click");
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
		const auto loopCrossfadeSeconds = parser.get<double>("--loop-crossfade");
		const auto loopSeamThreshold = parser.get<double>("--loop-seam-threshold");

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
			presetPaths = release_ordered_presets({presetPath});
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

		const process_temp_directory tempRoot(L"apply");
		const auto& tempDir = tempRoot.path();
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
					for (const auto& [name, source] : segment.Sources) {
						// `"target"` has no file of its own: it is the game's entry, which is
						// staged a few lines below this check rather than found on disk. A
						// graph-built source's Path is only its first input, and the graph
						// resolved its own inputs already.
						if (source.IsTarget || source.Graph)
							continue;
						if (!std::filesystem::exists(source.Path))
							throw std::runtime_error(std::format("Source file not found: {}", u8(source.Path)));
					}
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
			const auto templateIsHca = hca_payload::is_hca(templateItem);
			if (templateItem.Header->Format != xivres::sound::sound_entry_format::Ogg
				&& templateItem.Header->Format != xivres::sound::sound_entry_format::WaveFormatPcm
				&& !templateIsHca)
				throw std::runtime_error(std::format("{} entry {} is format {}, which is neither Ogg, PCM nor HCA; refusing to replace it.",
					job.TargetPath, entryIndex, static_cast<uint32_t>(*templateItem.Header->Format)));

			const auto [templateLoopStart, templateLoopEnd] = template_loop_points(templateItem);

			const auto templateChannels = static_cast<size_t>(templateItem.Header->ChannelCount);
			// An Orchestrion roll is built mono: the game folds a stereo roll to mono on
			// playback, so its second channel only costs size -- and a stereo build would be
			// level-matched against a stereo reference no listener hears.
			const auto orchestrion = [&] {
				auto lower = job.TargetPath;
				std::ranges::transform(lower, lower.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
				return lower.starts_with("music/ffxiv/orchestrion/");
			}();
			const auto channels = orchestrion && templateChannels == 2 ? size_t{1} : templateChannels;
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
			const auto templateAudio = tempDir / std::format(L"scdtool_apply_src_{}{}",
				tempFileCounter.fetch_add(1), templateIsHca ? L".hca" : L".ogg");
			keepTemp(templateAudio);
			{
				std::ofstream f(templateAudio, std::ios::binary);
				if (!f)
					throw std::runtime_error(std::format("could not stage template audio for {}", job.TargetPath));
				const auto staged = templateIsHca
					? hca_payload::payload_file(templateItem)
					: templateItem.get_ogg_file();
				f.write(reinterpret_cast<const char*>(staged.data()), static_cast<std::streamsize>(staged.size()));
			}
			// What the game's own entry actually runs for. Only a target with no loop needs
			// it, but it is one ffprobe against a file already on disk, so it is read for all
			// of them rather than threaded through a condition.
			const auto templateSeconds = probe_duration(ffprobePath, templateAudio);

			if (templateIsHca) {
				const auto lock = std::scoped_lock(logMutex);
				std::cerr << std::format("  note: {} is HCA (format 26) and the replacement will not be -- "
					"the entry becomes whatever --audio-format names.", job.TargetPath) << '\n';
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
						for (const auto& [name, source] : segment.Sources) {
							// `"target"` is the game's own entry, whose rate is `templateRate`
							// and which has no file to probe yet. A graph-built source has no
							// single file either -- its rate follows its inputs, and taking
							// the first one's would be a guess.
							if (source.IsTarget || source.Graph)
								continue;
							samplingRate = (std::max)(samplingRate, static_cast<size_t>(probe_sample_rate(ffprobePath, source.Path)));
						}
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
			std::vector<std::pair<std::string, double>> segmentAlignments;
			std::vector<std::pair<std::string, double>> segmentGains;
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
				floats = build_segment_audio(ffmpegPath, ffprobePath, templateAudio, job.Segments, channels,
					samplingRate, loopEnd > loopStart ? loopEnd
						: static_cast<size_t>(std::llround(templateSeconds * static_cast<double>(samplingRate))),
					loudnessMatch, maxGainDb, tempFile, &segmentAlignments, &segmentGains);

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
				if (autoOffset && (!job.FromPreset || !job.OffsetStated)) {
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
				//
				// Over the loop where there is one; otherwise over as much of the file as both
				// sides have. Requiring a loop left every non-looping target at the master's
				// own level: BGM_EX2_Field_Safe_01 came out 3.7 dB quiet, which neither the
				// weighted score nor the envelope hole shows -- only a level comparison does.
				//
				// A volume= in the preset's filter is where the match starts, not a gain to keep:
				// the source is measured through the filter, as the segment path does. Generated
				// presets carry apply's own earlier gain that way, measured before the -ss and
				// mono-fold fixes, and keeping it left the one-segment Orchestrion rolls ~4 dB
				// quiet (BGM_ORCH_599: volume=-2.9dB stated, -5.6 dB against the game).
				const auto looped = newLoopEnd > newLoopStart;
				const auto spanFrom = looped ? newLoopStart : size_t{0};
				const auto spanTo = looped ? newLoopEnd : (std::min)(totalSamples,
					static_cast<size_t>(std::llround(templateSeconds * static_cast<double>(samplingRate))));
				if (loudnessMatch && spanTo > spanFrom) {
					const auto spanSeconds = static_cast<double>(spanTo - spanFrom) / static_cast<double>(samplingRate);
					const auto templateStartSeconds = looped
						? static_cast<double>(templateLoopStart) / static_cast<double>(templateRate) : 0.;

					// The source still has to be measured at the position the rebased output
					// took its samples from, which is offset by the match offset.
					const auto sourceStartSeconds = (std::max)(0.,
						static_cast<double>(spanFrom) / static_cast<double>(samplingRate) - effectiveOffset);

					try {
						// Through the preset's filter and the same channel conversion the decode
						// applies (-ac), which is what plays: a stereo recording measured unfolded
						// reads 3-6 dB louder than the mono entry it becomes. The game's side
						// takes the same fold where the entry is built with fewer channels than it has.
						const auto fold = std::format(L"aformat=channel_layouts={}",
							channels == 1 ? L"mono" : channels == 2 ? L"stereo" : std::format(L"{}c", channels));
						const auto templateLufs = measure_loudness(ffmpegPath, templateAudio, templateStartSeconds, spanSeconds,
							channels < templateChannels ? fold : std::wstring());
						const auto sourceLufs = measure_loudness(ffmpegPath, job.SourcePath, sourceStartSeconds, spanSeconds,
							job.Filter.empty() ? fold : job.Filter + L"," + fold);
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
			// A target with no loop has no loop end to truncate at, so until now nothing
			// bounded the render: 103 of the 201 non-looping targets came out longer than the
			// game's own file, some by minutes, because the album track carries the whole
			// piece where the game ships an excerpt. The game's own length is the bound.
			double tailDb = 0., tailSeconds = 0.;
			if (!newLoopEnd && templateSeconds > 0.) {
				const auto templateFrames = static_cast<size_t>(
					std::llround(templateSeconds * static_cast<double>(samplingRate)));
				if (templateFrames && totalSamples > templateFrames) {
					floats.resize(templateFrames * channels);
					totalSamples = templateFrames;

					// And if the game faded out where we now cut, fade out too.
					try {
						const auto tailRawPath = tempDir / std::format(L"scdtool_apply_tail_{}.f32",
							tempFileCounter.fetch_add(1));
						keepTemp(tailRawPath);
						constexpr double TailWindowSeconds = 3.0;
						const auto templateTail = decode_tail_to_floats(ffmpegPath, templateAudio,
							channels, samplingRate, TailWindowSeconds, templateSeconds, tailRawPath);
						if (!templateTail.empty())
							apply_tail_correction(floats, templateTail, channels, samplingRate,
								tailDb, tailSeconds);
					} catch (const std::exception&) {
						// Same principle as the onset catch: a failed tail check must not lose
						// the file. A hard cut is a worse ending than a fade, not a broken one.
						tailDb = 0.;
						tailSeconds = 0.;
					}
				}
			}

			// Before the channel permutation below and before quantisation, so every codec
			// gets the same audio and the blend runs in the channel order everything else
			// above works in.
			const auto seam = close_loop_seam(floats, channels, samplingRate,
				newLoopStart, newLoopEnd, loopSeamThreshold, loopCrossfadeSeconds);

			// Only for a Vorbis payload. WAV and FLAC both order their channels the way the
			// decoder hands them back -- FL, FR, FC, LFE, BL, BR -- which is the order
			// everything above already works in, so permuting for them would be the bug this
			// permutation exists to fix.
			if (channels == 6 && encodesVorbis) {
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

				case audio_format::codec::NativePcm:
					newEntry = make_native_pcm_entry(quantise_pcm16(floats, channels, newLoopEnd),
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
				std::cerr << std::format("  {} <- {} (score {:.3f}, offset {:+.3f}s{}{}, trim {} pad {} samples, loop {}-{}, gain {:+.1f} dB{}{}{}{}{})",
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
					onsetSeconds > 0. ? std::format(", onset corrected {:.1f} dB over {:.2f}s", onsetDb, onsetSeconds) : "",
					tailSeconds > 0. ? std::format(", tail faded {:.1f} dB over {:.2f}s", tailDb, tailSeconds) : "",
					[&] {
						if (segmentAlignments.empty())
							return std::string();
						std::string res = ", aligned";
						for (const auto& [name, seconds] : segmentAlignments)
							res += std::format(" {}@{:+.3f}s", name, seconds);
						return res;
					}() + [&] {
						if (segmentGains.empty())
							return std::string();
						std::string res = ", levels";
						for (const auto& [name, db] : segmentGains)
							res += std::format(" {} {:+.1f} dB", name, db);
						return res;
					}(),
					seam.Applied
						? std::format(", loop seam {:.2f} -> {:.2f} over {:.0f}ms", seam.Before, seam.After,
							1000. * static_cast<double>(seam.Frames) / static_cast<double>(samplingRate))
						: seam.Before > 0. ? std::format(", loop seam {:.2f}", seam.Before) : "")
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
