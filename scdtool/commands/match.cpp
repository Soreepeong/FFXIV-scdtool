#include "pch.h"
#include "match.h"

#include "utils/argactions.h"
#include "utils/audio_match.h"
#include "utils/describe_path.h"
#include "utils/hca_payload.h"
#include "utils/misc.h"
#include "utils/win32_process.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <ranges>
#include <set>
#include <string_view>
#include <unordered_map>

#include <xivres/excel.h>

namespace {
	// Matches audio_match.h's decode_envelope default (16000 Hz PCM / 80-sample hop).
	constexpr double EnvelopeRateHz = 200.;

	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}
	// Game installation specs are resolved by argactions::installation_root.

	// Extracts a game .scd's first sound entry to a temp file so it can go through
	// the same ffmpeg decode_envelope() path as OST candidate files, rather than
	// needing a second, native decode path just for the "target" side of a match.
	std::filesystem::path extract_scd_audio_to_temp(const xivres::installation& installation, const std::string& relativePath, const std::filesystem::path& tempDir, uint32_t uniqueId, size_t* channels = nullptr, bool* stemStream = nullptr) {
		const auto stream = installation.get_file(relativePath);
		const xivres::sound::reader reader(stream);
		if (reader.sound_item_count() == 0)
			throw std::runtime_error("no sound entries");

		// Take the first entry that actually carries audio in a format we can hand to
		// ffmpeg; entry 0 is empty in some of the game's files.
		std::vector<uint8_t> bytes;
		const wchar_t* ext = nullptr;
		size_t foundChannels = 0;
		bool foundStemStream = false;
		for (size_t i = 0; i < reader.sound_item_count(); ++i) {
			const auto item = reader.read_sound_item(i);
			if (item.Header->Format == xivres::sound::sound_entry_format::Ogg) {
				bytes = item.get_ogg_file();
				ext = L".ogg";
			} else if (hca_payload::is_hca(item)) {
				bytes = hca_payload::payload_file(item);
				ext = L".hca";
			} else if (item.Header->Format == xivres::sound::sound_entry_format::WaveFormatPcm) {
				bytes = item.get_wav_file();
				ext = L".wav";
			} else {
				continue;
			}
			if (!bytes.empty()) {
				foundChannels = static_cast<uint32_t>(item.Header->ChannelCount);
				// What the file says it is, rather than what its channel count suggests.
				// A 4- or 6-channel music entry is DynamixStream -- stems the engine switches
				// between -- and a genuine surround mix would say FourChannelSurround.
				// Measured across the game's 2175 music files: every multichannel entry is
				// DynamixStream and every mono or stereo one is Normal.
				if (const auto descriptor = reader.read_sound_descriptor(i))
					foundStemStream = descriptor->Type == xivres::sound::sound_type::DynamixStream;
				break;
			}
		}
		if (!ext || bytes.empty())
			throw std::runtime_error("no usable sound entry (expected a non-empty Ogg or PCM wave entry)");
		if (channels)
			*channels = foundChannels;
		if (stemStream)
			*stemStream = foundStemStream;

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
				const auto lock = std::scoped_lock(tempFilesMutex);
				tempFiles.push_back(chPath);
			}
			channelEnvelopes[ch] = decode_envelope(ffmpegPath, chPath);
		}

		// Zero-lag-ish correlation: these are channels of the same stream, so any real
		// pair is already sample-aligned; a small offset budget just absorbs decode jitter.
		std::vector pairScore(channelCount, std::vector(channelCount, -1.));
		for (size_t a = 0; a < channelCount; ++a)
			for (size_t b = a + 1; b < channelCount; ++b)
				pairScore[a][b] = pairScore[b][a] = best_envelope_correlation(channelEnvelopes[a], channelEnvelopes[b], EnvelopeRateHz, 0.25, 1., 0.99);

		// Brute-force every perfect matching of {0..channelCount-1} into channelCount/2
		// pairs -- at most 15 of them (6 channels), so there is no need for anything
		// cleverer than recursion.
		std::vector<size_t> remaining(channelCount);
		std::ranges::iota(remaining, size_t{0});
		std::vector<std::pair<size_t, size_t>> best;
		double bestScore = -1e9;
		const std::function<void(std::vector<size_t>, std::vector<std::pair<size_t, size_t>>, double)> recurse =
			[&](const std::vector<size_t>& left, const std::vector<std::pair<size_t, size_t>>& chosen, double scoreSoFar) {
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
		std::ranges::sort(best);
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
				std::ranges::transform(lowerKey, lowerKey.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
					part.erase(part.begin(), std::ranges::find_if(part, notSpace));
					part.erase(std::ranges::find_if(part.rbegin(), part.rend(), notSpace).base(), part.end());
					if (!part.empty())
						rawParts.push_back(std::move(part));
					start = i + 1;
				}
			}

			for (const auto& part : rawParts) {
				const bool isAscii = std::ranges::all_of(part, [](unsigned char c) { return c < 0x80; });
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
				std::ranges::transform(part, part.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::ranges::sort(normParts);
			normParts.erase(std::ranges::unique(normParts).begin(), normParts.end());
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
		return std::ranges::any_of(*target, enabled);
	}

	// Adds what the game's sheets say about a target to the target itself, so a preset can be
	// read (and filtered) by place and duty name rather than by path alone. A target is one
	// object or, as the preset schema allows, a list of them.
	void augment_target_names(nlohmann::json& target, const xivres::installation& installation, xivres::game_language language) {
		if (target.is_array()) {
			for (auto& t : target)
				augment_target_names(t, installation, language);
			return;
		}
		if (!target.is_object())
			return;

		std::vector<std::string> paths;
		append_target_paths(paths, target);
		std::set<std::string> names;
		for (const auto& path : paths)
			for (auto& name : describe_path(installation, path, language))
				names.insert(std::move(name));
		target["names"] = std::vector(names.begin(), names.end());
	}

	// Which targets a reader still has to decide for themselves, and what deciding would
	// involve. Taken off the finished items rather than tallied as the decisions were made:
	// phase 4 rewrites some of them afterwards, so a running count describes a state the
	// written file no longer has.
	//
	// The first group is the one worth the reader's time. An ambiguous entry is not a failure
	// to find anything -- it is a candidate the run declined to commit to, so the answer is
	// usually already in its list and one listen settles it. An unmatched one found nothing at
	// all, which normally means the albums searched do not contain that cue.
	void print_attention_summary(const nlohmann::json& preset, size_t limit,
		double minScore, double minMargin, double rerankMinScore, double rerankMinMargin,
		const std::filesystem::path& csvPath) {

		struct entry {
			double Score = -2.;
			std::string Path;
			std::string Source;
			std::string Detail;
			// Carried for the CSV, which is read to decide what a target *is* rather than
			// skimmed for what to listen to next: the game's own names for the file, how long
			// it runs, and the runner-up as its own column so the two can be sorted on.
			std::string Group;
			std::string Names;
			double Seconds = 0.;
			std::string RunnerUp;
			double RunnerUpScore = -2.;
		};
		std::vector<entry> nearMiss, bestGuess, partialStems, nothing, unreadable, switchedOff;
		size_t total = 0, settled = 0;

		const auto hasSource = [](const nlohmann::json& item) {
			const auto it = item.find("source");
			if (it == item.end() || it->is_null())
				return false;
			if (it->is_string())
				return !it->get<std::string>().empty();
			if (it->is_array())
				return !it->empty();
			return true;
		};
		// A candidate is named by its path relative to --ost; the folder is the album, which
		// the preset's own name already says, so only the file tells a reader anything new.
		const auto leaf = [](std::string s) {
			if (const auto slash = s.find_last_of("/\\"); slash != std::string::npos)
				s = s.substr(slash + 1);
			return s;
		};
		static const auto noCandidates = nlohmann::json::array();
		// "TerritoryType:The Tempest", "Orchestrion:A New Hope" -- what the game's own sheets
		// say the file is the music of. For a target nothing ever released a recording of,
		// this is the only thing that says what it is, and the reason the CSV exists.
		const auto namesOf = [](const nlohmann::json& item) {
			std::string out;
			const auto take = [&out](const nlohmann::json& target) {
				const auto names = target.find("names");
				if (names == target.end() || !names->is_array())
					return;
				for (const auto& name : *names)
					if (name.is_string())
						out += (out.empty() ? "" : "; ") + name.get<std::string>();
			};
			if (const auto target = item.find("target"); target != item.end()) {
				if (target->is_array())
					for (const auto& one : *target)
						take(one);
				else
					take(*target);
			}
			return out;
		};

		for (const auto& item : preset.at("items")) {
			const auto paths = preset_target_paths(item);
			if (paths.empty())
				continue;
			total++;
			auto path = paths.front();
			if (paths.size() > 1)
				path += std::format(" +{}", paths.size() - 1);
			const auto names = namesOf(item);
			const auto mi0 = item.find("matchInfo");
			const auto seconds = mi0 != item.end() ? mi0->value("targetSeconds", 0.) : 0.;
			const auto tag = [&](entry e, const char* group) {
				e.Group = group;
				e.Names = names;
				e.Seconds = seconds;
				return e;
			};

			// Two different flags, and they are not interchangeable. A preset excludes an
			// entry deliberately by setting `enable` on the *target*, which is the one this
			// run skips over and the only one `apply` reads; this tool records its own
			// refusals by setting `enable` on the *item*. Read in the wrong order the second
			// swallows the first, and every unmatched target gets reported as a decision
			// somebody already made.
			if (!preset_target_enabled(item)) {
				switchedOff.push_back(tag({-2., path, {}, hasSource(item) ? "carries a source" : "no source"}, "switched off"));
				continue;
			}
			const auto refused = item.contains("enable") && item["enable"].is_boolean()
				&& !item["enable"].get<bool>();

			const auto mi = item.find("matchInfo");
			if (mi != item.end() && mi->value("status", std::string()) == "error") {
				unreadable.push_back(tag({-2., path, {}, mi->value("error", std::string("(no message)"))}, "unreadable"));
				continue;
			}
			if (!refused && hasSource(item)) {
				settled++;
				continue;
			}
			if (mi == item.end()) {
				nothing.push_back(tag({-2., path, {}, "never attempted"}, "nothing scored"));
				continue;
			}

			// A stem container is decided pair by pair, so it can be part-resolved in a way
			// no ordinary entry can: naming which pair fell short is the whole of the report.
			if (const auto stems = mi->find("stems"); stems != mi->end() && stems->is_array() && !stems->empty()) {
				size_t resolved = 0;
				double worstScore = 2.;
				std::string worst;
				for (const auto& stem : *stems) {
					if (stem.value("status", std::string()) == "matched") {
						resolved++;
						continue;
					}
					const auto score = stem.value("score", -2.);
					if (score >= worstScore)
						continue;
					worstScore = score;
					const auto& channels = stem.contains("channels") ? stem.at("channels") : noCandidates;
					worst = std::format("{} at {:.4f}{}",
						channels.size() >= 2
							? std::format("ch{}+{}", channels[0].dump(), channels[1].dump())
							: std::string("one pair"),
						score,
						stem.contains("source") ? " (" + leaf(stem.value("source", std::string())) + ")" : "");
				}
				partialStems.push_back(tag({-2., path, {},
					std::format("{} of {} stems resolved; weakest {}", resolved, stems->size(),
						worst.empty() ? "would not decode" : worst)}, "partial stems"));
				continue;
			}

			const auto& candidates = mi->contains("candidates") && mi->at("candidates").is_array()
				? mi->at("candidates") : noCandidates;
			if (candidates.empty()) {
				nothing.push_back(tag({-2., path, {}, {}}, "nothing scored"));
				continue;
			}

			// Whichever metric the verdict actually turned on. Printing the envelope score
			// beside a decision the rerank made would be quoting the wrong number at a reader
			// who is about to go and check it.
			const auto reranked = candidates.front().contains("spectralScore");
			const auto scoreOf = [&](const nlohmann::json& candidate) {
				return reranked ? candidate.value("spectralScore", -2.) : candidate.value("score", -2.);
			};
			const auto scoreBar = reranked ? rerankMinScore : minScore;
			const auto marginBar = reranked ? rerankMinMargin : minMargin;

			const auto best = scoreOf(candidates.front());
			std::string why;
			if (best < scoreBar)
				why = std::format("below {:.3f}", scoreBar);
			for (size_t i = 1; i < candidates.size(); i++) {
				// Another release of the same recording is not a rival; the margin was never
				// measured against it, so saying it was would send a reader after a non-issue.
				if (candidates[i].value("duplicateOfBest", false))
					continue;
				if (const auto gap = best - scoreOf(candidates[i]); gap < marginBar)
					why += std::format("{}only {:.4f} over {} ({:.4f}), needs {:.3f}",
						why.empty() ? "" : ", ", gap,
						leaf(candidates[i].value("source", std::string("?"))),
						scoreOf(candidates[i]), marginBar);
				break;
			}
			// Two different jobs wearing one label. A candidate that cleared the score bar and
			// lost on the margin names the answer and asks which release it is -- a listen
			// settles it. One that never cleared the bar is the best of a bad field, and on a
			// run over targets no preset covers it is nearly all of them: 226 of 228, the best
			// of them at 0.36. Calling those "matched" would send a reader to check 226 files
			// that nothing actually matched.
			auto miss = tag({best, path,
				leaf(candidates.front().value("source", std::string("?"))),
				why.empty() ? std::string("declined, though both bars were met") : why},
				best >= scoreBar ? "margin only" : "below the score bar");
			for (size_t i = 1; i < candidates.size(); i++) {
				if (candidates[i].value("duplicateOfBest", false))
					continue;
				miss.RunnerUp = leaf(candidates[i].value("source", std::string("?")));
				miss.RunnerUpScore = scoreOf(candidates[i]);
				break;
			}
			(best >= scoreBar ? nearMiss : bestGuess).push_back(std::move(miss));
		}

		const auto attention = nearMiss.size() + bestGuess.size() + partialStems.size()
			+ nothing.size() + unreadable.size() + switchedOff.size();

		for (auto* rows : {&nearMiss, &bestGuess})
			std::ranges::sort(*rows, [](const entry& a, const entry& b) { return a.Score > b.Score; });
		for (auto* rows : {&partialStems, &nothing, &unreadable, &switchedOff})
			std::ranges::sort(*rows, [](const entry& a, const entry& b) { return a.Path < b.Path; });

		// Written before the "nothing to do" exit below, so a caller that asked for the file
		// always gets one: an empty report and a missing report look the same to a script,
		// and only one of them means the run went well.
		if (!csvPath.empty()) {
			std::ofstream csv(csvPath, std::ios::binary);
			if (!csv)
				throw std::runtime_error(std::format("Could not open summary CSV: {}", u8(csvPath)));
			// Quoted per RFC 4180: the names column holds "TerritoryType:The Sea of Clouds",
			// and several hold a comma of their own.
			const auto field = [](std::string_view v) {
				std::string out = "\"";
				for (const auto c : v) {
					if (c == '"')
						out += '"';
					out += c;
				}
				return out + '"';
			};
			const auto number = [](double v) {
				return v <= -2. ? std::string() : std::format("{:.4f}", v);
			};
			csv << "target,group,seconds,best_source,best_score,runner_up,runner_up_score,reason,names\n";
			for (const auto* rows : {&nearMiss, &bestGuess, &partialStems, &nothing, &unreadable, &switchedOff})
				for (const auto& r : *rows)
					csv << field(r.Path) << ',' << field(r.Group) << ','
						<< (r.Seconds > 0. ? std::format("{:.2f}", r.Seconds) : std::string()) << ','
						<< field(r.Source) << ',' << number(r.Score) << ','
						<< field(r.RunnerUp) << ',' << number(r.RunnerUpScore) << ','
						<< field(r.Detail) << ',' << field(r.Names) << '\n';
			std::cerr << std::format("Wrote {} row(s) to {}.", attention, u8(csvPath)) << '\n';
		}

		if (!attention) {
			std::cerr << std::format("Nothing needs manual attention: all {} target(s) resolved.", total) << '\n';
			return;
		}

		std::cerr << '\n' << std::format("Needs manual attention: {} of {} target(s); {} resolved.",
			attention, total, settled) << '\n';

		const auto dump = [&](std::string_view heading, std::string_view note,
			const std::vector<entry>& rows, bool withScore) {
			if (rows.empty())
				return;
			std::cerr << '\n' << std::format("  {} ({})", heading, rows.size()) << '\n';
			if (!note.empty())
				std::cerr << std::format("  {}", note) << '\n';
			const auto shown = limit ? (std::min)(limit, rows.size()) : rows.size();
			for (size_t i = 0; i < shown; i++) {
				if (withScore)
					std::cerr << std::format("    {:.4f}  {:<46} {:<30} {}",
						rows[i].Score, rows[i].Path, rows[i].Source, rows[i].Detail) << '\n';
				else if (rows[i].Detail.empty())
					std::cerr << std::format("    {}", rows[i].Path) << '\n';
				else
					std::cerr << std::format("    {:<46} {}", rows[i].Path, rows[i].Detail) << '\n';
			}
			if (shown < rows.size())
				std::cerr << std::format("    ... and {} more (--summary-limit 0 lists every one)",
					rows.size() - shown) << '\n';
		};

		dump("Matched but not enabled -- it cleared the score bar and lost on the margin",
			"  Nearest the bar first: these are the ones a single listen is most likely to settle.",
			nearMiss, true);
		dump("Best guess only -- nothing cleared the score bar",
			"  The top of a weak field, listed so a reader can see there was nothing to find.",
			bestGuess, true);
		dump("Partly matched -- some of the file's stems resolved and some did not", {}, partialStems, false);
		dump("Nothing scored -- no candidate cleared the floor at any alignment", {}, nothing, false);
		dump("Could not be read", {}, unreadable, false);
		dump("Switched off in the input, and left exactly as they were", {}, switchedOff, false);
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
				"review. Those are listed at the end of the run, grouped by what reviewing one would involve --\n"
				"first the ones where something did score and only the score or the margin held it back, which\n"
				"are the ones a listen can settle.\n"
				"Its \"target\" also carries \"names\": every \"<SheetName>:<name>\" the game's own sheets\n"
				"attach to that file, e.g. \"TerritoryType:The Tempest\" or \"Orchestrion:A New Hope\" -- from\n"
				"territorytype/contentfindercondition/instancecontent/fate/mount/leve/weddingbgm (via bgm/\n"
				"bgmsituation/placename) and orchestrionpath/orchestrion. Empty for music no sheet references.\n"
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
		parser.add_argument("--summary-limit").default_value(20u).scan<'u', uint32_t>().help("how many targets to name in each group of the closing manual-attention summary (default: 20; 0 lists every one)");
		parser.add_argument("--summary-csv").default_value(std::string()).help("also write that summary as a CSV, one row per target, with the game's own names for each file");
		parser.add_argument("--ffmpeg").default_value(std::string("ffmpeg")).help("path to ffmpeg executable");
		parser.add_argument("--ffprobe").default_value(std::string("ffprobe")).help("path to ffprobe executable, used to read title tags for duplicate-release detection");
		parser.add_argument("--language").default_value(xivres::game_language::Unspecified)
			.help("language the place and duty names are written in: the client's own (default), or ja, en, de, fr, chs, cht, tc or ko")
			.action([](const std::string& spec) -> xivres::game_language {
				const auto lower = xivres::util::unicode::convert<std::string>(spec, &xivres::util::unicode::lower);
				for (const auto language : {
					xivres::game_language::Japanese,
					xivres::game_language::English,
					xivres::game_language::German,
					xivres::game_language::French,
					xivres::game_language::ChineseSimplified,
					xivres::game_language::ChineseTraditional,
					xivres::game_language::TraditionalChinese,
					xivres::game_language::Korean,
				})
					if (lower == xivres::game_language_code(language))
						return language;
				if (lower == "auto" || lower == "default" || lower == "unspecified")
					return xivres::game_language::Unspecified;
				throw std::runtime_error(std::format("Unknown language: {} (try ja, en, de, fr, chs, cht, tc or ko)", spec));
			});
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
		// The envelope ranks; log-mel decides. An envelope is one number per 5 ms and carries
		// no timbre, so on a short cue it finds a plausible alignment almost anywhere: one run
		// against the Monster Hunter World albums returned 13 "matched" targets of which 12
		// score between -0.15 and 0.41 spectrally. In the other direction it loses real
		// matches -- BGM_EX5_System_Title's correct source beat the runner-up by 0.0155 on
		// envelope, under the 0.05 required, and by 0.29 on log-mel.
		parser.add_argument("--rerank").default_value(true).implicit_value(true).help("rescore the top candidates on log-mel at the alignments the envelope found, and decide match/ambiguous on that; --no-rerank decides on envelope correlation alone");
		parser.add_argument("--no-rerank").default_value(false).implicit_value(true).help("disable --rerank");
		parser.add_argument("--rerank-min-score").default_value(0.75).scan<'g', double>().help("minimum log-mel similarity to accept a reranked match");
		parser.add_argument("--rerank-min-margin").default_value(0.10).scan<'g', double>().help("minimum log-mel gap over the best genuinely different candidate");
		parser.add_argument("--rerank-depth").default_value(5u).scan<'u', uint32_t>().help("how many of each target's best envelope candidates to rescore");
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
			<< "Error parsing arguments. Use `match -h` to show help.\n"
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
		const auto presetSpec = parser.present<std::string>("--preset").value_or(std::string());
		const auto excludeSpec = parser.get<std::string>("--exclude-preset");
		const auto targetPrefix = parser.get<std::string>("--target-prefix");
		const auto discover = parser.get<bool>("--discover");
		const auto outputPath = argactions::path(parser.get<std::string>("--output"));
		const auto ffmpegPath = argactions::path(parser.get<std::string>("--ffmpeg"));
		const auto ffprobePath = argactions::path(parser.get<std::string>("--ffprobe"));
		const auto minScore = parser.get<double>("--min-score");
		const auto minMargin = parser.get<double>("--min-margin");
		const auto rerank = parser.get<bool>("--rerank") && !parser.get<bool>("--no-rerank");
		const auto rerankMinScore = parser.get<double>("--rerank-min-score");
		const auto rerankMinMargin = parser.get<double>("--rerank-min-margin");
		const auto rerankDepth = parser.get<uint32_t>("--rerank-depth");
		const auto maxDurationDiff = parser.get<double>("--max-duration-diff");
		const auto maxOffset = parser.get<double>("--max-offset");
		const auto minOverlapSeconds = parser.get<double>("--min-overlap");
		const auto minOverlapFraction = parser.get<double>("--min-overlap-fraction");
		const auto shortlistSize = parser.get<uint32_t>("--shortlist");
		const auto duplicateThreshold = parser.get<double>("--duplicate-threshold");
		const auto segmentMinScore = parser.get<double>("--segment-min-score");
		const auto magnetThreshold = parser.get<uint32_t>("--magnet-threshold");
		const auto language = parser.get<xivres::game_language>("--language");

		const auto gameRoot = argactions::installation_root(gameSpec);
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
			: loadPreset(argactions::path(presetSpec));

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
			for (const auto& item : loadPreset(argactions::path(part)).at("items"))
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
				return std::ranges::any_of(prefixes, [&](const std::string& p) { return path.starts_with(p); });
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
				preset["items"].size(), skippedAsCovered, skippedByPrefix) << '\n';
		}

		if (preset["items"].empty())
			throw std::runtime_error("No target items to match.");

		std::vector<candidate> candidates;
		// constexpr data rather than a std::set: the list is fixed, so it must not need an
		// exit-time destructor.
		static constexpr std::array<std::wstring_view, 5> exts = {L".flac", L".ogg", L".wav", L".mp3", L".m4a"};
		// follow_directory_symlink so an OST directory assembled out of links to the real
		// album folders works; by default reparse points are skipped and the scan finds
		// nothing at all, which looks exactly like "no matches".
		for (const auto& entry : std::filesystem::recursive_directory_iterator(ostDir, std::filesystem::directory_options::follow_directory_symlink | std::filesystem::directory_options::skip_permission_denied)) {
			if (!entry.is_regular_file())
				continue;
			const auto ext = xivres::util::unicode::convert<std::wstring>(entry.path().extension().wstring(), &xivres::util::unicode::lower);
			if (!std::ranges::contains(exts, ext))
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
				const auto ext = xivres::util::unicode::convert<std::wstring>(p.extension().wstring(), &xivres::util::unicode::lower);
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
			for (const auto& group : byStem | std::views::values) {
				const bool hasLossless = std::ranges::any_of(group,
					[&](size_t i) { return rank(candidates[i].Path) == 0; });
				for (const auto index : group)
					if (!hasLossless || rank(candidates[index].Path) == 0)
						keep.push_back(index);
			}
			std::ranges::sort(keep);

			if (keep.size() != candidates.size()) {
				std::vector<candidate> deduped;
				deduped.reserve(keep.size());
				for (const auto index : keep)
					deduped.push_back(std::move(candidates[index]));
				std::cerr << std::format("Ignoring {} duplicate-encoding candidate(s) of the same track.", candidates.size() - deduped.size()) << '\n';
				candidates = std::move(deduped);
			}
		}

		std::cerr << std::format("Found {} candidate OST file(s). Decoding...", candidates.size()) << '\n';

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
		std::cerr << std::format("{} candidate(s) ready for matching.", candidates.size()) << '\n';

		// Coarse copies of every envelope, used to shortlist candidates cheaply before the
		// full-resolution pass. Correlating 200 Hz envelopes of whole tracks costs a
		// 256k-point FFT per pair, which is fine for one album but not when every BGM in
		// the game is matched against every track of every album; at 10 Hz the same pass
		// is thousands of times cheaper and still ranks the right track near the top.
		constexpr size_t CoarseFactor = 20;
		constexpr auto coarseRateHz = EnvelopeRateHz / static_cast<double>(CoarseFactor);
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
			std::cerr << std::format("Matching {} target(s) against {} candidate(s)...", workItems.size(), candidates.size()) << '\n';

		// Scores one envelope against the candidate pool. Shared by the ordinary path and
		// the per-stem path used for the game's multi-channel stem containers.
		struct scored {
			std::string Name;
			double Score;
			double OffsetSeconds;
			double OverlapSeconds;
			size_t CandidateIndex;
			// Log-mel similarity at this candidate's own offset, filled in by the rerank in
			// phase 3. -2 means "not measured": either the rerank is off, this candidate sat
			// below --rerank-depth, or its overlap was too short to judge.
			double SpectralScore = -2.;
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
			std::vector duplicateOfWinner(scores.size(), false);
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
				for (const auto& index : ranked | std::views::values)
					shortlist.push_back(index);
			}

			// Stage 2, full resolution: score only the shortlist.
			for (const auto index : shortlist) {
				const auto& c = candidates[index];
				const auto r = best_envelope_correlation_ex(envelope, c.Envelope, EnvelopeRateHz, maxOffset, minOverlap, minOverlapFraction);
				if (r.Score > -1.5)
					res.Scores.push_back({
						.Name = c.RelName,
						.Score = r.Score,
						.OffsetSeconds = r.OffsetSeconds,
						.OverlapSeconds = r.OverlapSeconds,
						.CandidateIndex = index,
					});
			}
			auto& scores = res.Scores;
			std::ranges::sort(scores, [](const auto& a, const auto& b) { return a.Score > b.Score; });

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
		std::vector needsPhase3(workItems.size(), false);
		// The extracted target audio, kept for the rerank in phase 3. The file itself already
		// outlives phase 1 -- it is in `tempFiles` and removed only at the end of the run --
		// but the path was a local, so phase 3 had no way to name it again.
		std::vector<std::filesystem::path> pendingTargetAudio(workItems.size());
		std::vector<double> pendingTargetDuration(workItems.size());
		// How long the game's own file runs, kept for every worked item rather than only the
		// deferred ones. It is the first thing a reader wants about a target nothing matched:
		// a four-second cue and a four-minute field theme are different problems.
		std::vector<double> targetSeconds(workItems.size(), 0.);

		// Filled in for every plain (mono/stereo) target that decoded successfully, so a
		// later pass can find targets that are verbatim reuses of another target's audio
		// (a shared boss/event stinger reused across zones/patches under a different BGM
		// slot name -- see [[project-magnet-source-theory-disproven]]) and let a confident
		// match on one propagate to the rest instead of asking each alias to independently
		// clear the score/margin bar. Multi-channel stem targets are excluded: they decide
		// per-stem, not against this single envelope, so linking them would not make sense.
		std::vector<std::vector<float>> dupTargetEnvelope(workItems.size());
		std::vector dupTargetDuration(workItems.size(), 0.0);
		std::vector dupTargetEligible(workItems.size(), false);

		parallel_for(workItems.size(), [&](size_t workIndex) {
			auto& item = preset.at("items")[workItems[workIndex]];
			const auto targetPaths = preset_target_paths(item);

			std::filesystem::path targetAudio;
			std::vector<float> targetEnvelope;
			double targetDuration = 0;
			size_t targetChannels = 0;
			bool isStemStream = false;
			try {
				targetAudio = extract_scd_audio_to_temp(installation, targetPaths.front(), tempDir, tempFileCounter.fetch_add(1), &targetChannels, &isStemStream);
				{
					const auto lock = std::scoped_lock(progressMutex);
					tempFiles.push_back(targetAudio);
				}
				targetEnvelope = decode_envelope(ffmpegPath, targetAudio);
				targetDuration = static_cast<double>(targetEnvelope.size()) / EnvelopeRateHz;
				targetSeconds[workIndex] = targetDuration;
				if (targetChannels <= 2) {
					dupTargetEnvelope[workIndex] = targetEnvelope;
					dupTargetDuration[workIndex] = targetDuration;
					dupTargetEligible[workIndex] = true;
				}
			} catch (const std::exception& e) {
				const auto lock = std::scoped_lock(progressMutex);
				std::cerr << std::format("Warning: could not decode target {}: {}", targetPaths.front(), e.what()) << '\n';
				item["matchInfo"] = {{"status", "error"}, {"error", e.what()}};
				item["enable"] = false;
				unmatchedCount++;
				return;
			}

			const auto resolved = resolveEnvelope(targetEnvelope, targetDuration);
			const auto& scores = resolved.Scores;
			const auto& isDuplicateOfWinner = resolved.DuplicateOfWinner;

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
					const bool overlapsKept = std::ranges::any_of(keptRegions, [&](const auto& kept) {
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
			if (targetChannels > 2 && targetChannels % 2 == 0 && segmentMinScore > 0. && isStemStream) {
				stemsAllConfident = true;
				std::vector<size_t> stemSources;  // candidate indices, one per confidently-matched stem
				const auto channelPairs = discover_channel_pairing(ffmpegPath, targetAudio, targetChannels, tempDir, tempFileCounter, tempFiles, progressMutex);
				for (const auto& [channelA, channelB] : channelPairs) {
					try {
						const auto stemPath = extract_stem_to_temp(ffmpegPath, targetAudio, channelA, channelB, tempDir, tempFileCounter.fetch_add(1));
						{
							const auto lock = std::scoped_lock(progressMutex);
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
							argactions::path(c.RelName).stem().wstring());
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

			const auto lock = std::scoped_lock(progressMutex);
			if (targetChannels > 2 && targetChannels % 2 == 0 && segmentMinScore > 0. && isStemStream) {
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
				pendingTargetAudio[workIndex] = targetAudio;
				pendingTargetDuration[workIndex] = targetDuration;
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
		std::vector isMagnet(candidates.size(), false);
		if (magnetThreshold) {
			for (size_t i = 0; i < candidates.size(); ++i) {
				if (appearanceCount[i] > magnetThreshold) {
					isMagnet[i] = true;
					std::cerr << std::format("Magnet source, dropped from candidate lists (scored >= {} against {} distinct targets): {}",
						minScore, appearanceCount[i], candidates[i].RelName) << '\n';
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

			// Rescore the best few on log-mel, at the alignments the envelope already found,
			// and re-sort on that. Only the ranking changes -- each candidate keeps the offset
			// it was found at, because the envelope locates a match well even when it cannot
			// choose between two of them.
			//
			// Done before the lock: it decodes audio, and holding the mutex across that would
			// serialise the whole pass.
			auto rerankScored = false;
			if (rerank && !scores.empty()) {
				try {
					const auto depth = (std::min<size_t>)(rerankDepth, scores.size());
					// Only as much of the target as the comparison will read.
					const auto targetSpec = decode_logmel(ffmpegPath, pendingTargetAudio[workIndex],
						pendingTargetDuration[workIndex] + 1.);
					std::vector<double> spectral(scores.size(), -2.);
					for (size_t i = 0; i < depth; ++i) {
						// A short window scores high almost anywhere -- that is the defect
						// being corrected, so it must not be reintroduced here. Judge only
						// alignments whose overlap was already worth judging.
						if (scores[i].OverlapSeconds < pendingMinOverlap[workIndex])
							continue;
						const auto& c = candidates[scores[i].CandidateIndex];
						const auto sourceSpec = decode_logmel(ffmpegPath, c.Path,
							std::abs(scores[i].OffsetSeconds) + pendingTargetDuration[workIndex] + 2.);
						spectral[i] = spectral_similarity_at(targetSpec, sourceSpec,
							scores[i].OffsetSeconds, 0., pendingTargetDuration[workIndex]);
					}
					if (std::ranges::any_of(spectral, [](double s) { return s > -1.5; })) {
						for (size_t i = 0; i < scores.size(); ++i)
							scores[i].SpectralScore = spectral[i];
						std::ranges::stable_sort(scores, [](const auto& a, const auto& b) {
							return a.SpectralScore > b.SpectralScore;
						});
						rerankScored = true;
					}
				} catch (const std::exception&) {
					// A source that will not decode costs this target its rerank, never the
					// whole run -- the envelope ranking below is still a usable answer.
				}
			}

			const auto lock = std::scoped_lock(progressMutex);
			if (scores.empty()) {
				item["matchInfo"] = {{"status", "unmatched"}};
				item["enable"] = false;
				unmatchedCount++;
				return;
			}

			const auto [isDuplicateOfWinner, distinctRunnerUp] =
				computeDuplicatesAndRunnerUp(scores, pendingMinOverlap[workIndex]);

			// Rebuilt rather than carried over from phase 1: the rerank may have reordered
			// these, and a report listing them in the order they were *not* decided in is
			// worse than no report. Both numbers are kept so a reader can see the two
			// metrics disagree, which is the whole reason the rerank exists.
			auto candidatesJson = pendingCandidatesJson[workIndex];
			if (rerankScored) {
				candidatesJson = nlohmann::json::array();
				for (size_t i = 0; i < (std::min<size_t>)(5, scores.size()); i++) {
					auto entry = nlohmann::json{
						{"source", scores[i].Name},
						{"score", scores[i].Score},
						{"offset", scores[i].OffsetSeconds},
						{"overlap", scores[i].OverlapSeconds},
					};
					if (scores[i].SpectralScore > -1.5)
						entry["spectralScore"] = scores[i].SpectralScore;
					if (const auto& rawTitle = candidates[scores[i].CandidateIndex].RawTitle; !rawTitle.empty())
						entry["sourceTitle"] = rawTitle;
					if (isDuplicateOfWinner[i])
						entry["duplicateOfBest"] = true;
					candidatesJson.push_back(std::move(entry));
				}
			}

			const bool confident = rerankScored
				? scores[0].SpectralScore >= rerankMinScore
					&& (distinctRunnerUp == scores.size()
						|| scores[0].SpectralScore - scores[distinctRunnerUp].SpectralScore >= rerankMinMargin)
				: scores[0].Score >= minScore
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
					argactions::path(scores[0].Name).stem().wstring());
				item["source"] = nlohmann::json::array({key});

				// Every other release of the same recording contributes its file stem too. A
				// piece re-released across discs is one recording under several names, and only
				// one of them can win this target -- but this key list is what a consumer searches
				// its own album folder with, so listing them all means owning any one of those
				// discs resolves the entry. It is the convention the hand-written presets already
				// follow: "Primogenitor" is listed against the same targets in both
				// Before The Fall.json and The Far Edge Of Fate.json.
				for (size_t i = 1; i < scores.size(); i++) {
					if (!isDuplicateOfWinner[i])
						continue;
					auto alt = xivres::util::unicode::convert<std::string>(
						argactions::path(scores[i].Name).stem().wstring());
					if (!std::ranges::contains(item["source"], nlohmann::json(alt)))
						item["source"].push_back(std::move(alt));
				}

				if (!winnerCandidate.EnglishTitle.empty())
					item["source"].push_back(winnerCandidate.EnglishTitle);
				if (!winnerCandidate.JapaneseTitle.empty())
					item["source"].push_back(winnerCandidate.JapaneseTitle);
				item["matchInfo"] = {
					{"status", "matched"},
					{"score", scores[0].Score},
					{"file", scores[0].Name},
					{"offset", scores[0].OffsetSeconds},
					{"candidates", std::move(candidatesJson)},
					{"segments", std::move(pendingSegmentsJson[workIndex])},
				};
				if (scores[0].SpectralScore > -1.5)
					item["matchInfo"]["spectralScore"] = scores[0].SpectralScore;
				matchedCount++;
			} else {
				item["matchInfo"] = {{"status", "ambiguous"}, {"candidates", std::move(candidatesJson)}};
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

			for (const auto& members : byDuration | std::views::values) {
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

			for (const auto& members : groups | std::views::values) {
				if (members.size() < 2)
					continue;

				{
					const auto lock = std::scoped_lock(progressMutex);
					std::cerr << "Duplicate-target group:";
					for (const auto wi : members)
						std::cerr << " " << preset_target_paths(preset.at("items")[workItems[wi]]).front();
					std::cerr << '\n';
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

		for (size_t wi = 0; wi < workItems.size(); ++wi)
			if (const auto mi = preset.at("items")[workItems[wi]].find("matchInfo");
				mi != preset.at("items")[workItems[wi]].end() && targetSeconds[wi] > 0.)
				(*mi)["targetSeconds"] = targetSeconds[wi];

		// The targets carry the game's own names for what each file is the music of, from the
		// bgm, territorytype, contentfindercondition and placename sheets.
		for (auto& item : preset.at("items"))
			if (const auto target = item.find("target"); target != item.end())
				augment_target_names(*target, installation, language);

		// Before --matched-only, which is the option that throws these away: the summary of
		// what was dropped is most useful precisely in the run that stops writing it down.
		print_attention_summary(preset, parser.get<uint32_t>("--summary-limit"),
			minScore, minMargin, rerankMinScore, rerankMinMargin,
			argactions::path(parser.get<std::string>("--summary-csv")));

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
			matchedCount, ambiguousCount, unmatchedCount, skippedCount) << '\n';
		return 0;

	} catch (const std::exception& e) {
		cleanupTempFiles();
		std::cerr
			<< "Error processing data.\n"
			<< e.what() << '\n';
		return -1;
	}
}
