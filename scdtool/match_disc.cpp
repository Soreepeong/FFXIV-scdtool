#include "pch.h"
#include "commands.h"

#include "audio_match.h"
#include "mpls.h"

namespace {
	struct file_entry {
		std::filesystem::path Path;
		std::vector<float> Envelope;
		double DurationSeconds = 0;
	};

	constexpr double EnvelopeRateHz = 200.; // 16000 Hz / 80-sample hop, see decode_envelope's default.

	// Runs fn(i) for i in [0, count) across a pool of hardware_concurrency() threads.
	// If any invocation throws, all workers finish their current item and the first
	// exception is rethrown on the calling thread.
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

	std::vector<std::filesystem::path> list_files(const std::filesystem::path& dir, std::initializer_list<const wchar_t*> extensions) {
		std::vector<std::filesystem::path> result;
		for (const auto& entry : std::filesystem::directory_iterator(dir)) {
			if (!entry.is_regular_file())
				continue;
			if (extensions.size() == 0) {
				result.push_back(entry.path());
				continue;
			}
			const auto ext = xivres::util::unicode::convert<std::wstring>(entry.path().extension().wstring(), &xivres::util::unicode::lower);
			for (const auto e : extensions) {
				if (ext == e) {
					result.push_back(entry.path());
					break;
				}
			}
		}
		std::sort(result.begin(), result.end());
		return result;
	}

	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}
}

int cmd_match_disc(const argparse::ArgumentParser& args) {
	const auto discPath = args.get<std::filesystem::path>("--disc");
	const auto tracksPath = args.get<std::filesystem::path>("--tracks");
	const auto ffmpeg = args.get<std::filesystem::path>("--ffmpeg");
	const auto outputPath = args.get<std::filesystem::path>("--output");
	const auto minScore = args.get<double>("--min-score");
	const auto maxOffsetSeconds = args.get<double>("--max-offset-seconds");
	const auto minOverlapSeconds = args.get<double>("--min-overlap-seconds");
	const auto maxDurationDiff = args.get<double>("--max-duration-diff");
	const auto minClipSeconds = args.get<double>("--min-clip-seconds");

	auto bdmvPath = discPath / "BDMV";
	if (!is_directory(bdmvPath))
		bdmvPath = discPath; // allow passing the BDMV directory itself
	const auto streamPath = bdmvPath / "STREAM";
	if (!is_directory(streamPath))
		throw std::runtime_error(std::format("Could not find {}; pass either a disc root containing BDMV, or the BDMV directory itself.", u8(streamPath)));

	const auto m2tsFiles = list_files(streamPath, {L".m2ts"});
	const auto trackFiles = list_files(tracksPath, {});
	if (m2tsFiles.empty())
		throw std::runtime_error(std::format("No .m2ts files found under {}.", u8(streamPath)));
	if (trackFiles.empty())
		throw std::runtime_error(std::format("No candidate track files found under {}.", u8(tracksPath)));

	std::vector<file_entry> m2tsEntries(m2tsFiles.size());
	std::vector<file_entry> trackEntries(trackFiles.size());
	for (size_t i = 0; i < m2tsFiles.size(); ++i)
		m2tsEntries[i].Path = m2tsFiles[i];
	for (size_t i = 0; i < trackFiles.size(); ++i)
		trackEntries[i].Path = trackFiles[i];

	const auto totalToDecode = m2tsEntries.size() + trackEntries.size();
	std::atomic<size_t> decodedCount{0};
	std::mutex progressMutex;
	auto decodeOne = [&](file_entry& e) {
		e.Envelope = decode_envelope(ffmpeg, e.Path);
		e.DurationSeconds = static_cast<double>(e.Envelope.size()) / EnvelopeRateHz;
		const auto done = ++decodedCount;
		std::lock_guard lock(progressMutex);
		std::cerr << std::format("\rDecoding: {}/{}", done, totalToDecode) << std::flush;
	};
	parallel_for(m2tsEntries.size(), [&](size_t i) { decodeOne(m2tsEntries[i]); });
	parallel_for(trackEntries.size(), [&](size_t i) { decodeOne(trackEntries[i]); });
	std::cerr << std::endl;

	std::vector<std::pair<size_t, size_t>> pairs;
	for (size_t mi = 0; mi < m2tsEntries.size(); ++mi) {
		if (m2tsEntries[mi].DurationSeconds < minClipSeconds)
			continue;
		for (size_t ti = 0; ti < trackEntries.size(); ++ti) {
			if (std::abs(m2tsEntries[mi].DurationSeconds - trackEntries[ti].DurationSeconds) <= maxDurationDiff)
				pairs.emplace_back(mi, ti);
		}
	}

	std::cerr << std::format("Scoring {} candidate pairs...\n", pairs.size());
	struct candidate {
		size_t M2ts, Track;
		double Score;
	};
	std::vector<candidate> candidates(pairs.size());
	parallel_for(pairs.size(), [&](size_t i) {
		const auto [mi, ti] = pairs[i];
		candidates[i] = {
			mi, ti,
			best_envelope_correlation(m2tsEntries[mi].Envelope, trackEntries[ti].Envelope, EnvelopeRateHz, maxOffsetSeconds, minOverlapSeconds)
		};
	});
	std::sort(candidates.begin(), candidates.end(), [](const candidate& a, const candidate& b) { return a.Score > b.Score; });

	std::vector<uint8_t> m2tsAssigned(m2tsEntries.size(), 0), trackAssigned(trackEntries.size(), 0);
	struct match_result {
		size_t M2ts, Track;
		double Score;
	};
	std::vector<match_result> matches;
	for (const auto& c : candidates) {
		if (c.Score < minScore)
			break;
		if (m2tsAssigned[c.M2ts] || trackAssigned[c.Track])
			continue;
		m2tsAssigned[c.M2ts] = trackAssigned[c.Track] = 1;
		matches.push_back({c.M2ts, c.Track, c.Score});
	}
	std::sort(matches.begin(), matches.end(), [](const match_result& a, const match_result& b) { return a.M2ts < b.M2ts; });

	const auto playlists = parse_all_playlists(bdmvPath);
	const mpls_playlist* mainPlaylist = nullptr;
	for (const auto& pl : playlists) {
		if (!mainPlaylist || pl.PlayItems.size() > mainPlaylist->PlayItems.size())
			mainPlaylist = &pl;
	}

	nlohmann::json j;
	j["disc"] = u8(discPath);
	j["tracks"] = u8(tracksPath);

	auto& jm = j["matches"] = nlohmann::json::array();
	for (const auto& m : matches) {
		jm.push_back({
			{"m2ts", u8(m2tsEntries[m.M2ts].Path.filename())},
			{"track", u8(trackEntries[m.Track].Path.filename())},
			{"score", m.Score},
			{"m2tsDurationSeconds", m2tsEntries[m.M2ts].DurationSeconds},
			{"trackDurationSeconds", trackEntries[m.Track].DurationSeconds},
		});
	}

	auto& ju = j["unmatchedM2ts"] = nlohmann::json::array();
	for (size_t i = 0; i < m2tsEntries.size(); ++i) {
		if (m2tsAssigned[i])
			continue;
		ju.push_back({
			{"m2ts", u8(m2tsEntries[i].Path.filename())},
			{"durationSeconds", m2tsEntries[i].DurationSeconds},
			{"reason", m2tsEntries[i].DurationSeconds < minClipSeconds ? "tooShort" : "noConfidentMatch"},
		});
	}

	auto& jt = j["unmatchedTracks"] = nlohmann::json::array();
	for (size_t i = 0; i < trackEntries.size(); ++i) {
		if (trackAssigned[i])
			continue;
		jt.push_back({
			{"track", u8(trackEntries[i].Path.filename())},
			{"durationSeconds", trackEntries[i].DurationSeconds},
		});
	}

	if (mainPlaylist) {
		auto& jp = j["playlist"] = nlohmann::json::object();
		jp["file"] = u8(mainPlaylist->Path.filename());
		auto& jo = jp["order"] = nlohmann::json::array();
		for (const auto& item : mainPlaylist->PlayItems)
			jo.push_back(item.ClipId);
	}

	create_directories(outputPath.parent_path());
	std::ofstream ofs(outputPath, std::ios::binary);
	if (!ofs)
		throw std::runtime_error(std::format("Could not open {} for writing.", u8(outputPath)));
	ofs << j.dump(2);
	ofs.close();

	std::cerr << std::format(
		"Matched {}/{} m2ts files ({} candidate tracks left unmatched).\nWrote {}\n",
		matches.size(), m2tsEntries.size(), trackEntries.size() - matches.size(), u8(outputPath));

	return 0;
}
