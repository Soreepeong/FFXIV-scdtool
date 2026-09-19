#include "pch.h"
#include "apply.h"

#include "audio_match.h"
#include "win32_process.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <thread>

namespace {
	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}

	std::filesystem::path fromU8(const std::string& s) {
		return xivres::util::unicode::convert<std::wstring>(s);
	}

	// Each task holds a whole decoded track as float32 in memory (hundreds of MB for a
	// long one), so this is capped well below the core count to avoid exhausting RAM.
	template<typename Fn>
	void parallel_for(size_t count, Fn&& fn) {
		if (!count)
			return;
		constexpr size_t MaxThreads = 4;
		const auto threadCount = (std::min)({count, MaxThreads, static_cast<size_t>((std::max)(1u, std::thread::hardware_concurrency()))});
		std::atomic<size_t> next{0};
		std::mutex errorMutex;
		std::exception_ptr firstError;
		std::vector<std::thread> threads;
		threads.reserve(threadCount);
		for (size_t t = 0; t < threadCount; ++t) {
			threads.emplace_back([&fn, &next, count, &errorMutex, &firstError]() {
				while (true) {
					const auto i = next.fetch_add(1);
					if (i >= count)
						break;
					try {
						fn(i);
					} catch (...) {
						std::lock_guard lock(errorMutex);
						if (!firstError)
							firstError = std::current_exception();
					}
				}
			});
		}
		for (auto& th : threads)
			th.join();
		if (firstError)
			std::rethrow_exception(firstError);
	}

	struct apply_job {
		std::string TargetPath;   // path inside the game
		std::filesystem::path SourcePath;
		double Score = 0.;
		double Offset = 0.;       // seconds the source is shifted relative to the game's timeline
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
	double measure_loudness(const std::filesystem::path& ffmpeg, const std::filesystem::path& file, double startSeconds, double durationSeconds) {
		// -ss after -i: sample-accurate, which matters because the two sides of the
		// comparison must cover exactly the same musical span for the difference to mean
		// anything. The files are only minutes long, so the extra decode is cheap.
		std::vector<std::wstring> args{L"-v", L"info", L"-nostdin", L"-i", file.wstring()};
		if (startSeconds > 0)
			args.insert(args.end(), {L"-ss", std::to_wstring(startSeconds)});
		if (durationSeconds > 0)
			args.insert(args.end(), {L"-t", std::to_wstring(durationSeconds)});
		args.insert(args.end(), {L"-af", L"ebur128", L"-f", L"null", L"-"});

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
			try {
				value = std::stod(line.substr(valueStart));
			} catch (const std::exception&) {
				continue;
			}
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
		if (!f)
			return {};
		const auto size = static_cast<size_t>(f.tellg());
		f.seekg(0);

		std::vector<float> floats(size / sizeof(float));
		if (!floats.empty() && !f.read(reinterpret_cast<char*>(floats.data()), static_cast<std::streamsize>(floats.size() * sizeof(float))))
			return {};
		return floats;
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
		std::vector<double> ratios(totalBlocks, 1.);
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
			const auto ratioNext = (b + 1 < windowBlocks) ? ratios[b + 1] : ratios[b];
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
		parser.parse_args(args);
	} catch (const std::exception& e) {
		std::cerr
			<< "Error parsing arguments. Use `apply -h` to show help." << std::endl
			<< e.what() << std::endl;
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
		const auto ostDir = fromU8(parser.get<std::string>("--ost"));
		const auto presetPath = fromU8(parser.get<std::string>("--preset"));
		const auto outputDir = fromU8(parser.get<std::string>("--output-dir"));
		const auto ffmpegPath = fromU8(parser.get<std::string>("--ffmpeg"));
		const auto ffprobePath = fromU8(parser.get<std::string>("--ffprobe"));
		const auto samplingRateSpec = parser.get<std::string>("--sampling-rate");
		const auto entryIndex = parser.get<uint32_t>("--entry-index");
		const auto oggQuality = std::clamp(parser.get<float>("--ogg-quality"), 0.f, 1.f);
		const auto minScore = parser.get<double>("--min-score");
		const auto dryRun = parser.get<bool>("--dry-run");
		const auto verify = parser.get<bool>("--verify");
		const auto loudnessMatch = parser.get<bool>("--loudness-match") && !parser.get<bool>("--no-loudness-match");
		const auto onsetMatch = parser.get<bool>("--onset-match") && !parser.get<bool>("--no-onset-match");
		const auto maxGainDb = parser.get<double>("--max-gain");

		const auto gameRoot = [&] {
			if (gameSpec == ":global") return xivres::installation::find_installation_global();
			if (gameSpec == ":china") return xivres::installation::find_installation_china();
			if (gameSpec == ":korea") return xivres::installation::find_installation_korea();
			return fromU8(gameSpec);
		}();
		if (gameRoot.empty())
			throw std::runtime_error("Could not resolve game installation path.");
		const xivres::installation installation(gameRoot);

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
			if (info == item.end() || info->value("status", "") != "matched") {
				skippedUnmatched++;
				continue;
			}
			const auto score = info->value("score", 0.);
			if (score < minScore) {
				skippedUnmatched++;
				continue;
			}

			const auto target = item.find("target");
			if (target == item.end() || !target->is_object())
				continue;
			const auto path = target->find("path");
			if (path == target->end() || !path->is_string())
				continue;

			// "file" is the exact OST file that was scored, relative to --ost.
			const auto file = info->find("file");
			if (file == info->end() || !file->is_string())
				continue;

			jobs.push_back({
				path->get<std::string>(),
				ostDir / fromU8(file->get<std::string>()),
				score,
				info->value("offset", 0.),
			});
		}

		std::cerr << std::format("{} entr(ies) to rewrite, {} skipped (unmatched or below --min-score).",
			jobs.size(), skippedUnmatched) << std::endl;
		if (jobs.empty()) {
			std::cerr << "Nothing to do." << std::endl;
			return 0;
		}

		if (dryRun) {
			for (const auto& job : jobs)
				std::cerr << std::format("  would write {} <- {} (score {:.3f})", job.TargetPath, u8(job.SourcePath), job.Score) << std::endl;
			return 0;
		}

		const auto tempDir = std::filesystem::temp_directory_path();
		std::atomic<uint32_t> tempFileCounter{0};
		std::atomic<size_t> writtenCount{0};
		std::mutex logMutex;

		parallel_for(jobs.size(), [&](size_t index) {
			const auto& job = jobs[index];

			if (!std::filesystem::exists(job.SourcePath))
				throw std::runtime_error(std::format("Source file not found: {}", u8(job.SourcePath)));

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

			// The entry header also carries LoopStart/LoopEnd, but as byte offsets into the
			// entry's stream, on a different scale from the sample indices the encoder takes.
			// Reported only so a mismatch is visible when reviewing what was written.
			const auto headerLoopStart = static_cast<uint32_t>(templateItem.Header->LoopStartOffset);
			const auto headerLoopEnd = static_cast<uint32_t>(templateItem.Header->LoopEndOffset);

			const auto channels = static_cast<size_t>(templateItem.Header->ChannelCount);
			const auto templateRate = static_cast<size_t>(templateItem.Header->SamplingRate);
			if (!channels || !templateRate)
				throw std::runtime_error(std::format("{} has no channels or sample rate.", job.TargetPath));

			// A 4- or 6-channel entry is a set of engine-switched stems, not a surround mix,
			// and the album source is a single stereo track. Writing that in would collapse
			// the calm/battle switching into one state, so refuse rather than silently
			// produce a file that behaves differently in game.
			if (channels > 2) {
				const auto lock = std::lock_guard(logMutex);
				std::cerr << std::format("  SKIPPED {}: template has {} channels (engine-switched stems); a single stereo source cannot reproduce them",
					job.TargetPath, channels) << std::endl;
				return;
			}

			// The SCD must be Ogg Vorbis, so a source that is lossless and high-rate can
			// only lose by being forced down to the game file's rate. Take the highest rate
			// available across the two, as MusicImporter's
			// SamplingRate_UseHighestAvailable did -- which is why its output was 96 kHz
			// where the game shipped 44.1 kHz.
			size_t samplingRate;
			if (samplingRateSpec == "auto")
				samplingRate = (std::max)(templateRate, static_cast<size_t>(probe_sample_rate(ffprobePath, job.SourcePath)));
			else if (samplingRateSpec == "keep")
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

			const auto rawPath = tempDir / std::format(L"scdtool_apply_{}.f32", tempFileCounter.fetch_add(1));
			{
				const auto lock = std::lock_guard(logMutex);
				tempFiles.push_back(rawPath);
			}
			auto floats = decode_source_to_floats(ffmpegPath, job.SourcePath, channels, samplingRate, rawPath);

			// Rebase the source onto the game's timeline before anything else.
			//
			// A match at offset o means game time t holds the source's content at t - o: the
			// OST track carries an intro the game file does not, or lacks one it has. The
			// loop points below are expressed in the game's timeline, so without this the
			// loop lands wherever the OST's own lead-in happens to put it -- off by the
			// offset, typically 0.5-2s, which is enough to move the loop off its phrase
			// boundary and produce an audible seam.
			size_t paddingAdded = 0;
			size_t trimmedAway = 0;
			if (const auto offsetSamples = static_cast<int64_t>(std::llround(job.Offset * static_cast<double>(samplingRate)))) {
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
			const auto totalSamples = floats.size() / channels;
			auto newLoopStart = loopStart;
			auto newLoopEnd = loopEnd;
			if (newLoopEnd > totalSamples)
				newLoopEnd = totalSamples;
			if (newLoopStart >= totalSamples) {
				newLoopStart = 0;
				newLoopEnd = 0;
			}

			// Staged once, unconditionally: used for loudness measurement below when that
			// is enabled, and always for the onset-correction check afterward, since a
			// track's own onset treatment is worth reproducing whether or not overall
			// loudness matching is on.
			const auto templateAudio = tempDir / std::format(L"scdtool_apply_src_{}.ogg", tempFileCounter.fetch_add(1));
			{
				const auto lock = std::lock_guard(logMutex);
				tempFiles.push_back(templateAudio);
			}
			{
				std::ofstream f(templateAudio, std::ios::binary);
				if (!f)
					throw std::runtime_error(std::format("could not stage template audio for {}", job.TargetPath));
				const auto ogg = templateItem.get_ogg_file();
				f.write(reinterpret_cast<const char*>(ogg.data()), static_cast<std::streamsize>(ogg.size()));
			}

			// Match the replacement's level to the file it replaces, measured over the same
			// musical span on both sides. Without this the swapped track sits at the OST
			// master's level, which is usually hotter than the game's own mix and would
			// stand out against every other track in game.
			double gainDb = 0.;
			double requestedGainDb = 0.;
			bool gainLimited = false;
			if (loudnessMatch && (newLoopEnd > newLoopStart)) {
				const auto spanSeconds = static_cast<double>(newLoopEnd - newLoopStart) / static_cast<double>(samplingRate);
				const auto templateStartSeconds = static_cast<double>(templateLoopStart) / static_cast<double>(templateRate);

				// The source still has to be measured at the position the rebased output
				// took its samples from, which is offset by the match offset.
				const auto sourceStartSeconds = static_cast<double>(newLoopStart) / static_cast<double>(samplingRate) - job.Offset;

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
					if (const auto peak = floats.empty() ? 0.f : *std::max_element(floats.begin(), floats.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
						std::abs(peak) > 1.f) {
						const auto scale = 1.f / std::abs(peak);
						for (auto& v : floats)
							v = static_cast<float>(v * scale);
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
			double onsetDb = 0.;
			double onsetSeconds = 0.;
			if (onsetMatch) {
				constexpr double OnsetWindowSeconds = 3.0;  // longest observed real case was ~1.3s; ample margin
				const auto onsetRawPath = tempDir / std::format(L"scdtool_apply_onset_{}.f32", tempFileCounter.fetch_add(1));
				{
					const auto lock = std::lock_guard(logMutex);
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
			const auto outputPath = outputDir / fromU8(job.TargetPath);
			std::filesystem::create_directories(outputPath.parent_path());
			{
				std::ofstream f(outputPath, std::ios::binary);
				if (!f)
					throw std::runtime_error(std::format("Could not create {}", u8(outputPath)));
				f.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size()));
				if (!f)
					throw std::runtime_error(std::format("Could not write {}", u8(outputPath)));
			}

			// Read the file back and confirm the audio is there and the loop survived.
			// Encoding is the one step that can silently produce a file the game will not
			// loop correctly, and it is far cheaper to catch here than in-game.
			if (verify) {
				const auto checkStream = std::make_shared<xivres::file_stream>(outputPath);
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
				const auto expectedSamples = (newLoopEnd > 0 && newLoopEnd < totalSamples) ? newLoopEnd : totalSamples;
				const auto samplesDelta = checkSamples > expectedSamples ? checkSamples - expectedSamples : expectedSamples - checkSamples;
				if (samplesDelta > LoopToleranceSamples)
					throw std::runtime_error(std::format("verification failed for {}: {} samples read back, expected about {}",
						u8(outputPath), checkSamples, expectedSamples));
			}

			{
				const auto lock = std::lock_guard(logMutex);
				writtenCount++;
				std::cerr << std::format("  {} <- {} (score {:.3f}, offset {:+.3f}s, trim {} pad {} samples, loop {}-{}, gain {:+.1f} dB{}{})",
					job.TargetPath, u8(job.SourcePath), job.Score, job.Offset, trimmedAway, paddingAdded,
					newLoopStart, newLoopEnd, gainDb,
					gainLimited ? std::format(", peak-limited from {:+.1f} dB", requestedGainDb) : "",
					onsetSeconds > 0. ? std::format(", onset corrected {:.1f} dB over {:.2f}s", onsetDb, onsetSeconds) : "") << std::endl;
			}
		});

		cleanupTempFiles();
		std::cerr << std::format("Done. Wrote {} file(s) under {}.", writtenCount.load(), u8(outputDir)) << std::endl;
		return 0;

	} catch (const std::exception& e) {
		cleanupTempFiles();
		std::cerr
			<< "Error processing data." << std::endl
			<< e.what() << std::endl;
		return -1;
	}
}
