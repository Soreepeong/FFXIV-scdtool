#include "pch.h"
#include "match.h"

#include "audio_match.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

namespace {
	// Matches audio_match.h's decode_envelope default (16000 Hz PCM / 80-sample hop).
	constexpr double EnvelopeRateHz = 200.;

	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}

	std::filesystem::path resolve_game_root(const std::string& spec) {
		if (spec == ":global") {
			auto path = xivres::installation::find_installation_global();
			if (path.empty())
				throw std::runtime_error("Could not autodetect global client installation path.");
			return path;
		}
		if (spec == ":china") {
			auto path = xivres::installation::find_installation_china();
			if (path.empty())
				throw std::runtime_error("Could not autodetect Chinese client installation path.");
			return path;
		}
		if (spec == ":korea") {
			auto path = xivres::installation::find_installation_korea();
			if (path.empty())
				throw std::runtime_error("Could not autodetect Korean client installation path.");
			return path;
		}
		return xivres::util::unicode::convert<std::wstring>(spec);
	}

	// Runs fn(i) for i in [0, count) across a pool of hardware_concurrency() threads.
	// If any invocation throws, all workers finish their current item and the first
	// exception is rethrown on the calling thread. (Mirrors match_disc.cpp's helper;
	// kept as a separate copy so match.cpp has no dependency on that unwired file.)
	template<typename Fn>
	void parallel_for(size_t count, Fn&& fn) {
		if (!count)
			return;
		const auto threadCount = (std::min)(count, static_cast<size_t>((std::max)(1u, std::thread::hardware_concurrency())));
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

	// Extracts a game .scd's first sound entry to a temp file so it can go through
	// the same ffmpeg decode_envelope() path as OST candidate files, rather than
	// needing a second, native decode path just for the "target" side of a match.
	std::filesystem::path extract_scd_audio_to_temp(const xivres::installation& installation, const std::string& relativePath, const std::filesystem::path& tempDir, uint32_t uniqueId) {
		const auto stream = installation.get_file(relativePath);
		const xivres::sound::reader reader(stream);
		if (reader.sound_item_count() == 0)
			throw std::runtime_error("no sound entries");
		const auto item = reader.read_sound_item(0);

		std::vector<uint8_t> bytes;
		const wchar_t* ext;
		if (item.Header->Format == xivres::sound::sound_entry_format::Ogg) {
			bytes = item.get_ogg_file();
			ext = L".ogg";
		} else if (item.Header->Format == xivres::sound::sound_entry_format::WaveFormatPcm) {
			bytes = item.get_wav_file();
			ext = L".wav";
		} else {
			throw std::runtime_error("unsupported sound entry format for matching (expected Ogg or PCM wave)");
		}

		const auto path = tempDir / std::format(L"scdtool_match_{}{}", uniqueId, ext);
		std::ofstream f(path, std::ios::binary);
		if (!f)
			throw std::runtime_error(std::format("could not create temp file {}", u8(path)));
		f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!f)
			throw std::runtime_error(std::format("could not write temp file {}", u8(path)));
		return path;
	}

	struct candidate {
		std::filesystem::path Path;
		std::string RelName;
		std::vector<float> Envelope;
		double DurationSeconds = 0;
	};
}

int cmd_match(const std::vector<std::string>& args) {
	argparse::ArgumentParser parser("scdtool match");
	try {
		parser
			.add_description("Automatically match OST audio tracks to game .scd background music files by audio content correlation.")
			.add_epilog(
				"Input preset JSON is {\"items\": [...]}, where each item has a \"target\" (scd relative path, or\n"
				"array of paths) and optionally a \"source\" (OST file path relative to --ost). Items that already\n"
				"have a non-empty \"source\" are left untouched. Items this tool cannot confidently resolve are left\n"
				"without a \"source\", get \"enable\": false, and get a \"matchInfo\" field explaining why, for manual\n"
				"review.");
		parser.add_argument("--game").required().help(R"(game installation path, or :global/:china/:korea to autodetect)");
		parser.add_argument("--ost").required().help("directory containing candidate OST audio files (flac/ogg/wav/mp3), searched recursively");
		parser.add_argument("--preset").required().help("input preset JSON (skeleton with target paths)");
		parser.add_argument("--output").required().help("output preset JSON path (can be the same as --preset)");
		parser.add_argument("--ffmpeg").default_value(std::string("ffmpeg")).help("path to ffmpeg executable");
		parser.add_argument("--min-score").default_value(0.85).scan<'g', double>().help("minimum correlation score [-1..1] to accept a match automatically");
		parser.add_argument("--min-margin").default_value(0.05).scan<'g', double>().help("minimum score gap over the second-best candidate to accept a match unambiguously");
		parser.add_argument("--max-duration-diff").default_value(3.0).scan<'g', double>().help("maximum allowed duration difference in seconds to consider a candidate at all");
		parser.add_argument("--max-offset").default_value(15.0).scan<'g', double>().help("maximum time offset in seconds to search for alignment");
		parser.add_argument("--min-overlap").default_value(10.0).scan<'g', double>().help("minimum overlapping duration in seconds required for a correlation score to be considered valid");
		parser.parse_args(args);
	} catch (const std::exception& e) {
		std::cerr
			<< "Error parsing arguments. Use `match -h` to show help." << std::endl
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
		const auto ostDir = std::filesystem::path(xivres::util::unicode::convert<std::wstring>(parser.get<std::string>("--ost")));
		const auto presetPath = std::filesystem::path(xivres::util::unicode::convert<std::wstring>(parser.get<std::string>("--preset")));
		const auto outputPath = std::filesystem::path(xivres::util::unicode::convert<std::wstring>(parser.get<std::string>("--output")));
		const auto ffmpegPath = std::filesystem::path(xivres::util::unicode::convert<std::wstring>(parser.get<std::string>("--ffmpeg")));
		const auto minScore = parser.get<double>("--min-score");
		const auto minMargin = parser.get<double>("--min-margin");
		const auto maxDurationDiff = parser.get<double>("--max-duration-diff");
		const auto maxOffset = parser.get<double>("--max-offset");
		const auto minOverlap = parser.get<double>("--min-overlap");

		const auto gameRoot = resolve_game_root(gameSpec);
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

		std::vector<candidate> candidates;
		static const std::set<std::wstring> exts = {L".flac", L".ogg", L".wav", L".mp3", L".m4a"};
		for (const auto& entry : std::filesystem::recursive_directory_iterator(ostDir)) {
			if (!entry.is_regular_file())
				continue;
			const auto ext = xivres::util::unicode::convert<std::wstring>(entry.path().extension().wstring(), &xivres::util::unicode::lower);
			if (!exts.contains(ext))
				continue;

			candidate c;
			c.Path = entry.path();
			c.RelName = u8(relative(entry.path(), ostDir));
			candidates.push_back(std::move(c));
		}
		std::cerr << std::format("Found {} candidate OST file(s). Decoding...", candidates.size()) << std::endl;

		parallel_for(candidates.size(), [&](size_t i) {
			auto& c = candidates[i];
			c.Envelope = decode_envelope(ffmpegPath, c.Path);
			c.DurationSeconds = static_cast<double>(c.Envelope.size()) / EnvelopeRateHz;
		});
		std::erase_if(candidates, [](const candidate& c) { return c.Envelope.empty(); });
		std::cerr << std::format("{} candidate(s) ready for matching.", candidates.size()) << std::endl;

		size_t matchedCount = 0, ambiguousCount = 0, unmatchedCount = 0, skippedCount = 0;
		const auto tempDir = std::filesystem::temp_directory_path();
		uint32_t tempFileCounter = 0;

		for (auto& item : preset.at("items")) {
			if (!item.contains("target"))
				continue;
			auto& target = item.at("target");

			std::vector<std::string> targetPaths;
			if (const auto it = target.find("path"); it != target.end()) {
				if (it->is_string())
					targetPaths.push_back(it->get<std::string>());
				else if (it->is_array())
					for (const auto& p : *it)
						targetPaths.push_back(p.get<std::string>());
			}
			if (targetPaths.empty())
				continue;

			const bool hasSource = item.contains("source") && !item["source"].is_null()
				&& !(item["source"].is_string() && item["source"].get<std::string>().empty());
			if (hasSource) {
				skippedCount++;
				continue;
			}

			std::vector<float> targetEnvelope;
			double targetDuration = 0;
			try {
				const auto tempPath = extract_scd_audio_to_temp(installation, targetPaths.front(), tempDir, tempFileCounter++);
				tempFiles.push_back(tempPath);
				targetEnvelope = decode_envelope(ffmpegPath, tempPath);
				targetDuration = static_cast<double>(targetEnvelope.size()) / EnvelopeRateHz;
			} catch (const std::exception& e) {
				std::cerr << std::format("Warning: could not decode target {}: {}", targetPaths.front(), e.what()) << std::endl;
				item["matchInfo"] = {{"status", "error"}, {"error", e.what()}};
				item["enable"] = false;
				unmatchedCount++;
				continue;
			}

			struct scored {
				std::string Name;
				double Score;
			};
			std::vector<scored> scores;
			for (const auto& c : candidates) {
				if (std::abs(c.DurationSeconds - targetDuration) > maxDurationDiff)
					continue;
				const auto score = best_envelope_correlation(targetEnvelope, c.Envelope, EnvelopeRateHz, maxOffset, minOverlap);
				if (score > -1.5)
					scores.push_back({c.RelName, score});
			}
			std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) { return a.Score > b.Score; });

			nlohmann::json candidatesJson = nlohmann::json::array();
			for (size_t i = 0; i < std::min<size_t>(5, scores.size()); i++)
				candidatesJson.push_back({{"source", scores[i].Name}, {"score", scores[i].Score}});

			if (scores.empty()) {
				item["matchInfo"] = {{"status", "unmatched"}};
				item["enable"] = false;
				unmatchedCount++;
			} else {
				const bool confident = scores[0].Score >= minScore
					&& (scores.size() == 1 || scores[0].Score - scores[1].Score >= minMargin);
				if (confident) {
					item["source"] = scores[0].Name;
					item["matchInfo"] = {{"status", "matched"}, {"score", scores[0].Score}, {"candidates", candidatesJson}};
					matchedCount++;
				} else {
					item["matchInfo"] = {{"status", "ambiguous"}, {"candidates", candidatesJson}};
					item["enable"] = false;
					ambiguousCount++;
				}
			}
		}

		{
			std::ofstream f(outputPath, std::ios::binary);
			if (!f)
				throw std::runtime_error(std::format("Could not open output file: {}", u8(outputPath)));
			f << preset.dump(2);
		}

		cleanupTempFiles();
		std::cerr << std::format("Done. matched={} ambiguous={} unmatched={} skipped(already had source)={}",
			matchedCount, ambiguousCount, unmatchedCount, skippedCount) << std::endl;
		return 0;

	} catch (const std::exception& e) {
		cleanupTempFiles();
		std::cerr
			<< "Error processing data." << std::endl
			<< e.what() << std::endl;
		return -1;
	}
}
