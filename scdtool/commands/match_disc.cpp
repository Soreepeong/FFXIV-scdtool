#include "pch.h"
#include "match_disc.h"

#include "utils/argactions.h"
#include "utils/audio_match.h"
#include "utils/misc.h"
#include "utils/mpls.h"
#include "utils/win32_process.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>

namespace {
	struct file_entry {
		std::filesystem::path Path;
		std::vector<float> Envelope;
		double DurationSeconds = 0;
	};

	constexpr double EnvelopeRateHz = 200.; // 16000 Hz / 80-sample hop, see decode_envelope's default.

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
		std::ranges::sort(result);
		return result;
	}

	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}


	// The --output value that means "write the report to stdout instead of a file", and its
	// default. The argument arrives as utf-8 text and is turned into a path after that, so this
	// is the narrow spelling that both sides compare against.
	constexpr auto StdoutOutput = "-";
	// Which of the two shapes of mapping the report is written as.
	enum class output_format {
		Json,
		Csv,
	};

	// Reads a track's TITLE tag, which the report carries alongside each match. The key is
	// matched case-insensitively: nothing guarantees which case a given ripper used. An empty
	// result simply means "no title tag", which is not an error -- the report leaves that field
	// blank (and a missing or failing ffprobe must not lose the whole mapping either).
	std::string read_title(const std::filesystem::path& ffprobe, const std::filesystem::path& source) {
		try {
			const auto out = run_process_capture_stdout(ffprobe, {
				L"-v", L"error",
				L"-show_entries", L"format_tags",
				L"-of", L"json",
				source.wstring(),
			});
			const auto json = nlohmann::json::parse(std::string(out.begin(), out.end()), nullptr, false);
			if (json.is_discarded() || !json.contains("format") || !json["format"].contains("tags"))
				return {};
			for (const auto& [key, value] : json["format"]["tags"].items()) {
				if (value.is_string() && xivres::util::unicode::convert<std::string>(key, &xivres::util::unicode::lower) == "title")
					return value.get<std::string>();
			}
			return {};
		} catch (const std::exception&) {
			return {};
		}
	}
}

int cmd_match_disc(const std::vector<std::string>& args) {
	argparse::ArgumentParser parser("scdtool match-disc");
	try {
		parser
			.add_description("Match the .m2ts clips of a Blu-ray disc to OST audio tracks by audio content correlation.")
			.add_epilog(
				"Writes one entry per .m2ts clip, named by the clip's file name without its extension; a\n"
				"matched entry also carries the OST file name without its extension, and that track's title\n"
				"tag. csv is a \"m2ts,mp3,title\" row per clip, with mp3/title blank when the clip did not\n"
				"match (every clip is listed); json is {\"matched\": {<m2ts>: {\"mp3\": ..., \"title\":\n"
				"...}}, \"unmatched\": [<m2ts>]}. Either way the entries are in the disc's playback order.");
		parser.add_argument("--disc").required().help("disc root containing BDMV/ (or the BDMV directory itself)")
			.action(argactions::existing_directory);
		parser.add_argument("--tracks").required().help("directory containing candidate OST audio files, searched recursively")
			.action(argactions::existing_directory);
		parser.add_argument("--output").default_value(std::filesystem::path(StdoutOutput)).help("output path for the mapping, or - for stdout (default)")
			.action(argactions::absolute_path_or_stdout);
		parser.add_argument("--format").help("output schema: json (default), or csv; when omitted, csv if --output ends with .csv, json otherwise")
			.action([](const std::string& spec) {
				if (spec == "json")
					return output_format::Json;
				if (spec == "csv")
					return output_format::Csv;
				throw std::runtime_error(std::format("Unknown output format: {} (valid: json and csv)", spec));
			});
		parser.add_argument("--ffmpeg").default_value(std::filesystem::path(L"ffmpeg")).help("path to ffmpeg executable")
			.action(argactions::path);
		parser.add_argument("--ffprobe").default_value(std::filesystem::path(L"ffprobe")).help("path to ffprobe executable, used to read track titles")
			.action(argactions::path);
		parser.add_argument("--min-score").default_value(0.5).scan<'g', double>().help("minimum correlation score [-1..1] to accept a match");
		parser.add_argument("--max-offset-seconds").default_value(10.0).scan<'g', double>().help("maximum time offset in seconds to search for alignment");
		parser.add_argument("--min-overlap-seconds").default_value(10.0).scan<'g', double>().help("minimum overlapping duration in seconds required for a correlation score to be considered valid");
		parser.add_argument("--max-duration-diff").default_value(10.0).scan<'g', double>().help("skip m2ts/track pairs whose length differs from each other by more than this many seconds; 0 disables the filter");
		parser.add_argument("--min-clip-seconds").default_value(0.0).scan<'g', double>().help("ignore m2ts clips shorter than this many seconds; 0 (default) considers every clip");
		parser.parse_args(args);
	} catch (const std::exception& e) {
		std::cerr
			<< "Error parsing arguments. Use `match-disc -h` to show help.\n"
			<< e.what() << '\n';
		return -1;
	}

	try {
		const auto discPath = parser.get<std::filesystem::path>("--disc");
		const auto tracksPath = parser.get<std::filesystem::path>("--tracks");
		const auto ffmpeg = parser.get<std::filesystem::path>("--ffmpeg");
		const auto ffprobe = parser.get<std::filesystem::path>("--ffprobe");
		const auto outputPath = parser.get<std::filesystem::path>("--output");
		const auto writeToStdout = outputPath == std::filesystem::path(StdoutOutput);
		// An explicit --format wins; otherwise a .csv output path selects csv, and anything else
		// (stdout included) is json.
		auto format = output_format::Json;
		if (const auto formatSpec = parser.present<output_format>("--format"))
			format = *formatSpec;
		else if (xivres::util::unicode::convert<std::wstring>(outputPath.extension().wstring(), &xivres::util::unicode::lower) == L".csv")
			format = output_format::Csv;
		const auto minScore = parser.get<double>("--min-score");
		const auto maxOffsetSeconds = parser.get<double>("--max-offset-seconds");
		const auto minOverlapSeconds = parser.get<double>("--min-overlap-seconds");
		const auto maxDurationDiff = parser.get<double>("--max-duration-diff");
		const auto minClipSeconds = parser.get<double>("--min-clip-seconds");

		auto bdmvPath = discPath / "BDMV";
		if (!std::filesystem::is_directory(bdmvPath))
			bdmvPath = discPath; // allow passing the BDMV directory itself
		const auto streamPath = bdmvPath / "STREAM";
		if (!std::filesystem::is_directory(streamPath))
			throw std::runtime_error(std::format("Could not find {}; pass either a disc root containing BDMV, or the BDMV directory itself.", u8(streamPath)));

		const auto m2tsFiles = list_files(streamPath, {L".m2ts"});
		const auto trackFiles = list_files(tracksPath, {});
		if (m2tsFiles.empty())
			throw std::runtime_error(std::format("No .m2ts files found under {}.", u8(streamPath)));
		if (trackFiles.empty())
			throw std::runtime_error(std::format("No candidate track files found under {}.", u8(tracksPath)));
		std::cerr << std::format("Found {} m2ts clip(s) and {} candidate track(s). Decoding...", m2tsFiles.size(), trackFiles.size()) << '\n';

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
			std::scoped_lock lock(progressMutex);
			std::cerr << std::format("\rDecoding: {}/{}", done, totalToDecode) << std::flush;
		};
		parallel_for(m2tsEntries.size(), [&](size_t i) { decodeOne(m2tsEntries[i]); });
		parallel_for(trackEntries.size(), [&](size_t i) { decodeOne(trackEntries[i]); });
		std::cerr << '\n';

		std::vector<std::pair<size_t, size_t>> pairs;
		for (size_t mi = 0; mi < m2tsEntries.size(); ++mi) {
			if (m2tsEntries[mi].DurationSeconds < minClipSeconds)
				continue;
			for (size_t ti = 0; ti < trackEntries.size(); ++ti) {
				if (maxDurationDiff > 0. && std::abs(m2tsEntries[mi].DurationSeconds - trackEntries[ti].DurationSeconds) > maxDurationDiff)
					continue;
				pairs.emplace_back(mi, ti);
			}
		}

		std::cerr << std::format("Scoring {} candidate pair(s)...", pairs.size()) << '\n';
		struct candidate {
			size_t M2ts, Track;
			double Score;
		};
		std::vector<candidate> candidates(pairs.size());
		parallel_for(pairs.size(), [&](size_t i) {
			const auto [mi, ti] = pairs[i];
			candidates[i] = {
				.M2ts = mi,
				.Track = ti,
				.Score = best_envelope_correlation(m2tsEntries[mi].Envelope, trackEntries[ti].Envelope, EnvelopeRateHz, maxOffsetSeconds, minOverlapSeconds),
			};
		});
		std::ranges::sort(candidates, [](const candidate& a, const candidate& b) { return a.Score > b.Score; });

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
			matches.push_back({.M2ts = c.M2ts, .Track = c.Track, .Score = c.Score});
		}
		std::ranges::sort(matches, [](const match_result& a, const match_result& b) { return a.M2ts < b.M2ts; });

		const auto playlists = parse_all_playlists(bdmvPath);
		const mpls_playlist* mainPlaylist = nullptr;
		for (const auto& pl : playlists) {
			if (!mainPlaylist || pl.PlayItems.size() > mainPlaylist->PlayItems.size())
				mainPlaylist = &pl;
		}

		// The mapping is keyed by file stems -- what a consumer already has when walking the
		// disc -- and ordered by the disc's own playlist, i.e. the order the content actually
		// plays in, so the report reads as a track list. Clips the playlist does not mention
		// follow in file name order.
		const auto stem = [](const std::filesystem::path& p) {
			return xivres::util::unicode::convert<std::string>(p.stem().wstring());
		};
		std::vector<size_t> rowByPlayback(m2tsEntries.size());
		{
			std::map<std::string, size_t> entryByClipId;
			std::vector playbackOrder(m2tsEntries.size(), SIZE_MAX);
			size_t next = 0;
			for (size_t i = 0; i < m2tsEntries.size(); ++i)
				entryByClipId[stem(m2tsEntries[i].Path)] = i;
			if (mainPlaylist)
				for (const auto& item : mainPlaylist->PlayItems) {
					if (const auto it = entryByClipId.find(item.ClipId); it != entryByClipId.end() && playbackOrder[it->second] == SIZE_MAX)
						playbackOrder[it->second] = next++;
				}
			for (size_t i = 0; i < m2tsEntries.size(); ++i)
				if (playbackOrder[i] == SIZE_MAX)
					playbackOrder[i] = next++;
			for (size_t i = 0; i < m2tsEntries.size(); ++i)
				rowByPlayback[playbackOrder[i]] = i;
		}

		// Titles are one ffprobe invocation per matched track, so they are read in parallel,
		// and only for the tracks that actually won a clip.
		std::vector<std::string> titles(matches.size());
		parallel_for(matches.size(), [&](size_t i) { titles[i] = read_title(ffprobe, trackEntries[matches[i].Track].Path); });
		std::vector matchOfM2ts(m2tsEntries.size(), SIZE_MAX);
		for (size_t i = 0; i < matches.size(); ++i)
			matchOfM2ts[matches[i].M2ts] = i;

		std::string report;
		if (format == output_format::Csv) {
			const auto field = [](const std::string& value) {
				if (value.find_first_of(",\"\r\n") == std::string::npos)
					return value;
				std::string quoted(1, '"');
				for (const auto ch : value) {
					if (ch == '"')
						quoted += '"';
					quoted += ch;
				}
				quoted += '"';
				return quoted;
			};
			report = "m2ts,mp3,title";
			for (const auto entry : rowByPlayback) {
				const auto mi = matchOfM2ts[entry];
				const auto mp3 = mi == SIZE_MAX ? std::string() : stem(trackEntries[matches[mi].Track].Path);
				const auto title = mi == SIZE_MAX ? std::string() : titles[mi];
				report += '\n' + field(stem(m2tsEntries[entry].Path)) + ',' + field(mp3) + ',' + field(title);
			}
		} else {
			// ordered_json keeps its insertion order (the playback order computed above),
			// but its object storage is a vector: inserting a key reallocates it, so no
			// reference into j may be held across a later insertion -- hence the lookups
			// below are repeated rather than cached in jm/ju.
			nlohmann::ordered_json j;
			j["matched"] = nlohmann::ordered_json::object();
			j["unmatched"] = nlohmann::ordered_json::array();
			for (const auto entry : rowByPlayback) {
				const auto name = stem(m2tsEntries[entry].Path);
				if (const auto mi = matchOfM2ts[entry]; mi == SIZE_MAX)
					j["unmatched"].push_back(name);
				else
					j["matched"][name] = {{"mp3", stem(trackEntries[matches[mi].Track].Path)}, {"title", titles[mi]}};
			}
			report = j.dump(2);
		}

		if (writeToStdout) {
			std::cout << report << '\n';
		} else {
			if (!outputPath.parent_path().empty())
				std::filesystem::create_directories(outputPath.parent_path());
			std::ofstream ofs(outputPath, std::ios::binary);
			if (!ofs)
				throw std::runtime_error(std::format("Could not open {} for writing.", u8(outputPath)));
			ofs << report << '\n';
		}

		std::cerr << std::format("Matched {}/{} m2ts clip(s) ({} candidate track(s) left unmatched).",
			matches.size(), m2tsEntries.size(), trackEntries.size() - matches.size()) << '\n';
		if (!writeToStdout)
			std::cerr << std::format("Wrote {}", u8(outputPath)) << '\n';
		return 0;

	} catch (const std::exception& e) {
		std::cerr
			<< "Error processing data.\n"
			<< e.what() << '\n';
		return -1;
	}
}
