#include "pch.h"
#include "apply.h"

#include "utils/argactions.h"
#include "utils/audio_match.h"
#include "utils/misc.h"
#include "utils/win32_process.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <functional>
#include <limits>
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

	struct apply_job {
		std::string TargetPath;   // path inside the game
		std::filesystem::path SourcePath;
		double Score = 0.;
		double Offset = 0.;       // seconds the source is shifted relative to the game's timeline
		std::vector<apply_stem> Stems;  // empty unless the entry is engine-switched stems
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
		const std::filesystem::path& rawPath) {

		std::error_code ec;
		std::filesystem::remove(rawPath, ec);

		// soxr, matching MusicImporter: the SCD is lossy Vorbis, so the resample is part
		// of the audible chain and a cheap one would be the weak link.
		run_process_capture_stdout(ffmpeg, {
			L"-v", L"error",
			L"-i", source.wstring(),
			L"-map", L"0:a:0",
			L"-ac", std::to_wstring(channels),
			L"-ar", std::to_wstring(samplingRate),
			L"-resampler", L"soxr",
			L"-f", L"f32le",
			L"-y", rawPath.wstring(),
		});

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
	std::vector<float> decode_channel_to_floats(
		const std::filesystem::path& ffmpeg,
		const std::filesystem::path& source,
		size_t channelIndex,
		size_t samplingRate,
		const std::filesystem::path& rawPath) {

		std::error_code ec;
		std::filesystem::remove(rawPath, ec);

		run_process_capture_stdout(ffmpeg, {
			L"-v", L"error",
			L"-i", source.wstring(),
			L"-map", L"0:a:0",
			L"-af", std::format(L"pan=mono|c0=c{}", channelIndex),
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
				"under --output-dir using the target's game-relative path.");
		parser.add_argument("--game").required().help(R"(game installation path, or :global/:china/:korea to autodetect)");
		parser.add_argument("--ost").required().help("directory the preset's source paths are relative to");
		parser.add_argument("--preset").required().help("matched preset JSON produced by `scdtool match`");
		parser.add_argument("--output-dir").required().help("directory to write replacement .scd files into");
		parser.add_argument("--ffmpeg").default_value(std::string("ffmpeg")).help("path to ffmpeg executable");
		parser.add_argument("--ffprobe").default_value(std::string("ffprobe")).help("path to ffprobe executable");
		parser.add_argument("--sampling-rate").default_value(std::string("auto")).help(R"(output sample rate: "auto" (highest of the game file and the source), "keep" (the game file's), or an integer)");
		parser.add_argument("--entry-index").default_value(0u).scan<'u', uint32_t>().help("sound entry index to replace (default: 0)");
		parser.add_argument("--ogg-quality").default_value(1.0f).scan<'g', float>().help("Ogg Vorbis encode quality, 0..1");
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
		const auto oggQuality = std::clamp(parser.get<float>("--ogg-quality"), 0.f, 1.f);
		const auto minScore = parser.get<double>("--min-score");
		const auto dryRun = parser.get<bool>("--dry-run");
		const auto verify = parser.get<bool>("--verify");
		const auto loudnessMatch = parser.get<bool>("--loudness-match") && !parser.get<bool>("--no-loudness-match");
		const auto onsetMatch = parser.get<bool>("--onset-match") && !parser.get<bool>("--no-onset-match");
		const auto autoOffset = parser.get<bool>("--auto-offset") && !parser.get<bool>("--no-auto-offset");
		const auto emitOriginal = parser.get<bool>("--emit-original");
		const auto maxGainDb = parser.get<double>("--max-gain");

		const xivres::installation installation(argactions::installation_root(gameSpec));

		nlohmann::json preset;
		{
			std::ifstream f(presetPath, std::ios::binary);
			if (!f)
				throw std::runtime_error(std::format("Could not open preset file: {}", u8(presetPath)));
			f >> preset;
		}
		if (!preset.contains("items") || !preset["items"].is_array())
			throw std::runtime_error("Preset file has no \"items\" array.");

		std::vector<apply_job> jobs;
		size_t skippedUnmatched = 0;
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

		std::cerr << std::format("{} entr(ies) to rewrite, {} skipped (unmatched or below --min-score).",
			jobs.size(), skippedUnmatched) << '\n';
		if (jobs.empty()) {
			std::cerr << "Nothing to do.\n";
			return 0;
		}

		if (dryRun) {
			for (const auto& job : jobs) {
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
		std::mutex logMutex;

		// Each job decodes and holds a whole track as float32 in memory (hundreds of MB for
		// a long one), so the worker count is kept well below the core count to avoid
		// exhausting RAM -- hence an explicit cap instead of parallel_for's default.
		constexpr size_t MaxDecodeThreads = 4;
		parallel_for(jobs.size(), [&](size_t index) {
			const auto& job = jobs[index];

			// A stem job has no single source; each stem carries its own, and only the
			// ones that resolved are going to be read.
			if (job.Stems.empty()) {
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
			if (channels > 2 && job.Stems.empty()) {
				const auto lock = std::scoped_lock(logMutex);
				std::cerr << std::format("  SKIPPED {}: template has {} channels (engine-switched stems); a single stereo source cannot reproduce them",
					job.TargetPath, channels) << '\n';
				return;
			}

			// Staged once, unconditionally: the offset deduction below aligns against it,
			// the loudness measurement compares to it, and the onset check samples it, so
			// it is wanted whatever the flags say.
			const auto templateAudio = tempDir / std::format(L"scdtool_apply_src_{}.ogg", tempFileCounter.fetch_add(1));
			{
				const auto lock = std::scoped_lock(logMutex);
				tempFiles.push_back(templateAudio);
			}
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
				if (job.Stems.empty()) {
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

			if (!job.Stems.empty()) {
				const auto tempFile = [&](const wchar_t* prefix, const wchar_t* extension) {
					auto path = tempDir / std::format(L"{}_{}{}", prefix, tempFileCounter.fetch_add(1), extension);
					const auto lock = std::scoped_lock(logMutex);
					tempFiles.push_back(path);
					return path;
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
				if (autoOffset) {
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

				const auto rawPath = tempDir / std::format(L"scdtool_apply_{}.f32", tempFileCounter.fetch_add(1));
				{
					const auto lock = std::scoped_lock(logMutex);
					tempFiles.push_back(rawPath);
				}
				floats = decode_source_to_floats(ffmpegPath, job.SourcePath, channels, samplingRate, rawPath);

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
				if (loudnessMatch && newLoopEnd > newLoopStart) {
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
				if (onsetMatch) {
					constexpr double OnsetWindowSeconds = 3.0;  // longest observed real case was ~1.3s; ample margin
					const auto onsetRawPath = tempDir / std::format(L"scdtool_apply_onset_{}.f32", tempFileCounter.fetch_add(1));
					{
						const auto lock = std::scoped_lock(logMutex);
						tempFiles.push_back(onsetRawPath);
					}
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
			if (channels == 6) {
				constexpr size_t VorbisToDecodedChannel[6] = {0, 2, 1, 4, 5, 3};
				std::vector<float> reordered(floats.size());
				for (size_t i = 0; i < totalSamples; ++i)
					for (size_t v = 0; v < 6; ++v)
						reordered[i * 6 + v] = floats[i * 6 + VorbisToDecodedChannel[v]];
				floats = std::move(reordered);
			}

			auto newEntry = xivres::sound::writer::sound_item::make_from_ogg_encode(
				channels,
				samplingRate,
				newLoopStart,
				newLoopEnd,
				xivres::memory_stream(xivres::util::span_cast<const uint8_t>(floats)).as_linear_reader<uint8_t>(),
				{},
				{},
				oggQuality);

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
					std::cerr << '\n';
					return;
				}
				std::cerr << std::format("  {} <- {} (score {:.3f}, offset {:+.3f}s{}, trim {} pad {} samples, loop {}-{}, gain {:+.1f} dB{}{})",
					job.TargetPath, u8(job.SourcePath), job.Score, effectiveOffset,
					deduced.Deduced && std::abs(effectiveOffset - job.Offset) > 0.05
						? std::format(" [deduced, was {:+.3f}s, intro {:.3f} over {} candidates]",
							job.Offset, deduced.Score, deduced.Candidates)
						: "",
					trimmedAway, paddingAdded,
					newLoopStart, newLoopEnd, gainDb,
					gainLimited ? std::format(", peak-limited from {:+.1f} dB", requestedGainDb) : "",
					onsetSeconds > 0. ? std::format(", onset corrected {:.1f} dB over {:.2f}s", onsetDb, onsetSeconds) : "") << '\n';
			}
		}, MaxDecodeThreads);

		cleanupTempFiles();
		std::cerr << std::format("Done. Wrote {} file(s) under {}.", writtenCount.load(), u8(outputDir)) << '\n';
		return 0;

	} catch (const std::exception& e) {
		cleanupTempFiles();
		std::cerr
			<< "Error processing data.\n"
			<< e.what() << '\n';
		return -1;
	}
}
