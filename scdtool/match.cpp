#include "pch.h"
#include "match.h"

#include "audio_match.h"
#include "win32_process.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <set>
#include <thread>
#include <unordered_map>

#include <xivres/excel.h>

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
	std::filesystem::path extract_scd_audio_to_temp(const xivres::installation& installation, const std::string& relativePath, const std::filesystem::path& tempDir, uint32_t uniqueId, size_t* channels = nullptr) {
		const auto stream = installation.get_file(relativePath);
		const xivres::sound::reader reader(stream);
		if (reader.sound_item_count() == 0)
			throw std::runtime_error("no sound entries");

		// Take the first entry that actually carries audio in a format we can hand to
		// ffmpeg; entry 0 is empty in some of the game's files.
		std::vector<uint8_t> bytes;
		const wchar_t* ext = nullptr;
		size_t foundChannels = 0;
		for (size_t i = 0; i < reader.sound_item_count(); ++i) {
			const auto item = reader.read_sound_item(i);
			if (item.Header->Format == xivres::sound::sound_entry_format::Ogg) {
				bytes = item.get_ogg_file();
				ext = L".ogg";
			} else if (item.Header->Format == xivres::sound::sound_entry_format::WaveFormatPcm) {
				bytes = item.get_wav_file();
				ext = L".wav";
			} else {
				continue;
			}
			if (!bytes.empty()) {
				foundChannels = static_cast<uint32_t>(item.Header->ChannelCount);
				break;
			}
		}
		if (!ext || bytes.empty())
			throw std::runtime_error("no usable sound entry (expected a non-empty Ogg or PCM wave entry)");
		if (channels)
			*channels = foundChannels;

		const auto path = tempDir / std::format(L"scdtool_match_{}{}", uniqueId, ext);
		std::ofstream f(path, std::ios::binary);
		if (!f)
			throw std::runtime_error(std::format("could not create temp file {}", u8(path)));
		f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!f)
			throw std::runtime_error(std::format("could not write temp file {}", u8(path)));
		return path;
	}

	// Pulls one stereo stem out of a multi-channel entry as a mono file, given the two
	// raw channel indices that make up that stem (see discover_channel_pairing -- the
	// game's own channel layout is not guaranteed to be sequential pairs).
	//
	// The game's 4- and 6-channel music is not surround: it is stems the engine switches
	// between -- 2ch for the out-of-combat state, 2ch for the in-combat state, and 2ch of
	// transition cymbal, with the 4-channel files simply omitting the cymbal. Downmixing
	// that to mono, which is what scoring an entry as one signal does, produces something
	// that corresponds to no album track at all, so these never matched.
	std::filesystem::path extract_stem_to_temp(const std::filesystem::path& ffmpeg, const std::filesystem::path& source, size_t channelA, size_t channelB, const std::filesystem::path& tempDir, uint32_t uniqueId) {
		const auto path = tempDir / std::format(L"scdtool_match_stem_{}.wav", uniqueId);
		run_process_capture_stdout(ffmpeg, {
			L"-v", L"error",
			L"-i", source.wstring(),
			L"-af", std::format(L"pan=mono|c0=0.5*c{}+0.5*c{}", channelA, channelB),
			L"-ar", L"16000",
			L"-c:a", L"pcm_s16le",
			L"-y", path.wstring(),
		});
		return path;
	}

	// Pulls a single raw channel out of a multi-channel entry as a mono file -- used only
	// to discover which channels actually pair up (see discover_channel_pairing).
	std::filesystem::path extract_single_channel_to_temp(const std::filesystem::path& ffmpeg, const std::filesystem::path& source, size_t channelIndex, const std::filesystem::path& tempDir, uint32_t uniqueId) {
		const auto path = tempDir / std::format(L"scdtool_match_ch_{}.wav", uniqueId);
		run_process_capture_stdout(ffmpeg, {
			L"-v", L"error",
			L"-i", source.wstring(),
			L"-af", std::format(L"pan=mono|c0=c{}", channelIndex),
			L"-ar", L"16000",
			L"-c:a", L"pcm_s16le",
			L"-y", path.wstring(),
		});
		return path;
	}

	// Which raw channels actually pair up as a stereo stem, discovered empirically rather
	// than assumed.
	//
	// A 6-channel entry was measured to lay its three stereo stems out as (0,2)(1,4)(3,5),
	// not the sequential (0,1)(2,3)(4,5) an earlier version of this file assumed -- the
	// asset's own channel layout, not an artifact of decoding, so it cannot be trusted to
	// be sequential for any given file. Decodes every channel individually, correlates
	// every pair (there are only 3 or 15 of them for 4 or 6 channels), and picks whichever
	// perfect matching of channels into stereo pairs maximizes total correlation -- a real
	// stereo pair reads close to 1.0 at zero lag; unrelated channels read near 0.
	std::vector<std::pair<size_t, size_t>> discover_channel_pairing(
		const std::filesystem::path& ffmpegPath,
		const std::filesystem::path& targetAudio,
		size_t channelCount,
		const std::filesystem::path& tempDir,
		std::atomic<uint32_t>& tempFileCounter,
		std::vector<std::filesystem::path>& tempFiles,
		std::mutex& tempFilesMutex) {
		std::vector<std::vector<float>> channelEnvelopes(channelCount);
		for (size_t ch = 0; ch < channelCount; ++ch) {
			const auto chPath = extract_single_channel_to_temp(ffmpegPath, targetAudio, ch, tempDir, tempFileCounter.fetch_add(1));
			{
				const auto lock = std::lock_guard(tempFilesMutex);
				tempFiles.push_back(chPath);
			}
			channelEnvelopes[ch] = decode_envelope(ffmpegPath, chPath);
		}

		// Zero-lag-ish correlation: these are channels of the same stream, so any real
		// pair is already sample-aligned; a small offset budget just absorbs decode jitter.
		std::vector<std::vector<double>> pairScore(channelCount, std::vector<double>(channelCount, -1.));
		for (size_t a = 0; a < channelCount; ++a)
			for (size_t b = a + 1; b < channelCount; ++b)
				pairScore[a][b] = pairScore[b][a] = best_envelope_correlation(channelEnvelopes[a], channelEnvelopes[b], EnvelopeRateHz, 0.25, 1., 0.99);

		// Brute-force every perfect matching of {0..channelCount-1} into channelCount/2
		// pairs -- at most 15 of them (6 channels), so there is no need for anything
		// cleverer than recursion.
		std::vector<size_t> remaining(channelCount);
		std::iota(remaining.begin(), remaining.end(), size_t{0});
		std::vector<std::pair<size_t, size_t>> best;
		double bestScore = -1e9;
		const std::function<void(std::vector<size_t>, std::vector<std::pair<size_t, size_t>>, double)> recurse =
			[&](std::vector<size_t> left, std::vector<std::pair<size_t, size_t>> chosen, double scoreSoFar) {
				if (left.empty()) {
					if (scoreSoFar > bestScore) {
						bestScore = scoreSoFar;
						best = chosen;
					}
					return;
				}
				const auto first = left.front();
				for (size_t i = 1; i < left.size(); ++i) {
					const auto partner = left[i];
					auto nextLeft = left;
					nextLeft.erase(nextLeft.begin() + static_cast<ptrdiff_t>(i));
					nextLeft.erase(nextLeft.begin());
					auto nextChosen = chosen;
					nextChosen.emplace_back(first, partner);
					recurse(std::move(nextLeft), std::move(nextChosen), scoreSoFar + pairScore[first][partner]);
				}
			};
		recurse(remaining, {}, 0.);
		for (auto& [a, b] : best)
			if (a > b)
				std::swap(a, b);
		std::sort(best.begin(), best.end());
		return best;
	}

	struct candidate {
		std::filesystem::path Path;
		std::string RelName;
		std::vector<float> Envelope;
		double DurationSeconds = 0;
		std::string NormalizedTitle;  // empty if the file has no readable title tag
		std::string RawTitle;         // title tag as read, untouched (for display, e.g. "sourceTitle")
		std::string EnglishTitle;     // first '/'-separated part that is pure ASCII, if any
		std::string JapaneseTitle;    // first '/'-separated part containing non-ASCII bytes, if any
	};

	struct title_info {
		std::string Normalized;  // sorted, deduped, lowercased parts joined by \x1f -- for duplicate-release comparison
		std::string Raw;         // title tag exactly as read, for display (e.g. "sourceTitle")
		std::string English;     // first '/'-separated part that is pure ASCII, trimmed but not lowercased
		std::string Japanese;    // first '/'-separated part containing a non-ASCII byte, trimmed but not lowercased
	};

	// Reads a track's TITLE tag (case-insensitive key; FLAC/Vorbis-comment tags are
	// conventionally case-insensitive and different rippers write different cases).
	//
	// `Normalized` exists so the same piece re-released on two different album discs
	// compares equal even when the tag's language order is swapped -- e.g. one disc's
	// "<English title> / <Japanese title>" and another's "<Japanese title> / <English
	// title>" both normalize to the same string (a real example: "Fleeting Rays" paired
	// with its Japanese title, swapped between the ARR and Trail to the Heavens discs).
	// This is a far cheaper and more reliable duplicate-release signal than
	// an audio-correlation threshold: two masterings of the same piece can legitimately
	// correlate below any fixed threshold (a real case measured at 0.935, under the
	// 0.95 the audio check uses), while the title is exact by construction.
	//
	// `English`/`Japanese` split the same tag by language, classified by whether a part
	// contains any non-ASCII byte -- this OST catalog only ever mixes those two scripts,
	// so "any non-ASCII byte" is a reliable enough stand-in for "is Japanese" here.
	title_info read_title_info(const std::filesystem::path& ffprobe, const std::filesystem::path& source) {
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
			std::string title;
			for (const auto& [key, value] : json["format"]["tags"].items()) {
				auto lowerKey = key;
				std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (lowerKey == "title" && value.is_string()) {
					title = value.get<std::string>();
					break;
				}
			}
			if (title.empty())
				return {};

			title_info info;
			info.Raw = title;

			// Split on '/', trim each part.
			std::vector<std::string> rawParts;
			size_t start = 0;
			for (size_t i = 0; i <= title.size(); ++i) {
				if (i == title.size() || title[i] == '/') {
					auto part = title.substr(start, i - start);
					const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
					part.erase(part.begin(), std::find_if(part.begin(), part.end(), notSpace));
					part.erase(std::find_if(part.rbegin(), part.rend(), notSpace).base(), part.end());
					if (!part.empty())
						rawParts.push_back(std::move(part));
					start = i + 1;
				}
			}

			for (const auto& part : rawParts) {
				const bool isAscii = std::all_of(part.begin(), part.end(), [](unsigned char c) { return c < 0x80; });
				if (isAscii) {
					if (info.English.empty())
						info.English = part;
				} else {
					if (info.Japanese.empty())
						info.Japanese = part;
				}
			}

			// Normalized: lowercase every part (a no-op on non-Latin script, harmless),
			// sort so language order does not matter, then drop duplicates -- a piece
			// with no distinct localized title is sometimes tagged with the same text on
			// both sides of the '/' (e.g. "Title / Title"), which must normalize the same
			// as a release tagged with that title only once, or the two compare unequal
			// purely because one release's tag is more redundant than the other's.
			std::vector<std::string> normParts = rawParts;
			for (auto& part : normParts)
				std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::sort(normParts.begin(), normParts.end());
			normParts.erase(std::unique(normParts.begin(), normParts.end()), normParts.end());
			for (const auto& part : normParts) {
				if (!info.Normalized.empty())
					info.Normalized += '\x1f';
				info.Normalized += part;
			}
			return info;
		} catch (const std::exception&) {
			return {};
		}
	}

	// Every BGM path the installed game knows about, read from the `bgm` excel sheet.
	//
	// xivres can only look files up by hash, so there is no way to enumerate the music
	// folder directly; this sheet is the only way to discover target .scd paths, which
	// is what makes presets for content that has none yet possible at all.
	//
	// Column names are not stored in the sheet header (exh::column carries only a type
	// and an offset), so rather than hard-code a column index that a game update could
	// shift, take any string cell that looks like a music path.
	std::vector<std::string> enumerate_bgm_paths(const xivres::installation& installation) {
		// Deliberately the default reader, not new_with_language(): a client need not ship
		// every localized page file (this one only has exd/bgm_0.exd, not bgm_0_en.exd),
		// and the columns read here are paths, which are not localized anyway.
		const auto sheet = installation.get_excel("bgm");
		const auto& exh = sheet.get_exh_reader();

		std::set<std::string> paths;
		for (size_t pageIndex = 0; pageIndex < exh.get_pages().size(); ++pageIndex) {
			for (const auto& row : sheet.get_exd_reader(pageIndex)) {
				for (const auto& subrow : row) {
					for (const auto& cell : subrow) {
						if (cell.Type != xivres::excel::cell_type::String)
							continue;
						auto path = cell.String.repr();
						if (path.starts_with("music/") && path.ends_with(".scd"))
							paths.insert(std::move(path));
					}
				}
			}
		}
		return {paths.begin(), paths.end()};
	}

	// Collects the "path" of one target object, which the schema allows to be either a
	// single string or a list of strings.
	void append_target_paths(std::vector<std::string>& res, const nlohmann::json& target) {
		const auto path = target.find("path");
		if (path == target.end())
			return;
		if (path->is_string()) {
			if (!path->get<std::string>().empty())
				res.push_back(path->get<std::string>());
		} else if (path->is_array()) {
			for (const auto& p : *path)
				if (p.is_string() && !p.get<std::string>().empty())
					res.push_back(p.get<std::string>());
		}
	}

	// Target paths of a preset item. `target` may be a single object or, for entries
	// that feed one source into several files, a list of them.
	std::vector<std::string> preset_target_paths(const nlohmann::json& item) {
		std::vector<std::string> res;
		const auto target = item.find("target");
		if (target == item.end())
			return res;
		if (target->is_array()) {
			for (const auto& t : *target)
				append_target_paths(res, t);
		} else {
			append_target_paths(res, *target);
		}
		return res;
	}

	// An entry is deliberately disabled when every one of its targets says so.
	bool preset_target_enabled(const nlohmann::json& item) {
		const auto target = item.find("target");
		if (target == item.end())
			return false;
		const auto enabled = [](const nlohmann::json& t) {
			const auto it = t.find("enable");
			return !(it != t.end() && it->is_boolean() && !it->get<bool>());
		};
		if (!target->is_array())
			return enabled(*target);
		if (target->empty())
			return false;
		for (const auto& t : *target)
			if (enabled(t))
				return true;
		return false;
	}
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
				"review.\n"
				"\n"
				"With --discover, the targets are instead read from the installed game's own bgm sheet, so a\n"
				"preset can be built for content that has none yet. Combine with --exclude-preset to consider\n"
				"only BGM not already covered by existing presets, and --target-prefix to narrow it to one\n"
				"expansion's music folder.");
		parser.add_argument("--game").required().help(R"(game installation path, or :global/:china/:korea to autodetect)");
		parser.add_argument("--ost").required().help("directory containing candidate OST audio files (flac/ogg/wav/mp3), searched recursively");
		parser.add_argument("--preset").help("input preset JSON (skeleton with target paths)");
		parser.add_argument("--output").required().help("output preset JSON path (can be the same as --preset)");
		parser.add_argument("--discover").default_value(false).implicit_value(true).help("also take targets from the game's own bgm sheet, not just from --preset");
		parser.add_argument("--target-prefix").default_value(std::string()).help("with --discover, only consider BGM paths starting with one of these comma-separated prefixes (e.g. music/ex5/); empty means all");
		parser.add_argument("--target-exclude-prefix").default_value(std::string()).help("with --discover, skip BGM paths starting with any of these comma-separated prefixes (e.g. music/ffxiv/Orchestrion/)");
		parser.add_argument("--exclude-preset").default_value(std::string()).help("comma-separated preset JSON files whose target paths should be excluded from matching");
		parser.add_argument("--matched-only").default_value(false).implicit_value(true).help("write only the items that were matched, so the output is directly usable as a preset");
		parser.add_argument("--ffmpeg").default_value(std::string("ffmpeg")).help("path to ffmpeg executable");
		parser.add_argument("--ffprobe").default_value(std::string("ffprobe")).help("path to ffprobe executable, used to read title tags for duplicate-release detection");
		// Re-measured against the 690 hand-verified target->source mappings in the
		// existing presets (a far larger and more honest sample than one ground-truth
		// album, and one that actually contains wrong answers to catch): sweeping this
		// from 0.75 down to 0.10, with min-margin fixed at its default, adds exactly one
		// new wrong answer out of 690 while recovering 26 more correct matches (587->613
		// agree). The margin check, not this floor, is what rejects false positives --
		// ambient/dungeon tracks correlate weakly on their RMS envelope even when they
		// are the correct match, and a floor of 0.75 was quietly discarding those.
		parser.add_argument("--min-score").default_value(0.5).scan<'g', double>().help("minimum correlation score [-1..1] to accept a match automatically");
		parser.add_argument("--min-margin").default_value(0.05).scan<'g', double>().help("minimum score gap over the best genuinely different candidate to accept a match unambiguously");
		parser.add_argument("--duplicate-threshold").default_value(0.95).scan<'g', double>().help("candidates correlating at least this much with the winner are the same recording on another album, and are skipped when measuring the margin");
		parser.add_argument("--segment-min-score").default_value(0.75).scan<'g', double>().help("report the distinct non-overlapping regions of the target that score at least this well, which decomposes a medley into its component tracks; 0 disables");
		parser.add_argument("--max-duration-diff").default_value(0.0).scan<'g', double>().help("skip candidates whose length differs from the target by more than this many seconds; 0 (default) disables the filter so that long medleys can still match their component tracks");
		parser.add_argument("--max-offset").default_value(600.0).scan<'g', double>().help("maximum time offset in seconds to search for alignment");
		parser.add_argument("--min-overlap").default_value(10.0).scan<'g', double>().help("minimum overlapping duration in seconds required for a correlation score to be considered valid");
		parser.add_argument("--min-overlap-fraction").default_value(0.8).scan<'g', double>().help("minimum fraction of the shorter signal that a candidate alignment must cover");
		parser.add_argument("--shortlist").default_value(60u).scan<'u', uint32_t>().help("correlate each target at full resolution against only this many best candidates from a cheap coarse pass; 0 disables and correlates against all");
		// Off by default: measured on the whole game, this does far more harm than good.
		// The original theory -- a source scoring decently against many distinct targets
		// must be a generic/ambient recording, not a real match for any of them -- does
		// not hold. Several "Trail to the Heavens" tracks are legitimate compilations that
		// correctly explain 50-170 short targets each (album medley tracks, same idea as
		// the credits-medley case segments already handle in the other direction); turning
		// this on at threshold 4 dropped matched count 1116 -> 728. Kept only as an opt-in
		// escape hatch; do not enable by default without a smarter signal (e.g. whether the
		// matched region actually differs between targets, not just how many targets match).
		parser.add_argument("--magnet-threshold").default_value(0u).scan<'u', uint32_t>().help("a source scoring >= --min-score against more than this many distinct targets is dropped from every target's candidate list before the match/ambiguous decision. 0 (default) disables this filter -- see the comment above this flag before raising it");
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
		const auto presetSpec = parser.present<std::string>("--preset").value_or(std::string());
		const auto excludeSpec = parser.get<std::string>("--exclude-preset");
		const auto targetPrefix = parser.get<std::string>("--target-prefix");
		const auto discover = parser.get<bool>("--discover");
		const auto outputPath = std::filesystem::path(xivres::util::unicode::convert<std::wstring>(parser.get<std::string>("--output")));
		const auto ffmpegPath = std::filesystem::path(xivres::util::unicode::convert<std::wstring>(parser.get<std::string>("--ffmpeg")));
		const auto ffprobePath = std::filesystem::path(xivres::util::unicode::convert<std::wstring>(parser.get<std::string>("--ffprobe")));
		const auto minScore = parser.get<double>("--min-score");
		const auto minMargin = parser.get<double>("--min-margin");
		const auto maxDurationDiff = parser.get<double>("--max-duration-diff");
		const auto maxOffset = parser.get<double>("--max-offset");
		const auto minOverlapSeconds = parser.get<double>("--min-overlap");
		const auto minOverlapFraction = parser.get<double>("--min-overlap-fraction");
		const auto shortlistSize = parser.get<uint32_t>("--shortlist");
		const auto duplicateThreshold = parser.get<double>("--duplicate-threshold");
		const auto segmentMinScore = parser.get<double>("--segment-min-score");
		const auto magnetThreshold = parser.get<uint32_t>("--magnet-threshold");

		const auto gameRoot = resolve_game_root(gameSpec);
		const xivres::installation installation(gameRoot);

		const auto loadPreset = [](const std::filesystem::path& path) {
			std::ifstream f(path, std::ios::binary);
			if (!f)
				throw std::runtime_error(std::format("Could not open preset file: {}", u8(path)));
			nlohmann::json res;
			f >> res;
			if (!res.contains("items") || !res["items"].is_array())
				throw std::runtime_error(std::format("Preset file has no \"items\" array: {}", u8(path)));
			return res;
		};

		nlohmann::json preset = presetSpec.empty()
			? nlohmann::json{{"name", ""}, {"items", nlohmann::json::array()}}
			: loadPreset(std::filesystem::path(xivres::util::unicode::convert<std::wstring>(presetSpec)));

		// Targets already described by other presets, so that --discover only reports
		// BGM that nothing covers yet.
		std::set<std::string> coveredPaths;
		for (size_t begin = 0, end; begin <= excludeSpec.size(); begin = end + 1) {
			end = excludeSpec.find(',', begin);
			if (end == std::string::npos)
				end = excludeSpec.size();
			const auto part = excludeSpec.substr(begin, end - begin);
			if (part.empty())
				continue;
			for (const auto& item : loadPreset(std::filesystem::path(xivres::util::unicode::convert<std::wstring>(part))).at("items"))
				for (auto& p : preset_target_paths(item))
					coveredPaths.insert(std::move(p));
		}

		if (discover) {
			const auto split = [](const std::string& s) {
				std::vector<std::string> res;
				for (size_t begin = 0, end; begin <= s.size(); begin = end + 1) {
					end = s.find(',', begin);
					if (end == std::string::npos)
						end = s.size();
					if (auto part = s.substr(begin, end - begin); !part.empty())
						res.push_back(std::move(part));
				}
				return res;
			};
			const auto matchesAny = [](const std::vector<std::string>& prefixes, const std::string& path) {
				return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& p) { return path.starts_with(p); });
			};

			const auto includePrefixes = split(targetPrefix);
			const auto excludePrefixes = split(parser.get<std::string>("--target-exclude-prefix"));

			size_t skippedAsCovered = 0, skippedByPrefix = 0;
			for (auto& path : enumerate_bgm_paths(installation)) {
				if (!includePrefixes.empty() && !matchesAny(includePrefixes, path)) {
					skippedByPrefix++;
					continue;
				}
				if (matchesAny(excludePrefixes, path)) {
					skippedByPrefix++;
					continue;
				}
				if (coveredPaths.contains(path)) {
					skippedAsCovered++;
					continue;
				}
				preset["items"].push_back({{"target", {{"path", path}}}});
			}
			std::cerr << std::format("Discovered {} BGM target(s) from the game ({} excluded as already covered, {} filtered out by prefix).",
				preset["items"].size(), skippedAsCovered, skippedByPrefix) << std::endl;
		}

		if (preset["items"].empty())
			throw std::runtime_error("No target items to match.");

		std::vector<candidate> candidates;
		static const std::set<std::wstring> exts = {L".flac", L".ogg", L".wav", L".mp3", L".m4a"};
		// follow_directory_symlink so an OST directory assembled out of links to the real
		// album folders works; by default reparse points are skipped and the scan finds
		// nothing at all, which looks exactly like "no matches".
		for (const auto& entry : std::filesystem::recursive_directory_iterator(ostDir, std::filesystem::directory_options::follow_directory_symlink | std::filesystem::directory_options::skip_permission_denied)) {
			if (!entry.is_regular_file())
				continue;
			const auto ext = xivres::util::unicode::convert<std::wstring>(entry.path().extension().wstring(), &xivres::util::unicode::lower);
			if (!exts.contains(ext))
				continue;

			candidate c;
			c.Path = entry.path();
			// lexically_relative, not relative(): the latter canonicalises, which resolves
			// any junction in the path to its real target outside the OST directory and
			// yields an empty result, so every source key silently came out blank.
			c.RelName = u8(entry.path().lexically_relative(ostDir).generic_wstring());
			candidates.push_back(std::move(c));
		}

		// An OST folder often carries the same track twice -- typically a lossless rip
		// next to the disc's companion MP3DATA folder. Both copies describe one
		// performance, so they score almost identically and the weaker copy always looks
		// like a near-tie, which makes the margin check reject an otherwise good match.
		// Keep only the best-encoded copy of each track name.
		{
			const auto rank = [](const std::filesystem::path& p) {
				auto ext = xivres::util::unicode::convert<std::wstring>(p.extension().wstring(), &xivres::util::unicode::lower);
				if (ext == L".flac" || ext == L".wav")
					return 0;  // lossless
				if (ext == L".ogg" || ext == L".m4a")
					return 1;
				return 2;      // mp3
			};
			const auto stemOf = [](const std::filesystem::path& p) {
				return xivres::util::unicode::convert<std::wstring>(p.stem().wstring(), &xivres::util::unicode::lower);
			};

			// A stem with any lossless copy drops its lossy copies, which are redundant
			// encodings of the same performance. When every copy is lossy the stem is left
			// alone: those are more likely to be different albums that happen to share a
			// track numbering, and merging them would lose real candidates.
			std::map<std::wstring, std::vector<size_t>> byStem;
			for (size_t i = 0; i < candidates.size(); ++i)
				byStem[stemOf(candidates[i].Path)].push_back(i);

			std::vector<size_t> keep;
			keep.reserve(candidates.size());
			for (const auto& entry : byStem) {
				const auto& group = entry.second;
				const bool hasLossless = std::any_of(group.begin(), group.end(),
					[&](size_t i) { return rank(candidates[i].Path) == 0; });
				for (const auto index : group)
					if (!hasLossless || rank(candidates[index].Path) == 0)
						keep.push_back(index);
			}
			std::sort(keep.begin(), keep.end());

			if (keep.size() != candidates.size()) {
				std::vector<candidate> deduped;
				deduped.reserve(keep.size());
				for (const auto index : keep)
					deduped.push_back(std::move(candidates[index]));
				std::cerr << std::format("Ignoring {} duplicate-encoding candidate(s) of the same track.", candidates.size() - deduped.size()) << std::endl;
				candidates = std::move(deduped);
			}
		}

		std::cerr << std::format("Found {} candidate OST file(s). Decoding...", candidates.size()) << std::endl;

		parallel_for(candidates.size(), [&](size_t i) {
			auto& c = candidates[i];
			c.Envelope = decode_envelope(ffmpegPath, c.Path);
			c.DurationSeconds = static_cast<double>(c.Envelope.size()) / EnvelopeRateHz;
			{
				auto titleInfo = read_title_info(ffprobePath, c.Path);
				c.NormalizedTitle = std::move(titleInfo.Normalized);
				c.RawTitle = std::move(titleInfo.Raw);
				c.EnglishTitle = std::move(titleInfo.English);
				c.JapaneseTitle = std::move(titleInfo.Japanese);
			}
		});
		std::erase_if(candidates, [](const candidate& c) { return c.Envelope.empty(); });
		std::cerr << std::format("{} candidate(s) ready for matching.", candidates.size()) << std::endl;

		// Coarse copies of every envelope, used to shortlist candidates cheaply before the
		// full-resolution pass. Correlating 200 Hz envelopes of whole tracks costs a
		// 256k-point FFT per pair, which is fine for one album but not when every BGM in
		// the game is matched against every track of every album; at 10 Hz the same pass
		// is thousands of times cheaper and still ranks the right track near the top.
		constexpr size_t CoarseFactor = 20;
		const auto coarseRateHz = EnvelopeRateHz / static_cast<double>(CoarseFactor);
		std::vector<std::vector<float>> coarseEnvelopes;
		if (shortlistSize) {
			const auto decimate = [](const std::vector<float>& env) {
				const auto count = env.size() / CoarseFactor;
				std::vector<float> res(count);
				for (size_t i = 0; i < count; ++i) {
					double sum = 0;
					for (size_t j = 0; j < CoarseFactor; ++j)
						sum += env[i * CoarseFactor + j];
					res[i] = static_cast<float>(sum / CoarseFactor);
				}
				return res;
			};
			coarseEnvelopes.reserve(candidates.size());
			for (const auto& c : candidates)
				coarseEnvelopes.push_back(decimate(c.Envelope));
		}

		size_t matchedCount = 0, ambiguousCount = 0, unmatchedCount = 0, skippedCount = 0;
		std::mutex progressMutex;
		const auto tempDir = std::filesystem::temp_directory_path();
		std::atomic<uint32_t> tempFileCounter{0};

		// Pick the items to resolve first, so the expensive per-target work below can be
		// spread across threads. Each task writes only its own item, so the array must not
		// be resized while they run.
		std::vector<size_t> workItems;
		for (size_t index = 0; index < preset.at("items").size(); ++index) {
			const auto& item = preset.at("items")[index];

			// Explicitly disabled entries (including ones with placeholder target paths)
			// are deliberate: leave them exactly as they are.
			if (!preset_target_enabled(item))
				continue;

			if (preset_target_paths(item).empty())
				continue;

			const bool hasSource = item.contains("source") && !item["source"].is_null()
				&& !(item["source"].is_string() && item["source"].get<std::string>().empty());
			if (hasSource) {
				skippedCount++;
				continue;
			}
			workItems.push_back(index);
		}

		if (!workItems.empty())
			std::cerr << std::format("Matching {} target(s) against {} candidate(s)...", workItems.size(), candidates.size()) << std::endl;

		// Scores one envelope against the candidate pool. Shared by the ordinary path and
		// the per-stem path used for the game's multi-channel stem containers.
		struct scored {
			std::string Name;
			double Score;
			double OffsetSeconds;
			double OverlapSeconds;
			size_t CandidateIndex;
		};
		struct resolution {
			std::vector<scored> Scores;
			std::vector<bool> DuplicateOfWinner;
			size_t DistinctRunnerUp = 0;
		};

		// Finds the best genuinely-different runner-up in an already-sorted score list,
		// skipping other releases of the same recording as the winner (see below). Split
		// out so the magnet-filtering pass can re-run it against a winner other than
		// scores[0] once magnet candidates have been dropped from the list.
		const auto computeDuplicatesAndRunnerUp = [&](const std::vector<scored>& scores, double minOverlap) {
			std::vector<bool> duplicateOfWinner(scores.size(), false);
			size_t distinctRunnerUp = scores.size();
			if (!scores.empty()) {
				for (size_t i = 1; i < scores.size(); ++i) {
					// A shared, non-empty title tag is a duplicate release of the winner
					// regardless of how the two masterings correlate -- titles are reliably
					// unique per piece, but two legitimate re-releases of the same piece can
					// correlate below any fixed audio threshold (measured case: 0.935,
					// under the 0.95 the audio check below uses). Cheaper too: no decode.
					const auto& winnerTitle = candidates[scores[0].CandidateIndex].NormalizedTitle;
					const auto& candidateTitle = candidates[scores[i].CandidateIndex].NormalizedTitle;
					if (!winnerTitle.empty() && winnerTitle == candidateTitle) {
						duplicateOfWinner[i] = true;
						continue;
					}

					// Two releases of one recording can carry a very different amount of
					// lead-in silence (remaster padding, album crossfades) -- easily more
					// than a few seconds. Use the same search window as the real match, or
					// genuine duplicates with a long lead-in difference are missed and the
					// pair is scored as a false ambiguity instead of being skipped.
					const auto mutual = best_envelope_correlation_ex(
						candidates[scores[0].CandidateIndex].Envelope,
						candidates[scores[i].CandidateIndex].Envelope,
						EnvelopeRateHz, maxOffset, minOverlap, 0.5).Score;
					if (mutual >= duplicateThreshold) {
						duplicateOfWinner[i] = true;
						continue;
					}
					distinctRunnerUp = i;
					break;
				}
			}
			return std::make_pair(std::move(duplicateOfWinner), distinctRunnerUp);
		};

		const auto resolveEnvelope = [&](const std::vector<float>& envelope, double duration) {
			resolution res;
			const auto minOverlap = std::min(minOverlapSeconds, duration * 0.5);

			std::vector<float> coarseEnvelope;
			if (shortlistSize) {
				const auto count = envelope.size() / CoarseFactor;
				coarseEnvelope.resize(count);
				for (size_t i = 0; i < count; ++i) {
					double sum = 0;
					for (size_t j = 0; j < CoarseFactor; ++j)
						sum += envelope[i * CoarseFactor + j];
					coarseEnvelope[i] = static_cast<float>(sum / CoarseFactor);
				}
			}

			// Stage 1, coarse: rank every duration-compatible candidate cheaply.
			std::vector<size_t> shortlist;
			{
				std::vector<std::pair<double, size_t>> ranked;
				for (size_t i = 0; i < candidates.size(); ++i) {
					// Disabled by default. This used to prune candidates before scoring, back
					// when every pair cost a 256k-point FFT; the shortlist pass does that job
					// now, and the filter actively hurts: a credits medley runs to 38 minutes
					// and is built from ordinary-length album tracks, so any candidate more
					// than the tolerance shorter than the target was dropped before it could
					// be recognised as one of the medley's segments.
					if (maxDurationDiff > 0. && std::abs(candidates[i].DurationSeconds - duration) > maxDurationDiff)
						continue;
					if (!shortlistSize) {
						shortlist.push_back(i);
						continue;
					}
					// Deliberately lenient here: this pass only decides what is worth a
					// closer look, so it must not apply the strict overlap rule that the
					// real scoring uses, or it would drop good candidates before scoring.
					const auto r = best_envelope_correlation(coarseEnvelope, coarseEnvelopes[i], coarseRateHz, maxOffset, minOverlap, 0.5);
					ranked.emplace_back(r, i);
				}
				if (shortlistSize && ranked.size() > shortlistSize) {
					std::partial_sort(ranked.begin(), ranked.begin() + shortlistSize, ranked.end(),
						[](const auto& a, const auto& b) { return a.first > b.first; });
					ranked.resize(shortlistSize);
				}
				for (const auto& [score, index] : ranked)
					shortlist.push_back(index);
			}

			// Stage 2, full resolution: score only the shortlist.
			for (const auto index : shortlist) {
				const auto& c = candidates[index];
				const auto r = best_envelope_correlation_ex(envelope, c.Envelope, EnvelopeRateHz, maxOffset, minOverlap, minOverlapFraction);
				if (r.Score > -1.5)
					res.Scores.push_back({c.RelName, r.Score, r.OffsetSeconds, r.OverlapSeconds, index});
			}
			auto& scores = res.Scores;
			std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) { return a.Score > b.Score; });

			// The same recording is usually released on several albums, so the runner-up is
			// often not a competing hypothesis but the identical audio under another name.
			// Comparing against it rejects a perfect match as "ambiguous" purely because two
			// copies exist -- which is what kept ~90 targets with scores above 0.80 out of
			// the results. Margin must be measured against the best genuinely different
			// candidate instead, so duplicates of the winner are skipped.
			std::tie(res.DuplicateOfWinner, res.DistinctRunnerUp) = computeDuplicatesAndRunnerUp(scores, minOverlap);
			return res;
		};

		// Filled in by the main parallel_for below for every plain (non-stem) target that
		// found at least one candidate; the match/ambiguous decision for those is deferred
		// to a later pass, once every target has been scored, so a magnet source (see
		// isMagnet below) can be identified from its effect across the whole run and
		// dropped before any individual target commits to a winner.
		std::vector<resolution> pendingMain(workItems.size());
		std::vector<double> pendingMinOverlap(workItems.size());
		std::vector<nlohmann::json> pendingCandidatesJson(workItems.size());
		std::vector<nlohmann::json> pendingSegmentsJson(workItems.size());
		std::vector<bool> needsPhase3(workItems.size(), false);

		// Filled in for every plain (mono/stereo) target that decoded successfully, so a
		// later pass can find targets that are verbatim reuses of another target's audio
		// (a shared boss/event stinger reused across zones/patches under a different BGM
		// slot name -- see [[project-magnet-source-theory-disproven]]) and let a confident
		// match on one propagate to the rest instead of asking each alias to independently
		// clear the score/margin bar. Multi-channel stem targets are excluded: they decide
		// per-stem, not against this single envelope, so linking them would not make sense.
		std::vector<std::vector<float>> dupTargetEnvelope(workItems.size());
		std::vector<double> dupTargetDuration(workItems.size(), 0.0);
		std::vector<bool> dupTargetEligible(workItems.size(), false);

		parallel_for(workItems.size(), [&](size_t workIndex) {
			auto& item = preset.at("items")[workItems[workIndex]];
			const auto targetPaths = preset_target_paths(item);

			std::filesystem::path targetAudio;
			std::vector<float> targetEnvelope;
			double targetDuration = 0;
			size_t targetChannels = 0;
			try {
				targetAudio = extract_scd_audio_to_temp(installation, targetPaths.front(), tempDir, tempFileCounter.fetch_add(1), &targetChannels);
				{
					const auto lock = std::lock_guard(progressMutex);
					tempFiles.push_back(targetAudio);
				}
				targetEnvelope = decode_envelope(ffmpegPath, targetAudio);
				targetDuration = static_cast<double>(targetEnvelope.size()) / EnvelopeRateHz;
				if (targetChannels <= 2) {
					dupTargetEnvelope[workIndex] = targetEnvelope;
					dupTargetDuration[workIndex] = targetDuration;
					dupTargetEligible[workIndex] = true;
				}
			} catch (const std::exception& e) {
				const auto lock = std::lock_guard(progressMutex);
				std::cerr << std::format("Warning: could not decode target {}: {}", targetPaths.front(), e.what()) << std::endl;
				item["matchInfo"] = {{"status", "error"}, {"error", e.what()}};
				item["enable"] = false;
				unmatchedCount++;
				return;
			}

			const auto resolved = resolveEnvelope(targetEnvelope, targetDuration);
			const auto& scores = resolved.Scores;
			const auto& isDuplicateOfWinner = resolved.DuplicateOfWinner;
			const auto distinctRunnerUp = resolved.DistinctRunnerUp;

			nlohmann::json candidatesJson = nlohmann::json::array();
			for (size_t i = 0; i < std::min<size_t>(5, scores.size()); i++) {
				auto entry = nlohmann::json{
					{"source", scores[i].Name},
					{"score", scores[i].Score},
					{"offset", scores[i].OffsetSeconds},
					{"overlap", scores[i].OverlapSeconds},
				};
				if (const auto& rawTitle = candidates[scores[i].CandidateIndex].RawTitle; !rawTitle.empty())
					entry["sourceTitle"] = rawTitle;
				if (isDuplicateOfWinner[i])
					entry["duplicateOfBest"] = true;
				candidatesJson.push_back(std::move(entry));
			}

			// Decompose the target into the distinct pieces it is built from.
			//
			// A credits medley runs for tens of minutes and is assembled from ordinary
			// album tracks, so no single candidate explains it and whichever one scores
			// best is only its largest component. Each scored candidate carries the region
			// of the target it matched, so components can be told apart from near-duplicate
			// explanations of the same passage by whether their regions overlap.
			nlohmann::json segmentsJson = nlohmann::json::array();
			if (segmentMinScore > 0.) {
				std::vector<std::pair<double, double>> keptRegions;
				for (const auto& s : scores) {
					if (s.Score < segmentMinScore)
						break;
					const auto begin = (std::max)(0., s.OffsetSeconds);
					const auto end = begin + s.OverlapSeconds;
					const bool overlapsKept = std::any_of(keptRegions.begin(), keptRegions.end(), [&](const auto& kept) {
						const auto overlap = (std::min)(end, kept.second) - (std::max)(begin, kept.first);
						return overlap > 0.5 * (std::min)(end - begin, kept.second - kept.first);
					});
					if (overlapsKept)
						continue;
					keptRegions.emplace_back(begin, end);
					segmentsJson.push_back({
						{"source", s.Name},
						{"score", s.Score},
						{"startSeconds", begin},
						{"endSeconds", end},
						{"durationSeconds", end - begin},
					});
					if (segmentsJson.size() >= 16)
						break;
				}
			}

			// Multi-channel entries are scored stem by stem rather than as one signal; see
			// extract_stem_to_temp for why the downmix cannot work.
			nlohmann::json stemsJson = nlohmann::json::array();
			bool stemsAllConfident = false;
			if (targetChannels > 2 && targetChannels % 2 == 0 && segmentMinScore > 0.) {
				stemsAllConfident = true;
				std::vector<size_t> stemSources;  // candidate indices, one per confidently-matched stem
				const auto channelPairs = discover_channel_pairing(ffmpegPath, targetAudio, targetChannels, tempDir, tempFileCounter, tempFiles, progressMutex);
				for (const auto& [channelA, channelB] : channelPairs) {
					try {
						const auto stemPath = extract_stem_to_temp(ffmpegPath, targetAudio, channelA, channelB, tempDir, tempFileCounter.fetch_add(1));
						{
							const auto lock = std::lock_guard(progressMutex);
							tempFiles.push_back(stemPath);
						}
						const auto stemEnvelope = decode_envelope(ffmpegPath, stemPath);
						const auto stemResolved = resolveEnvelope(stemEnvelope, static_cast<double>(stemEnvelope.size()) / EnvelopeRateHz);

						nlohmann::json stemEntry{
							{"channels", nlohmann::json::array({channelA, channelB})},
							{"status", "unmatched"},
						};
						if (!stemResolved.Scores.empty()) {
							const auto& best = stemResolved.Scores[0];
							const auto confident = best.Score >= minScore
								&& (stemResolved.DistinctRunnerUp == stemResolved.Scores.size()
									|| best.Score - stemResolved.Scores[stemResolved.DistinctRunnerUp].Score >= minMargin);
							stemEntry["source"] = best.Name;
							stemEntry["score"] = best.Score;
							stemEntry["offset"] = best.OffsetSeconds;
							stemEntry["status"] = confident ? "matched" : "ambiguous";
							if (confident)
								stemSources.push_back(best.CandidateIndex);
							else
								stemsAllConfident = false;
						} else {
							stemsAllConfident = false;
						}
						stemsJson.push_back(std::move(stemEntry));
					} catch (const std::exception&) {
						// One unusable stem must not lose the others' findings.
						stemsAllConfident = false;
					}
				}
				if (!stemSources.empty()) {
					// The preset schema names sources per channel, so record every stem's
					// source rather than only one. Each stem's key is followed by its
					// English/Japanese title, if known, the same way the plain (non-stem)
					// path below does -- extra search-key variants the old importer's
					// preset convention already uses (see e.g. Shadowbringers.json).
					auto keys = nlohmann::json::array();
					for (const auto candidateIndex : stemSources) {
						const auto& c = candidates[candidateIndex];
						auto key = xivres::util::unicode::convert<std::string>(
							std::filesystem::path(xivres::util::unicode::convert<std::wstring>(c.RelName)).stem().wstring());
						keys.push_back(std::move(key));
						if (!c.EnglishTitle.empty())
							keys.push_back(c.EnglishTitle);
						if (!c.JapaneseTitle.empty())
							keys.push_back(c.JapaneseTitle);
					}
					// Kept for the branch below, which runs under the lock.
					item["source"] = std::move(keys);
				}
			}

			const auto lock = std::lock_guard(progressMutex);
			if (targetChannels > 2 && targetChannels % 2 == 0 && segmentMinScore > 0.) {
				item["matchInfo"] = {
					{"status", stemsAllConfident ? "matched" : "ambiguous"},
					{"stems", std::move(stemsJson)},
					{"candidates", candidatesJson},
				};
				if (stemsAllConfident) {
					matchedCount++;
				} else {
					item["enable"] = false;
					ambiguousCount++;
				}
			} else if (scores.empty()) {
				item["matchInfo"] = {{"status", "unmatched"}};
				item["enable"] = false;
				unmatchedCount++;
			} else {
				// Deferred to the phase-3 pass below, after magnet sources are known.
				pendingMain[workIndex] = resolved;
				pendingMinOverlap[workIndex] = std::min(minOverlapSeconds, targetDuration * 0.5);
				pendingCandidatesJson[workIndex] = std::move(candidatesJson);
				pendingSegmentsJson[workIndex] = std::move(segmentsJson);
				needsPhase3[workIndex] = true;
			}
		});

		// Phase 2: a source that scores >= minScore against an implausible number of
		// distinct targets is not really matching any of them -- it is a generic/ambient
		// recording (sustained pad, drone, rainfall) that superficially correlates with
		// almost anything quiet. A real OST track matches at most a handful of targets
		// (occasionally a couple more when the same recording ships on two discs), so
		// counting appearances across the whole run catches what a single target's score
		// and margin cannot: e.g. "Before_Meteor_FFXIV_035" turning up as a top-3 candidate
		// for a dozen unrelated ambient/event targets, each near-tying a different real
		// source and forcing it into "ambiguous".
		std::vector<uint32_t> appearanceCount(candidates.size(), 0);
		if (magnetThreshold) {
			for (size_t wi = 0; wi < workItems.size(); ++wi) {
				if (!needsPhase3[wi])
					continue;
				for (const auto& s : pendingMain[wi].Scores)
					if (s.Score >= minScore)
						appearanceCount[s.CandidateIndex]++;
			}
		}
		std::vector<bool> isMagnet(candidates.size(), false);
		if (magnetThreshold) {
			for (size_t i = 0; i < candidates.size(); ++i) {
				if (appearanceCount[i] > magnetThreshold) {
					isMagnet[i] = true;
					std::cerr << std::format("Magnet source, dropped from candidate lists (scored >= {} against {} distinct targets): {}",
						minScore, appearanceCount[i], candidates[i].RelName) << std::endl;
				}
			}
		}

		// Phase 3: finalize every deferred target now that magnet sources are known, using
		// the same decision logic as before but with magnet candidates removed from the
		// ranked list first -- so a real match that happened to near-tie a magnet is no
		// longer held to "ambiguous" by it, and a target whose only candidates were magnets
		// correctly falls through to "unmatched" instead of a false match.
		parallel_for(workItems.size(), [&](size_t workIndex) {
			if (!needsPhase3[workIndex])
				return;
			auto& item = preset.at("items")[workItems[workIndex]];

			auto scores = pendingMain[workIndex].Scores;
			if (magnetThreshold) {
				std::vector<scored> filtered;
				filtered.reserve(scores.size());
				for (auto& s : scores)
					if (!isMagnet[s.CandidateIndex])
						filtered.push_back(s);
				scores = std::move(filtered);
			}

			const auto lock = std::lock_guard(progressMutex);
			if (scores.empty()) {
				item["matchInfo"] = {{"status", "unmatched"}};
				item["enable"] = false;
				unmatchedCount++;
				return;
			}

			const auto distinctRunnerUp = computeDuplicatesAndRunnerUp(scores, pendingMinOverlap[workIndex]).second;
			const bool confident = scores[0].Score >= minScore
				&& (distinctRunnerUp == scores.size() || scores[0].Score - scores[distinctRunnerUp].Score >= minMargin);
			if (confident) {
				// `source` is a list of search keys matched against the OST directory
				// (the importer treats each entry as a pattern), not a path. The file
				// stem is the key; the exact file it resolved to is kept in matchInfo
				// as "file", so a track found in a subfolder stays traceable. English/
				// Japanese title, if known, follow as extra search-key variants -- the
				// same convention the hand-written presets already use (e.g. the
				// Shadowbringers.json entry for "'Neath Dark Waters" also lists its
				// Japanese title and old track number as alternate keys).
				const auto& winnerCandidate = candidates[scores[0].CandidateIndex];
				const auto key = xivres::util::unicode::convert<std::string>(
					std::filesystem::path(xivres::util::unicode::convert<std::wstring>(scores[0].Name)).stem().wstring());
				item["source"] = nlohmann::json::array({key});
				if (!winnerCandidate.EnglishTitle.empty())
					item["source"].push_back(winnerCandidate.EnglishTitle);
				if (!winnerCandidate.JapaneseTitle.empty())
					item["source"].push_back(winnerCandidate.JapaneseTitle);
				item["matchInfo"] = {
					{"status", "matched"},
					{"score", scores[0].Score},
					{"file", scores[0].Name},
					{"offset", scores[0].OffsetSeconds},
					{"candidates", pendingCandidatesJson[workIndex]},
					{"segments", std::move(pendingSegmentsJson[workIndex])},
				};
				matchedCount++;
			} else {
				item["matchInfo"] = {{"status", "ambiguous"}, {"candidates", pendingCandidatesJson[workIndex]}};
				item["enable"] = false;
				ambiguousCount++;
			}
		});

		// Phase 4: link targets whose decoded audio is (near-)identical -- a shared
		// boss/event stinger reused verbatim under a different BGM slot name -- and
		// propagate a confident match from one member of the group to the rest. An alias
		// with no OST content of its own can never clear the score/margin bar on its own
		// merits, no matter how the thresholds are tuned; it needs its sibling's answer.
		{
			std::vector<size_t> parent(workItems.size());
			for (size_t i = 0; i < parent.size(); ++i)
				parent[i] = i;
			std::function<size_t(size_t)> find = [&](size_t x) {
				while (parent[x] != x)
					x = parent[x] = parent[parent[x]];
				return x;
			};
			const auto unite = [&](size_t a, size_t b) {
				a = find(a);
				b = find(b);
				if (a != b)
					parent[a] = b;
			};

			// Bucket by rounded duration first -- identical audio decodes to (near-)
			// identical duration -- so the pairwise correlation below only ever runs
			// within a small bucket instead of across all eligible targets.
			std::unordered_map<int64_t, std::vector<size_t>> byDuration;
			for (size_t wi = 0; wi < workItems.size(); ++wi)
				if (dupTargetEligible[wi])
					byDuration[std::llround(dupTargetDuration[wi] * 20.0)].push_back(wi);

			for (const auto& [bucket, members] : byDuration) {
				for (size_t a = 0; a < members.size(); ++a) {
					for (size_t b = a + 1; b < members.size(); ++b) {
						const auto wa = members[a], wb = members[b];
						const auto minOverlap = std::min(dupTargetDuration[wa], dupTargetDuration[wb]) * 0.95;
						const auto r = best_envelope_correlation_ex(dupTargetEnvelope[wa], dupTargetEnvelope[wb], EnvelopeRateHz, 2.0, minOverlap, 0.95);
						if (r.Score >= 0.995 && std::abs(r.OffsetSeconds) <= 0.5)
							unite(wa, wb);
					}
				}
			}

			std::unordered_map<size_t, std::vector<size_t>> groups;
			for (size_t wi = 0; wi < workItems.size(); ++wi)
				if (dupTargetEligible[wi])
					groups[find(wi)].push_back(wi);

			for (const auto& [root, members] : groups) {
				if (members.size() < 2)
					continue;

				{
					const auto lock = std::lock_guard(progressMutex);
					std::cerr << "Duplicate-target group:";
					for (const auto wi : members)
						std::cerr << " " << preset_target_paths(preset.at("items")[workItems[wi]]).front();
					std::cerr << std::endl;
				}

				size_t bestMember = SIZE_MAX;
				double bestScore = -2.0;
				for (const auto wi : members) {
					const auto& mi = preset.at("items")[workItems[wi]]["matchInfo"];
					if (mi.value("status", "") == "matched" && mi.value("score", -2.0) > bestScore) {
						bestScore = mi.value("score", -2.0);
						bestMember = wi;
					}
				}
				if (bestMember == SIZE_MAX)
					continue; // no confident answer anywhere in this group (yet)

				const auto& sourceItem = preset.at("items")[workItems[bestMember]];
				const auto linkedFrom = preset_target_paths(sourceItem).front();
				for (const auto wi : members) {
					if (wi == bestMember)
						continue;
					auto& item = preset.at("items")[workItems[wi]];
					auto& mi = item["matchInfo"];
					const auto status = mi.value("status", "");
					if (status == "matched")
						continue; // independently matched already (possibly a different, equally genuine duplicate release) -- don't override
					if (status == "ambiguous")
						ambiguousCount--;
					else if (status == "unmatched")
						unmatchedCount--;
					item["source"] = sourceItem["source"];
					item["matchInfo"] = {
						{"status", "matched"},
						{"score", sourceItem["matchInfo"]["score"]},
						{"file", sourceItem["matchInfo"]["file"]},
						{"offset", sourceItem["matchInfo"]["offset"]},
						{"linkedFrom", linkedFrom},
					};
					item.erase("enable");
					matchedCount++;
				}
			}
		}

		if (parser.get<bool>("--matched-only")) {
			nlohmann::json kept = nlohmann::json::array();
			for (auto& item : preset.at("items"))
				if (const auto it = item.find("matchInfo"); it != item.end() && it->value("status", "") == "matched")
					kept.push_back(std::move(item));
			preset["items"] = std::move(kept);
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
