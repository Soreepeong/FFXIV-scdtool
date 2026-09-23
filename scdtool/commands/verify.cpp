#include "pch.h"
#include "verify.h"

#include "utils/argactions.h"
#include "utils/audio_match.h"
#include "utils/misc.h"
#include "utils/win32_process.h"
#include "utils/hca_payload.h"
#include "utils/join_detector.h"
#include "utils/preset_model.h"
#include "utils/substitute_codec.h"
#include "utils/verify_audio.h"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <set>
#include <fstream>
#include <atomic>
#include <mutex>
#include <ranges>
#include <chrono>
#include <thread>

namespace {
	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}

	struct loop_info {
		size_t StartSample = 0;
		size_t EndSample = 0;
		size_t Rate = 0;
		size_t TotalSamples = 0;
		size_t Channels = 0;
	};

	// The entry's audio written somewhere ffmpeg can open, plus what the header says about
	// its loop. Both halves of every comparison go through this, so the built file and the
	// game's own are read the same way and a difference between them is theirs.
	loop_info unwrap_entry(const std::shared_ptr<xivres::stream>& stream, uint32_t entryIndex,
		const std::filesystem::path& outStem, std::filesystem::path& written) {

		const xivres::sound::reader scd(stream);
		if (scd.sound_item_count() <= entryIndex)
			throw std::runtime_error(std::format("only {} sound entries", scd.sound_item_count()));
		const auto item = scd.read_sound_item(entryIndex);

		loop_info info{.Rate = static_cast<size_t>(item.Header->SamplingRate),
			.Channels = static_cast<size_t>(item.Header->ChannelCount)};
		std::vector<uint8_t> bytes;
		const wchar_t* ext;
		if (hca_payload::is_hca(item)) {
			bytes = hca_payload::payload_file(item);
			ext = L".hca";
			info.TotalSamples = static_cast<size_t>(hca_payload::inspect(item).TotalFrames);
		} else if (const auto payload = substitute_codec::payload_of(item);
			payload != substitute_codec::payload::Vorbis) {
			bytes = substitute_codec::payload_file(item);
			ext = substitute_codec::payload_extension(payload);
			const auto inspected = substitute_codec::inspect(item);
			info.TotalSamples = inspected.TotalFrames;
			// Without this the seam check silently never runs -- it is gated on
			// EndSample > StartSample -- and, worse, silence is counted over the whole file
			// instead of to the loop end, which read 124.2s against a true 123.0s.
			if (const auto loop = substitute_codec::loop_in_samples(item)) {
				info.StartSample = static_cast<size_t>(loop->Start);
				info.EndSample = static_cast<size_t>(loop->End);
			}
		} else if (item.Header->Format == xivres::sound::sound_entry_format::Ogg) {
			const auto decoded = item.get_ogg_decoded();
			info.StartSample = decoded.LoopStartBlockIndex;
			info.EndSample = decoded.LoopEndBlockIndex;
			info.TotalSamples = decoded.Data.size() / sizeof(float)
				/ (decoded.Channels ? decoded.Channels : 1);
			bytes = item.get_ogg_file();
			ext = L".ogg";
		} else if (item.Header->Format == xivres::sound::sound_entry_format::WaveFormatPcm) {
			bytes = item.get_wav_file();
			ext = L".wav";
		} else {
			throw std::runtime_error("entry is neither Ogg nor PCM wave");
		}

		written = outStem;
		written.replace_extension(ext);
		std::ofstream f(written, std::ios::binary);
		if (!f)
			throw std::runtime_error(std::format("could not create {}", u8(written)));
		f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		return info;
	}

	// One stretch of the build measured against the same stretch of the game's own file.
	struct window_comparison {
		double Level = 0.;   // mean dB, built minus game: negative is quieter
		double Worst = 0.;   // the worst single second of that, which is what a dip sounds like
		double Match = 0.;   // weighted log-mel cosine, the same metric as the whole-file score
		bool Valid = false;
	};

	// How far the build sits from the game's own file over one window, in milliseconds.
	//
	// Reported instead of a score because a score cannot answer "is the sync right": a
	// different master scores low while perfectly aligned, and a misaligned copy of the same
	// master scores low too. The lag is zero when the two line up, whatever else differs.
	struct lag_probe {
		double LagMs = 0.;   // built against the game; positive means the build is late
		double Match = 0.;   // correlation at that lag
		bool Valid = false;
	};

	// The same lag, probed at several points from the head to near the end. The head and
	// loop-start probes cannot see an offset that is wrong only after a cut, or a recording
	// that runs fast: a join campaign over 61 stitched targets found offsets 1-13 ms out on
	// most of them, and recordings 7-13 ppm fast (3.7 ms by BGM_EX5_Raid_26's loop end),
	// while every log-mel score read 0.98. The slope is the drift, the spread the offset.
	struct lag_span {
		size_t Probes = 0;         // confident probes (match >= 0.90) that went into it
		double MinMs = 0., MaxMs = 0.;
		double Ppm = 0.;           // lag against time: positive means the build falls behind
		bool Valid = false;
	};

	// The build's level against the game's, per second over the seconds the game is
	// audible. Neither the weighted score nor the envelope hole sees a whole-file level
	// error: BGM_EX2_Field_Safe_01 built 3.7 dB quiet and every mono entry about 4 dB quiet
	// while both read clean.
	struct level_offset {
		double MedianDb = 0.;      // build minus game
		double P90AbsDb = 0.;      // 90th percentile of the per-second |difference|
		bool Valid = false;
	};

	// Both files are decoded at the same rate and laid on the same timeline, so one pair of
	// indices addresses the same music in each.
	window_comparison measure_window(std::span<const float> built, std::span<const float> game,
		double fromSeconds, double toSeconds) {

		window_comparison res;
		if (toSeconds <= fromSeconds || fromSeconds < 0.)
			return res;
		const auto rate = static_cast<double>(AnalysisRateHz);
		const auto begin = static_cast<size_t>(fromSeconds * rate);
		const auto want = static_cast<size_t>((toSeconds - fromSeconds) * rate);
		if (begin >= built.size() || begin >= game.size())
			return res;
		const auto n = (std::min)({want, built.size() - begin, game.size() - begin});
		const auto seconds = n / AnalysisRateHz;
		if (!seconds)
			return res;

		const auto b = built.subspan(begin, n);
		const auto g = game.subspan(begin, n);

		auto sum = 0.;
		auto worst = 0.;
		for (size_t sec = 0; sec < seconds; ++sec) {
			auto eb = 0., eg = 0.;
			for (size_t i = sec * AnalysisRateHz; i < (sec + 1) * AnalysisRateHz; ++i) {
				eb += static_cast<double>(b[i]) * b[i];
				eg += static_cast<double>(g[i]) * g[i];
			}
			const auto d = 10. * std::log10(eb / AnalysisRateHz + 1e-12)
				- 10. * std::log10(eg / AnalysisRateHz + 1e-12);
			sum += d;
			if (!sec || d < worst)
				worst = d;
		}
		res.Level = sum / static_cast<double>(seconds);
		res.Worst = worst;
		// A span the score cannot judge is not a span with a match of zero. Reporting it
		// valid left BGM_EX3_Field_Ama_Night's join reading exactly 0.000.
		const auto score = build_score(b, g);
		if (!score.Valid)
			return res;
		res.Match = score.Weighted;
		res.Valid = true;
		return res;
	}

	// The first moment the game's own file is carrying something, so a probe does not try to
	// align two silences. Judged against the file's own median so a quiet piece is not
	// mistaken for a silent one.
	double first_audible_second(std::span<const float> game) {
		constexpr double StepSeconds = 0.25;
		const auto step = static_cast<size_t>(StepSeconds * AnalysisRateHz);
		if (!step || game.size() < step)
			return 0.;
		std::vector<double> level;
		level.reserve(game.size() / step);
		for (size_t i = 0; i + step <= game.size(); i += step) {
			auto e = 0.;
			for (size_t j = i; j < i + step; ++j)
				e += static_cast<double>(game[j]) * game[j];
			level.push_back(10. * std::log10(e / static_cast<double>(step) + 1e-12));
		}
		if (level.empty())
			return 0.;
		auto sorted = level;
		std::sort(sorted.begin(), sorted.end());
		const auto floorDb = sorted[sorted.size() / 2] - 20.;
		for (size_t i = 0; i < level.size(); ++i)
			if (level[i] >= floorDb)
				return static_cast<double>(i) * StepSeconds;
		return 0.;
	}

	lag_probe probe_lag(std::span<const float> built, std::span<const float> game,
		double fromSeconds, double lengthSeconds, double maxLagMs) {

		lag_probe res;
		const auto rate = static_cast<double>(AnalysisRateHz);
		const auto maxLag = static_cast<ptrdiff_t>(maxLagMs * rate / 1000.);
		const auto begin = static_cast<ptrdiff_t>(fromSeconds * rate);
		const auto n = static_cast<ptrdiff_t>(lengthSeconds * rate);
		if (n < static_cast<ptrdiff_t>(AnalysisRateHz) || maxLag < 1)
			return res;
		// Room to shift the build either way without running off either end.
		if (begin < maxLag
			|| begin + n + maxLag > static_cast<ptrdiff_t>(built.size())
			|| begin + n > static_cast<ptrdiff_t>(game.size()))
			return res;

		const auto g = game.subspan(static_cast<size_t>(begin), static_cast<size_t>(n));
		auto gm = 0.;
		for (const auto v : g)
			gm += v;
		gm /= static_cast<double>(n);
		auto gd = 0.;
		for (const auto v : g)
			gd += (v - gm) * (v - gm);
		gd = std::sqrt(gd);
		if (gd <= 0.)
			return res;   // silence on the game's side: nothing to align to

		// Every lag's correlation, so the peak can be refined between grid points: at the
		// 16 kHz analysis rate one step is 62.5 us, and a build aligned to the sample sat
		// between steps and read LOWER than one that happened to be off by a whole step
		// (BGM_Event_VerySad's loop start 0.996 misaligned, 0.988 exact).
		std::vector<double> corr(static_cast<size_t>(2 * maxLag + 1), -2.);
		for (ptrdiff_t lag = -maxLag; lag <= maxLag; ++lag) {
			const auto b = built.subspan(static_cast<size_t>(begin + lag), static_cast<size_t>(n));
			auto bm = 0.;
			for (const auto v : b)
				bm += v;
			bm /= static_cast<double>(n);
			auto num = 0., bd = 0.;
			for (ptrdiff_t i = 0; i < n; ++i) {
				const auto x = static_cast<double>(b[i]) - bm;
				const auto y = static_cast<double>(g[i]) - gm;
				num += x * y;
				bd += x * x;
			}
			if (bd <= 0.)
				continue;
			const auto c = num / (std::sqrt(bd) * gd);
			corr[static_cast<size_t>(lag + maxLag)] = c;
			if (!res.Valid || c > res.Match) {
				res.Match = c;
				res.LagMs = static_cast<double>(lag) * 1000. / rate;
				res.Valid = true;
			}
		}
		if (res.Valid) {
			// A parabola through the peak and its neighbours: the lag between grid points,
			// and the correlation the peak actually reaches there.
			const auto k = static_cast<ptrdiff_t>(std::llround(res.LagMs * rate / 1000.)) + maxLag;
			if (k > 0 && k + 1 < static_cast<ptrdiff_t>(corr.size())) {
				const auto y0 = corr[static_cast<size_t>(k - 1)], y1 = corr[static_cast<size_t>(k)], y2 = corr[static_cast<size_t>(k + 1)];
				if (y0 > -2. && y2 > -2.) {
					if (const auto den = y0 - 2. * y1 + y2; den < 0.) {
						const auto d = std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
						res.LagMs += d * 1000. / rate;
						res.Match = (std::min)(1., y1 - 0.25 * (y0 - y2) * d);
					}
				}
			}
		}
		return res;
	}

	lag_span probe_lag_span(std::span<const float> built, std::span<const float> game,
		double fromSeconds, double toSeconds, double lengthSeconds, double maxLagMs) {

		constexpr size_t Points = 7;
		lag_span res;
		if (toSeconds - fromSeconds < 2. * lengthSeconds)
			return res;
		std::vector<std::pair<double, double>> at;   // (seconds, lag ms)
		for (size_t k = 0; k < Points; ++k) {
			const auto t = fromSeconds + (toSeconds - fromSeconds - lengthSeconds) * static_cast<double>(k) / (Points - 1);
			if (const auto p = probe_lag(built, game, t, lengthSeconds, maxLagMs); p.Valid && p.Match >= 0.90)
				at.emplace_back(t + lengthSeconds / 2., p.LagMs);
		}
		if (at.size() < 2)
			return res;
		res.Probes = at.size();
		res.MinMs = res.MaxMs = at.front().second;
		auto st = 0., sl = 0.;
		for (const auto& [t, l] : at) {
			res.MinMs = (std::min)(res.MinMs, l);
			res.MaxMs = (std::max)(res.MaxMs, l);
			st += t;
			sl += l;
		}
		st /= static_cast<double>(at.size());
		sl /= static_cast<double>(at.size());
		auto num = 0., den = 0.;
		for (const auto& [t, l] : at) {
			num += (t - st) * (l - sl);
			den += (t - st) * (t - st);
		}
		// ms per second is thousandths; a thousandth of that is a millionth.
		res.Ppm = den > 0. ? num / den * 1000. : 0.;
		res.Valid = true;
		return res;
	}

	level_offset measure_level(std::span<const float> built, std::span<const float> game) {
		level_offset res;
		const auto seconds = (std::min)(built.size(), game.size()) / AnalysisRateHz;
		if (seconds < 2)
			return res;
		std::vector<double> gb(seconds), gg(seconds);
		for (size_t sec = 0; sec < seconds; ++sec) {
			auto eb = 0., eg = 0.;
			for (size_t i = sec * AnalysisRateHz; i < (sec + 1) * AnalysisRateHz; ++i) {
				eb += static_cast<double>(built[i]) * built[i];
				eg += static_cast<double>(game[i]) * game[i];
			}
			gb[sec] = 10. * std::log10(eb / AnalysisRateHz + 1e-12);
			gg[sec] = 10. * std::log10(eg / AnalysisRateHz + 1e-12);
		}
		auto sorted = gg;
		std::ranges::sort(sorted);
		const auto floorDb = sorted[sorted.size() / 2] - 20.;
		std::vector<double> d;
		for (size_t sec = 0; sec < seconds; ++sec)
			if (gg[sec] >= floorDb)
				d.push_back(gb[sec] - gg[sec]);
		if (d.size() < 2)
			return res;
		auto m = d;
		std::ranges::sort(m);
		res.MedianDb = m[m.size() / 2];
		for (auto& v : d)
			v = std::abs(v);
		std::ranges::sort(d);
		res.P90AbsDb = d[(d.size() * 9) / 10 < d.size() ? (d.size() * 9) / 10 : d.size() - 1];
		res.Valid = true;
		return res;
	}

	// One place two segments of a preset meet, on the target's own timeline.
	struct join_point {
		double AtSeconds = 0.;         // where the incoming segment starts
		double CrossfadeSeconds = 0.;  // how long the outgoing one keeps playing under it
		bool SameRecording = false;    // both sides read the same source: a loop-out, not a medley
	};

	struct join_comparison {
		join_point Join;
		window_comparison Window;      // centred on the crossfade
		window_comparison Control;     // same length, from inside the outgoing segment
		// Build minus game over the second before the join and the second after its crossfade,
		// and where the game sits against its own median there. Separate sides, because the
		// two things that go wrong at a join happen on different sides: the outgoing recording
		// runs out before it, or the incoming one plays where the game is silent after it.
		double BeforeDb = 0., AfterDb = 0.;
		double GameBeforeRel = 0., GameAfterRel = 0.;
		double BuiltBeforeRel = 0., BuiltAfterRel = 0.;
		double Seam = 0.;              // click ratio at a hard cut
		bool HasBefore = false, HasAfter = false, HasSeam = false;
	};

	// Where the segments of each target meet, read out of the presets a build came from.
	//
	// A join leaves no mark in the file it produces -- hiding it is what a crossfade is for --
	// and an .scd records nothing about where one was, so this is the only place to learn it.
	// Which listing a path is built from follows `apply`: files in release order by their
	// `name`, and the first enabled listing wins. `apply` also passes over a listing whose
	// source does not resolve, which this cannot see.
	std::map<std::string, std::vector<join_point>> read_joins(const std::string& spec) {
		std::vector<std::pair<std::string, std::filesystem::path>> files;
		for (size_t begin = 0, end; begin <= spec.size(); begin = end + 1) {
			end = spec.find(',', begin);
			if (end == std::string::npos)
				end = spec.size();
			const auto part = spec.substr(begin, end - begin);
			if (part.empty())
				continue;
			const auto path = argactions::path(part);
			std::vector<std::filesystem::path> found;
			if (std::filesystem::is_directory(path)) {
				for (const auto& entry : std::filesystem::directory_iterator(path))
					if (entry.is_regular_file() && entry.path().extension() == L".json")
						found.push_back(entry.path());
			} else {
				found.push_back(path);
			}
			for (auto& one : found) {
				std::string name;
				try {
					std::ifstream f(one, std::ios::binary);
					nlohmann::json head;
					f >> head;
					name = head.value("name", std::string{});
				} catch (const std::exception&) {
				}
				files.emplace_back(name.empty() ? "\xff" + u8(one.filename()) : name, std::move(one));
			}
		}
		std::ranges::sort(files);

		std::map<std::string, std::vector<join_point>> res;
		std::set<std::string> seen;
		for (const auto& [_name, file] : files) {
			nlohmann::json preset;
			try {
				std::ifstream f(file, std::ios::binary);
				f >> preset;
			} catch (const std::exception& e) {
				std::cerr << std::format("Warning: could not read {}: {}", u8(file), e.what()) << '\n';
				continue;
			}
			if (!preset.contains("items") || !preset["items"].is_array())
				continue;
			for (const auto& item : preset["items"]) {
				const auto target = item.find("target");
				if (target == item.end())
					continue;
				for (const auto& one : target->is_array() ? *target : nlohmann::json::array({*target})) {
					if (!one.is_object() || one.value("enable", true) == false)
						continue;
					std::vector<std::string> paths;
					if (const auto p = one.find("path"); p != one.end()) {
						if (p->is_string())
							paths.push_back(p->get<std::string>());
						else if (p->is_array())
							for (const auto& x : *p)
								if (x.is_string())
									paths.push_back(x.get<std::string>());
					}

					// The same walk `apply` makes: a segment starts where it says, or where
					// the previous one's stated length ends.
					std::vector<join_point> joins;
					if (const auto segs = one.find("segments"); segs != one.end() && segs->is_array()) {
						auto cursor = 0.;
						std::set<std::string> previous;
						for (size_t i = 0; i < segs->size(); ++i) {
							const auto& seg = (*segs)[i];
							std::set<std::string> sources;
							if (const auto ch = seg.find("channels"); ch != seg.end() && ch->is_array())
								for (const auto& c : *ch) {
									const auto n = c.value("source", std::string("source"));
									if (n != "target")
										sources.insert(n);
								}
							const auto stated = seg.value("startSeconds", -1.);
							const auto start = stated >= 0. ? stated : cursor;
							if (i > 0)
								joins.push_back({
									.AtSeconds = start,
									.CrossfadeSeconds = seg.value("crossfadeSeconds", 0.),
									.SameRecording = !sources.empty() && sources == previous,
								});
							cursor = start + seg.value("length", 0.);
							previous = std::move(sources);
						}
					}
					// Keyed in lower case: SqPack lowercases a path before hashing it, so two
					// spellings that differ only in case are one file, and the presets do not
					// always agree with the build on which spelling to use.
					for (const auto& path : paths) {
						auto key = path;
						std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						if (seen.insert(key).second && !joins.empty())
							res.emplace(std::move(key), joins);
					}
				}
			}
		}
		return res;
	}

	std::string lower_ascii(std::string s) {
		std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	// The segments `apply` built each multi-copy target from, resolved against the OST pool
	// the same way, for the join detector.
	//
	// Unlike read_joins this can see resolution, so it follows `apply` the rest of the way: in
	// release order, the first listing of a path that *resolves* is the one built. Only paths
	// some listing could make multi-copy are resolved at all -- more than one segment, an
	// asplit in a filter, or a graph-built source -- since resolving is a directory walk per
	// pattern and 1900 of them is minutes; but every listing of such a path is resolved in
	// turn, the single-copy ones included, because an earlier single-copy listing that
	// resolves is what the build holds and there is then nothing to judge.
	std::map<std::string, std::vector<apply_segment>> read_detector_targets(const std::string& spec,
		const std::filesystem::path& ostDir, std::vector<std::pair<std::string, std::string>>& unresolved) {

		std::vector<std::filesystem::path> roots;
		for (size_t begin = 0, end; begin <= spec.size(); begin = end + 1) {
			end = spec.find(',', begin);
			if (end == std::string::npos)
				end = spec.size();
			if (const auto part = spec.substr(begin, end - begin); !part.empty())
				roots.push_back(argactions::path(part));
		}

		struct listing {
			const nlohmann::json* Config;
			const nlohmann::json* Source;
			const nlohmann::json* Target;
			std::vector<std::string> Paths;   // lowercased
		};
		std::vector<nlohmann::json> presets;
		for (const auto& file : release_ordered_presets(roots)) {
			try {
				std::ifstream f(file, std::ios::binary);
				presets.push_back(nlohmann::json::parse(f));
			} catch (const std::exception&) {
				// read_joins has already said so.
			}
		}
		std::vector<listing> listings;
		std::set<std::string> candidates;
		for (const auto& preset : presets) {
			// `apply` reads only a MusicImportConfig this way; a matchset has no segments.
			if (!preset.contains("searchDirectories") || !preset.contains("items") || !preset["items"].is_array())
				continue;
			for (const auto& item : preset["items"]) {
				const auto source = item.find("source");
				const auto target = item.find("target");
				if (source == item.end() || target == item.end())
					continue;
				auto graph = false;
				if (source->is_object())
					for (const auto& [_n, spec2] : source->items())
						graph = graph || (spec2.is_object() && spec2.contains("inputFiles"));
				// By address into the preset, which outlives this: a listing is resolved later,
				// and a one-element array built here to iterate a lone target would not.
				std::vector<const nlohmann::json*> targets;
				if (target->is_array())
					for (const auto& one : *target)
						targets.push_back(&one);
				else
					targets.push_back(&*target);
				for (const auto* pone : targets) {
					const auto& one = *pone;
					if (!one.is_object() || one.value("enable", true) == false)
						continue;
					listing l{&preset, &*source, &one};
					if (const auto p = one.find("path"); p != one.end()) {
						if (p->is_string())
							l.Paths.push_back(lower_ascii(p->get<std::string>()));
						else if (p->is_array())
							for (const auto& x : *p)
								if (x.is_string())
									l.Paths.push_back(lower_ascii(x.get<std::string>()));
					}
					auto candidate = graph;
					if (const auto segs = one.find("segments"); segs != one.end() && segs->is_array()) {
						candidate = candidate || segs->size() > 1;
						for (const auto& seg : *segs)
							if (const auto filters = seg.find("sourceFilters"); filters != seg.end() && filters->is_object())
								for (const auto& [_n, fl] : filters->items())
									candidate = candidate || (fl.is_string() && fl.get<std::string>().find("asplit") != std::string::npos);
					}
					if (candidate)
						candidates.insert(l.Paths.begin(), l.Paths.end());
					listings.push_back(std::move(l));
				}
			}
		}

		std::map<std::string, std::vector<apply_segment>> res;
		std::set<std::string> decided;
		for (const auto& l : listings) {
			if (std::ranges::none_of(l.Paths, [&](const auto& p) { return candidates.contains(p) && !decided.contains(p); }))
				continue;
			std::optional<config_target> read;
			try {
				read = read_config_target(ostDir, *l.Config, *l.Source, *l.Target, unresolved);
			} catch (const std::exception& e) {
				unresolved.emplace_back(collect_config_target_path(*l.Target), e.what());
			}
			if (!read)
				continue;
			for (const auto& p : l.Paths)
				if (decided.insert(p).second && candidates.contains(p))
					res.emplace(p, read->Segments);
		}
		return res;
	}

	// Mean level of one stretch of a file, in dB. The step across a join is built from two of
	// these on each side.
	std::optional<double> level_db(std::span<const float> x, double fromSeconds, double toSeconds) {
		if (fromSeconds < 0. || toSeconds <= fromSeconds)
			return std::nullopt;
		const auto begin = static_cast<size_t>(fromSeconds * AnalysisRateHz);
		const auto end = static_cast<size_t>(toSeconds * AnalysisRateHz);
		if (end > x.size() || end <= begin)
			return std::nullopt;
		auto e = 0.;
		for (size_t i = begin; i < end; ++i)
			e += static_cast<double>(x[i]) * x[i];
		// Floored, so digital silence reads as very quiet rather than as -120 dB, which would
		// otherwise dominate any difference it takes part in.
		return (std::max)(-90., 10. * std::log10(e / static_cast<double>(end - begin) + 1e-12));
	}

	// The loop-seam click ratio, at a point inside a file rather than across a wrap: the jump
	// between the last sample of one segment and the first of the next, against the largest
	// step the audio takes on its own either side of it.
	std::optional<double> join_seam_ratio(std::span<const float> x, size_t at, size_t rate) {
		const auto w = (std::max<size_t>)(4, static_cast<size_t>(0.02 * static_cast<double>(rate)));
		if (at < w + 1 || at + w >= x.size())
			return std::nullopt;
		const auto jump = std::abs(static_cast<double>(x[at]) - static_cast<double>(x[at - 1]));
		auto peak = 0.;
		for (size_t i = at - w; i + 1 < at; ++i)
			peak = (std::max)(peak, std::abs(static_cast<double>(x[i + 1]) - static_cast<double>(x[i])));
		for (size_t i = at; i + 1 < at + w; ++i)
			peak = (std::max)(peak, std::abs(static_cast<double>(x[i + 1]) - static_cast<double>(x[i])));
		if (peak <= 0.)
			return std::nullopt;
		return jump / peak;
	}

	// The join that looks worst: the one whose window falls furthest below its own control,
	// or, where none has a control, the one with the lowest match.
	const join_comparison* worst_join(const std::vector<join_comparison>& joins) {
		const join_comparison* worst = nullptr;
		auto worstGap = 0.;
		for (const auto& j : joins) {
			if (!j.Window.Valid)
				continue;
			const auto gap = j.Control.Valid ? j.Window.Match - j.Control.Match : j.Window.Match - 1.;
			if (!worst || gap < worstGap) {
				worst = &j;
				worstGap = gap;
			}
		}
		return worst;
	}

	// How far below its own median a file may sit and still be carrying something audible.
	constexpr double AudibleWithinDb = 30.;
	// How far the build may sit from the game on one side of a join before that side is wrong.
	constexpr double JoinSideDb = 10.;
	// How far a join window's match may fall below its control. Calibrated over the 86 joins
	// with room either side: -0.011 at the median, -0.132 at the tenth percentile; the two
	// joins judged by ear sit at -0.222 (reported wrong) and +0.011 (reported fixed).
	constexpr double JoinMatchGap = 0.10;

	// What, if anything, is wrong at one join. Empty when nothing is.
	std::string join_problem(const join_comparison& j, double seamThreshold) {
		std::string res;
		const auto add = [&](std::string_view what) {
			if (!res.empty())
				res += "; ";
			res += what;
		};
		// Quieter only counts where the game is audible, louder only where the build is, so a
		// silence in both is never a finding.
		if (j.HasBefore && j.BeforeDb <= -JoinSideDb && j.GameBeforeRel >= -AudibleWithinDb)
			add("outgoing runs out before the join");
		if (j.HasAfter && j.AfterDb <= -JoinSideDb && j.GameAfterRel >= -AudibleWithinDb)
			add("incoming too quiet after it");
		if (j.HasAfter && j.AfterDb >= JoinSideDb && j.BuiltAfterRel >= -AudibleWithinDb)
			add("plays where the game is silent after it");
		if (j.HasBefore && j.BeforeDb >= JoinSideDb && j.BuiltBeforeRel >= -AudibleWithinDb)
			add("plays where the game is silent before it");
		if (j.Window.Valid && j.Control.Valid && j.Window.Match < j.Control.Match - JoinMatchGap)
			add("different material across the join");
		if (j.HasSeam && j.Seam > seamThreshold)
			add("clicks at the cut");
		return res;
	}

	bool join_is_suspect(const join_comparison& j, double seamThreshold) {
		return !join_problem(j, seamThreshold).empty();
	}

	nlohmann::json curve_json(const detector_curve& c) {
		using kind = detector_curve::kind;
		if (c.Kind == kind::Unknown)
			return {{"kind", "unknown"}};
		nlohmann::json x{{"kind", c.Kind == kind::Steady ? "steady" : c.Kind == kind::In ? "in" : "out"},
			{"start", c.Start}, {"end", c.End}};
		if (c.Kind == kind::Steady)
			x["mid"] = c.Mid;
		else
			x.update({{"t10", c.T10}, {"t50", c.T50}, {"t90", c.T90}, {"shape", c.Shape}});
		return x;
	}

	nlohmann::json region_json(const detector_region_result& g) {
		nlohmann::json x{{"from", g.From}, {"to", g.To}, {"joins", g.Joins}};
		if (!g.Error.empty()) {
			x["error"] = g.Error;
			return x;
		}
		x["copies"] = g.Copies;
		x["shape"] = {{"peak", g.PeakShape}, {"at", g.PeakShapeAt}, {"area", g.Area}, {"wrongSeconds", g.WrongSeconds}};
		x["level"] = {{"peakDb", g.PeakLevelDb}, {"at", g.PeakLevelAt}};
		x["relLagMs"] = g.RelLagMs;
		x["total"] = g.Total;
		x["r2"] = {{"game", g.R2Game}, {"build", g.R2Build}};
		auto flags = nlohmann::json::array();
		if (g.ShapeFlag)
			flags.push_back("shape");
		if (g.LevelFlag)
			flags.push_back("level");
		if (g.LagFlag)
			flags.push_back("lag");
		x["flags"] = std::move(flags);
		if (const auto p = g.problem(); !p.empty())
			x["problem"] = p;
		auto per = nlohmann::json::array();
		for (const auto& c : g.PerCopy) {
			nlohmann::json y{{"copy", c.Name}, {"game", curve_json(c.Game)}, {"build", curve_json(c.Build)},
				{"lagGameMs", c.LagGameMs}, {"lagBuildMs", c.LagBuildMs},
				{"lagWindows", {c.LagWindowsGame, c.LagWindowsBuild}}, {"lagRho", {c.LagRhoGame, c.LagRhoBuild}},
				{"identFraction", c.IdentFraction}};
			if (c.Dt50)
				y["dt50"] = *c.Dt50;
			if (c.KindMismatch)
				y["kindMismatch"] = true;
			per.push_back(std::move(y));
		}
		x["perCopy"] = std::move(per);
		return x;
	}

	// How far past its thresholds a region sits, for ranking the worst first.
	double region_severity(const detector_region_result& g) {
		if (!g.Error.empty())
			return 0.;
		// A wrong arrangement or level step before any copy that is merely out of step: the
		// first is heard as a different piece of music at the join, the second as a smear.
		return (g.flagged() ? 1000. : 0.) + (std::max)(g.PeakShape / 0.14, std::abs(g.PeakLevelDb) / 3.5)
			+ (g.LagFlag ? g.RelLagMs / 100. : 0.);
	}

	struct row {
		std::string Target;
		double Weighted = 0., Plain = 0.;
		double BuiltSeconds = 0., GameSeconds = 0.;
		envelope_comparison Envelope;
		std::vector<silence_run> Silence;
		seam_result Seam;
		spectrum_comparison Spectrum;
		// The seconds before the loop end, and a same-length control from mid-loop.
		window_comparison Tail, Control;
		// Where the build sits against the game's own file, at the head and at the loop
		// start. The head is not what `apply` aligned on, so it is an independent check;
		// the difference between the two is clock drift rather than a bad offset.
		lag_probe Head, AtLoop;
		lag_span Lags;
		level_offset Level;
		// Every place two segments meet, when --preset says where they are.
		std::vector<join_comparison> Joins;
		// The game's own loop and length, which is what the join detector's regions are
		// clipped to: past the game's loop end nothing is heard, whatever the build holds.
		loop_info GameLoop;
		size_t BuiltChannels = 0;
		// Both entries' payloads, kept past the per-file pass for a target the join detector
		// will read: extracting them costs a full Vorbis decode each, just for the loop points.
		std::filesystem::path KeptBuilt, KeptGame;
		// The join detector's verdict per region, with --ost; or why this target could not
		// be judged at all.
		std::vector<detector_region_result> Regions;
		std::string RegionsNotAnalysable;
		std::string Error;
	};

	// The sample rate of a decoded file, for picking the band both sides can carry.
	size_t probe_rate(const std::filesystem::path& ffprobe, const std::filesystem::path& file) {
		try {
			const auto out = run_process_capture_stdout(ffprobe, {
				L"-v", L"error", L"-select_streams", L"a:0",
				L"-show_entries", L"stream=sample_rate", L"-of", L"csv=p=0",
				file.wstring(),
			});
			return static_cast<size_t>(std::stoul(std::string(out.begin(), out.end())));
		} catch (const std::exception&) {
			return 44100;
		}
	}

	double total_silence(const std::vector<silence_run>& runs) {
		double acc = 0;
		for (const auto& r : runs)
			acc += r.ToSeconds - r.FromSeconds;
		return acc;
	}
}

int cmd_verify(const std::vector<std::string>& args) {
	argparse::ArgumentParser parser("scdtool verify");
	try {
		parser
			.add_description("Score every .scd of a build against the game's own file it replaces.")
			.add_epilog(
				"Four measurements, because each is blind to what the others catch:\n"
				"  score     level-weighted log-mel cosine over the span the two share. The verdict,\n"
				"            and blind to anything local -- a two-second dropout in a two-minute piece\n"
				"            moves it about a percent.\n"
				"  envelope  the peak envelope a seekbar draws. `hole` is the worst sustained place\n"
				"            the build is quieter than the game's own file, which the score cannot see.\n"
				"            Where `span` is under 2 dB the game's own envelope is a flat block and the\n"
				"            correlations measure noise; read `dev` instead.\n"
				"  silence   stretches the build is digitally silent and the game is not, reported\n"
				"            against the loop end because silence past it is never played.\n"
				"  tail      the last --tail-seconds before the loop end, against a control window\n"
				"            of the same length from mid-loop. Everything else here averages over\n"
				"            the file and cannot see five bad seconds in two hundred and ninety,\n"
				"            which is exactly where a recording runs out or a loop-out takes over.\n"
				"            Read the two together: a low tail *and* a low control is a recording\n"
				"            that matches poorly throughout, while a low tail against a good\n"
				"            control is something the build did at the loop end.\n"
				"  head      how far the build sits from the game's own file at the start, in\n"
				"            milliseconds, measured from where the game's file first carries\n"
				"            something. A lag rather than a score, because a score cannot tell a\n"
				"            different master from a misaligned one. `apply` fits its alignment\n"
				"            around the loop start, so the head is an independent check; the same\n"
				"            probe at the loop start is reported beside it, and the difference\n"
				"            between the two is clock drift rather than a wrong offset.\n"
				"  joins     with --preset, every place two segments meet: a window centred on the\n"
				"            crossfade against a control from inside the outgoing segment; each side\n"
				"            of it against the game, which catches an outgoing recording that has run\n"
				"            out before the join and an incoming one playing where the game is\n"
				"            silent; and a click ratio at a hard cut. A join leaves no mark in the\n"
				"            file, so the presets are the only place to learn where one is.\n"
				"  seam      whether the loop point clicks, judged from the built file alone. Above\n"
				"            about 1 the seam is a bigger jump than anything happening near it.\n"
				"  regions   with --preset and --ost, every item that mixes more than one copy of a\n"
				"            recording -- segments, or an asplit/adelay sum -- is modelled copy by copy:\n"
				"            per quarter second, how much of each copy the game plays and how much the\n"
				"            build does, over 20 s either side of each join. `shape` is the part of the\n"
				"            game the build's copy ratio fails to explain (flagged over 0.14 for a\n"
				"            second), `level` the build's step against the game there (over 3.5 dB),\n"
				"            and copies out of step with each other by more than 2 ms are reported\n"
				"            apart. The only check here that can tell a wrong crossfade from a\n"
				"            different master, which lower every spectral match alike.\n"
				"\n"
				"The csv columns are target,weighted,plain,built_seconds,game_seconds and then the\n"
				"envelope and silence fields, so an existing build_scores.csv reader keeps working.");
		parser.add_argument("--game").required().help(R"(game installation path, or :global/:china/:korea to autodetect)");
		parser.add_argument("--built").required().help("directory of .scd files produced by `scdtool apply`")
			.action(argactions::existing_directory);
		parser.add_argument("--output").help("write the per-file table here; omit for stdout");
		parser.add_argument("--format").default_value(std::string("csv")).help("csv (default) or json");
		parser.add_argument("--ffmpeg").default_value(std::string("ffmpeg")).help("path to ffmpeg executable");
		parser.add_argument("--ffprobe").default_value(std::string("ffprobe")).help("path to ffprobe executable, used to pick the shared sample rate for --spectrum");
		parser.add_argument("--spectrum").default_value(false).implicit_value(true).help("also compare third-octave spectra to the lower of the two files' rates: tilt, worst band, bandwidth edge, and the worst band-and-time hole. Costs a second decode of both files, and is the only measurement here that sees above 8 kHz");
		parser.add_argument("--entry-index").default_value(0u).scan<'u', uint32_t>().help("sound entry index to compare (default: 0)");
		parser.add_argument("--worst").default_value(20u).scan<'u', uint32_t>().help("how many of the worst files to print (default: 20)");
		parser.add_argument("--min-score").default_value(0.95).scan<'g', double>().help("call out files scoring below this");
		parser.add_argument("--seam-threshold").default_value(1.0).scan<'g', double>().help("call out loop seams above this ratio");
		parser.add_argument("--tail-seconds").default_value(10.0).scan<'g', double>().help("how long the window before the loop end is, in seconds (default: 10). Raise it past the longest crossfade in the presets being judged -- a crossfade has to sit inside the window that judges it");
		parser.add_argument("--head-seconds").default_value(3.0).scan<'g', double>().help("how long the head window is, in seconds (default: 3), measured from the first moment the game's own file is carrying something");
		parser.add_argument("--max-lag-ms").default_value(25.0).scan<'g', double>().help("how far either way the head and loop-start lag is searched, in milliseconds (default: 25)");
		parser.add_argument("--preset").default_value(std::string()).help("the presets the build came from -- files or directories, comma-separated -- so every place two segments meet can be judged. A join leaves no mark in the .scd it produces, so without this the joins are not checked");
		parser.add_argument("--ost").default_value(std::string()).help("the recordings the build was made from, as `apply --ost` took them. With --preset, runs the join detector on every item that mixes more than one copy of a recording; without it the joins are judged by level and spectrum only");
		parser.add_argument("--join-seconds").default_value(6.0).scan<'g', double>().help("how long the window centred on each join is, in seconds (default: 6; never less than twice the crossfade plus a second either side)");
		parser.parse_args(args);
	} catch (const std::exception& e) {
		std::cerr << "Error parsing arguments. Use `verify -h` to show help.\n" << e.what() << '\n';
		return -1;
	}

	try {
		const xivres::installation installation(argactions::installation_root(parser.get<std::string>("--game")));
		const auto builtDir = parser.get<std::filesystem::path>("--built");
		const auto ffmpeg = argactions::path(parser.get<std::string>("--ffmpeg"));
		const auto entryIndex = parser.get<uint32_t>("--entry-index");
		const auto asJson = parser.get<std::string>("--format") == "json";
		const auto worstCount = parser.get<uint32_t>("--worst");
		const auto minScore = parser.get<double>("--min-score");
		const auto seamThreshold = parser.get<double>("--seam-threshold");
		const auto ffprobe = argactions::path(parser.get<std::string>("--ffprobe"));
		const auto withSpectrum = parser.get<bool>("--spectrum");
		const auto tailSeconds = parser.get<double>("--tail-seconds");
		const auto headSeconds = parser.get<double>("--head-seconds");
		const auto maxLagMs = parser.get<double>("--max-lag-ms");
		const auto joinSeconds = parser.get<double>("--join-seconds");
		const auto joinsByTarget = read_joins(parser.get<std::string>("--preset"));
		const auto ostSpec = parser.get<std::string>("--ost");
		std::map<std::string, std::vector<apply_segment>> detectorTargets;
		if (!ostSpec.empty()) {
			if (parser.get<std::string>("--preset").empty())
				throw std::runtime_error("--ost only names the recordings; --preset says what was built from them.");
			std::vector<std::pair<std::string, std::string>> unresolved;
			detectorTargets = read_detector_targets(parser.get<std::string>("--preset"), argactions::path(ostSpec), unresolved);
			std::cerr << std::format("{} target(s) the join detector may judge{}", detectorTargets.size(),
				unresolved.empty() ? "." : std::format("; {} listing(s) did not resolve and were passed over, as apply does:", unresolved.size())) << '\n';
			for (size_t i = 0; i < (std::min<size_t>)(unresolved.size(), 4); ++i)
				std::cerr << std::format("   {}: {}", unresolved[i].first, unresolved[i].second) << '\n';
		}
		if (!parser.get<std::string>("--preset").empty()) {
			size_t count = 0;
			for (const auto& [_t, j] : joinsByTarget)
				count += j.size();
			std::cerr << std::format("{} join(s) across {} target(s) from the presets.",
				count, joinsByTarget.size()) << '\n';
		}

		std::vector<std::pair<std::filesystem::path, std::string>> pairs;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(builtDir)) {
			if (!entry.is_regular_file() || entry.path().extension() != L".scd")
				continue;
			const auto name = entry.path().filename().wstring();
			if (name.ends_with(L".orig.scd"))
				continue;
			auto rel = std::filesystem::relative(entry.path(), builtDir).generic_string();
			pairs.emplace_back(entry.path(), std::move(rel));
		}
		std::ranges::sort(pairs, [](const auto& a, const auto& b) { return a.second < b.second; });
		if (pairs.empty()) {
			std::cerr << "No .scd files under " << u8(builtDir) << ".\n";
			return -1;
		}

		const process_temp_directory tempRoot(L"verify");
		const auto& tempDir = tempRoot.path();
		std::atomic_size_t counter = 0;
		std::atomic_size_t done = 0;
		std::mutex progressMutex;
		std::vector<row> rows(pairs.size());

		std::cerr << std::format("Verifying {} file(s)...", pairs.size()) << '\n';
		parallel_for(pairs.size(), [&](size_t index) {
			const auto& [builtPath, target] = pairs[index];
			auto& r = rows[index];
			r.Target = target;

			const auto stem = tempDir / std::format(L"v{}", counter.fetch_add(1));
			std::filesystem::path builtAudio, gameAudio;
			try {
				const auto builtLoop = unwrap_entry(std::make_shared<xivres::file_stream>(builtPath),
					entryIndex, stem.wstring() + L"_a", builtAudio);
				r.BuiltChannels = builtLoop.Channels;
				r.GameLoop = unwrap_entry(installation.get_file(target), entryIndex, stem.wstring() + L"_b", gameAudio);

				const auto a = decode_mono_float(ffmpeg, builtAudio);
				const auto b = decode_mono_float(ffmpeg, gameAudio);
				r.BuiltSeconds = static_cast<double>(a.size()) / AnalysisRateHz;
				r.GameSeconds = static_cast<double>(b.size()) / AnalysisRateHz;

				if (const auto score = build_score(a, b); score.Valid) {
					r.Weighted = score.Weighted;
					r.Plain = score.Plain;
				} else {
					r.Error = "too short to judge";
				}

				const auto bucket = static_cast<size_t>(std::llround(
					EnvelopeBucketSeconds * static_cast<double>(AnalysisRateHz)));
				const auto n = (std::min)(a.size(), b.size());
				const auto ea = peak_envelope_db(std::span(a).subspan(0, n), bucket);
				const auto eb = peak_envelope_db(std::span(b).subspan(0, n), bucket);
				r.Envelope = compare_envelopes(ea, eb);

				// Against the built file's own loop end: that is what the game plays to, and
				// silence past it is never heard.
				const auto loopEndSeconds = builtLoop.EndSample && builtLoop.Rate
					? static_cast<double>(builtLoop.EndSample) / static_cast<double>(builtLoop.Rate)
					: 0.;
				r.Silence = silence_gaps(ea, eb, loopEndSeconds);

				// What the loop actually ends on, against a control from the middle of the
				// same loop. Needs no extra decode -- both files are already here at the
				// analysis rate. Skipped where the loop is too short to hold three windows,
				// since the control would then overlap the tail and compare it with itself.
				const auto loopStartSeconds = builtLoop.Rate
					? static_cast<double>(builtLoop.StartSample) / static_cast<double>(builtLoop.Rate)
					: 0.;
				if (loopEndSeconds > 0. && builtLoop.Rate && tailSeconds > 0.
					&& loopEndSeconds - loopStartSeconds >= 3. * tailSeconds) {
					r.Tail = measure_window(a, b, loopEndSeconds - tailSeconds, loopEndSeconds);
					const auto mid = (loopStartSeconds + loopEndSeconds) / 2.;
					r.Control = measure_window(a, b, mid - tailSeconds / 2., mid + tailSeconds / 2.);
				}

				// Is the first sync right? Taken from where the game's file starts carrying
				// something rather than from zero, because an entry that opens with silence
				// or a fade-in would otherwise have two silences correlated against each
				// other. The loop start gets the same probe: that *is* what `apply` aligned
				// on, so the two together say whether a head error is a bad offset or drift.
				if (headSeconds > 0. && maxLagMs > 0.) {
					const auto audible = first_audible_second(b);
					r.Head = probe_lag(a, b, (std::max)(audible, maxLagMs / 1000.),
						headSeconds, maxLagMs);
					if (loopStartSeconds > 0.)
						r.AtLoop = probe_lag(a, b, loopStartSeconds, headSeconds, maxLagMs);
					const auto end = static_cast<double>((std::min)(a.size(), b.size())) / AnalysisRateHz;
					r.Lags = probe_lag_span(a, b, (std::max)(audible, maxLagMs / 1000.),
						end - maxLagMs / 1000., headSeconds, maxLagMs);
				}
				r.Level = measure_level(a, b);

				// Every place two segments meet. The window is centred on the crossfade and
				// always wide enough to hold it with a second to spare either side; the control
				// comes from inside the outgoing segment, clear of the join before it, so a
				// recording that matches poorly throughout is not blamed on its joins.
				auto targetKey = target;
				std::ranges::transform(targetKey, targetKey.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (const auto it = joinsByTarget.find(targetKey); it != joinsByTarget.end()) {
					// Each file's own typical level, for judging whether a side is audible.
					const auto median_db = [](std::span<const float> x) {
						std::vector<double> v;
						for (size_t i = 0; i + AnalysisRateHz <= x.size(); i += AnalysisRateHz) {
							auto e = 0.;
							for (size_t k = i; k < i + AnalysisRateHz; ++k)
								e += static_cast<double>(x[k]) * x[k];
							v.push_back((std::max)(-90., 10. * std::log10(e / AnalysisRateHz + 1e-12)));
						}
						if (v.empty())
							return -90.;
						std::ranges::nth_element(v, v.begin() + static_cast<ptrdiff_t>(v.size() / 2));
						return v[v.size() / 2];
					};
					const auto builtMedian = median_db(a);
					const auto gameMedian = median_db(b);
					const auto builtEnd = static_cast<double>(a.size()) / AnalysisRateHz;
					std::vector<float> native;
					auto previousEnd = 0.;
					for (const auto& j : it->second) {
						join_comparison jc{.Join = j};
						const auto width = (std::max)(joinSeconds, 2. * j.CrossfadeSeconds + 2.);
						const auto centre = j.AtSeconds + j.CrossfadeSeconds / 2.;
						// Not scored unless there is at least a second of build after the
						// crossfade: a window clamped to the side before the join is judging
						// the outgoing segment, not the join.
						if (j.AtSeconds + j.CrossfadeSeconds + 1. <= builtEnd)
							jc.Window = measure_window(a, b, centre - width / 2.,
								(std::min)(centre + width / 2., builtEnd));
						if (j.AtSeconds - 2. * width >= previousEnd)
							jc.Control = measure_window(a, b, j.AtSeconds - 2. * width, j.AtSeconds - width);

						// Each side on its own, against the game at the same moment.
						const auto bb = level_db(a, j.AtSeconds - 1.5, j.AtSeconds - 0.5);
						const auto gb = level_db(b, j.AtSeconds - 1.5, j.AtSeconds - 0.5);
						const auto ba = level_db(a, j.AtSeconds + j.CrossfadeSeconds + 0.5, j.AtSeconds + j.CrossfadeSeconds + 1.5);
						const auto ga = level_db(b, j.AtSeconds + j.CrossfadeSeconds + 0.5, j.AtSeconds + j.CrossfadeSeconds + 1.5);
						if (bb && gb) {
							jc.BeforeDb = *bb - *gb;
							jc.BuiltBeforeRel = *bb - builtMedian;
							jc.GameBeforeRel = *gb - gameMedian;
							jc.HasBefore = true;
						}
						if (ba && ga) {
							jc.AfterDb = *ba - *ga;
							jc.BuiltAfterRel = *ba - builtMedian;
							jc.GameAfterRel = *ga - gameMedian;
							jc.HasAfter = true;
						}

						// A hard cut is the one join that can click. At the file's own rate for
						// the reason the loop seam is: resampling smears a single-sample jump.
						if (j.CrossfadeSeconds <= 0. && builtLoop.Rate) {
							if (native.empty())
								native = decode_mono_float(ffmpeg, builtAudio, builtLoop.Rate);
							const auto at = static_cast<size_t>(std::llround(j.AtSeconds * static_cast<double>(builtLoop.Rate)));
							if (const auto ratio = join_seam_ratio(native, at, builtLoop.Rate)) {
								jc.Seam = *ratio;
								jc.HasSeam = true;
							}
						}
						previousEnd = j.AtSeconds + j.CrossfadeSeconds;
						r.Joins.push_back(std::move(jc));
					}
				}

				// At the file's own rate, not the analysis rate. A click is a single-sample
				// discontinuity, and resampling to 16 kHz spreads it over its neighbours
				// until it reads as ordinary content -- so the loop points stay the sample
				// indices the header states rather than being scaled into another grid.
				if (builtLoop.EndSample > builtLoop.StartSample && builtLoop.Rate) {
					const auto native = decode_mono_float(ffmpeg, builtAudio, builtLoop.Rate);
					r.Seam = loop_seam_ratio(native, builtLoop.StartSample, builtLoop.EndSample,
						builtLoop.Rate);
				}

				// The spectrogram pane, at its own rate rather than the 16 kHz the other
				// three share -- the whole point of it is the band above 8 kHz that nothing
				// else here has ever been able to see. That means a second decode of both
				// files, which is why it is opt-in.
				if (withSpectrum) {
					const auto rate = (std::min)({SpectrumMaxRateHz,
						probe_rate(ffprobe, builtAudio), probe_rate(ffprobe, gameAudio)});
					const auto wa = decode_mono_float(ffmpeg, builtAudio, rate);
					const auto wb = decode_mono_float(ffmpeg, gameAudio, rate);
					r.Spectrum = compare_spectra(wa, wb, rate);
				}
			} catch (const std::exception& e) {
				r.Error = std::string(e.what()).substr(0, 90);
			}
			if (r.Error.empty() && detectorTargets.contains(lower_ascii(target))) {
				r.KeptBuilt = std::exchange(builtAudio, {});
				r.KeptGame = std::exchange(gameAudio, {});
			}
			for (const auto& p : {builtAudio, gameAudio}) {
				if (!p.empty()) {
					std::error_code ec;
					std::filesystem::remove(p, ec);
				}
			}
			if (const auto n = done.fetch_add(1) + 1; n % 250 == 0) {
				const auto lock = std::scoped_lock(progressMutex);
				std::cerr << std::format("   {}/{}", n, pairs.size()) << '\n';
			}
		});

		// The join detector, over every region of every multi-copy target. A pass of its own
		// rather than part of the one above: a region is the unit of work, not a file -- the
		// credits rolls hold nine each and would otherwise run on one thread while the rest sit
		// idle -- and the recordings are shared between targets, so each is rendered once.
		const auto detectorStarted = std::chrono::steady_clock::now();
		size_t detectorTargetsRun = 0;
		if (!detectorTargets.empty()) {
			// Everything decoded is on disk and mapped, and dropped once the last region that
			// reads it is done: a run holds a few gigabytes of renders at most at a time,
			// rather than every recording of the library.
			struct shared_decode {
				std::mutex Lock;
				bool Tried = false;
				std::string Error;
				std::atomic_size_t Remaining = 0;
			};
			struct target_audio : shared_decode {
				std::shared_ptr<mapped_audio_file> Game, Build;
			};
			struct render_audio : shared_decode {
				std::filesystem::path File;
				std::string Render;
				std::shared_ptr<mapped_audio_file> Audio;
			};
			struct region_job {
				size_t Row = 0, Index = 0;
			};

			std::mutex probeLock;
			std::map<std::filesystem::path, std::pair<size_t, size_t>> probed;   // (rate, channels)
			const auto probe = [&](const std::filesystem::path& file) {
				{
					const auto lock = std::scoped_lock(probeLock);
					if (const auto it = probed.find(file); it != probed.end())
						return it->second;
				}
				std::pair<size_t, size_t> v{44100, 2};
				try {
					const auto out = run_process_capture_stdout(ffprobe, {
						L"-v", L"error", L"-select_streams", L"a:0",
						L"-show_entries", L"stream=sample_rate,channels", L"-of", L"csv=p=0",
						file.wstring(),
					});
					const std::string text(out.begin(), out.end());
					const auto comma = text.find(',');
					v = {std::stoul(text.substr(0, comma)), std::stoul(text.substr(comma + 1))};
				} catch (const std::exception&) {
				}
				const auto lock = std::scoped_lock(probeLock);
				return probed.emplace(file, v).first->second;
			};

			std::vector<detector_plan> plans(rows.size());
			std::vector<std::vector<detector_region_span>> spans(rows.size());
			std::vector<std::unique_ptr<target_audio>> audio(rows.size());
			std::map<std::string, std::unique_ptr<render_audio>> renders;
			const auto renderKey = [](const detector_copy& c) {
				return u8(c.File) + "|" + c.Render;
			};
			std::vector<region_job> jobs;
			for (size_t i = 0; i < rows.size(); ++i) {
				auto& r = rows[i];
				if (r.KeptGame.empty())
					continue;
				const auto& segments = detectorTargets.at(lower_ascii(r.Target));
				plans[i] = plan_copies(segments, r.GameLoop.Channels,
					[&](const std::filesystem::path& f) { return static_cast<double>(probe(f).first); });
				const auto& plan = plans[i];
				if (!plan.MultiCopy)
					continue;
				if (!plan.NotAnalysable.empty()) {
					r.RegionsNotAnalysable = plan.NotAnalysable;
					continue;
				}
				// The game's loop end, else its length: nothing past it is heard.
				const auto rate = static_cast<double>(r.GameLoop.Rate ? r.GameLoop.Rate : 1);
				const auto duration = static_cast<double>(r.GameLoop.TotalSamples) / rate;
				const auto end = (std::min)(r.GameLoop.EndSample ? static_cast<double>(r.GameLoop.EndSample) / rate : duration, duration);
				spans[i] = detector_regions(plan.Events, end);
				if (spans[i].empty())
					continue;
				++detectorTargetsRun;
				r.Regions.resize(spans[i].size());
				audio[i] = std::make_unique<target_audio>();
				audio[i]->Remaining = spans[i].size();
				std::set<std::string> used;
				for (const auto& c : plan.Copies)
					used.insert(renderKey(c));
				for (const auto& c : plan.Copies) {
					auto& entry = renders[renderKey(c)];
					if (!entry) {
						entry = std::make_unique<render_audio>();
						entry->File = c.File;
						entry->Render = c.Render;
					}
				}
				for (const auto& key : used)
					renders.at(key)->Remaining += spans[i].size();
				for (size_t k = 0; k < spans[i].size(); ++k)
					jobs.push_back({i, k});
			}

			if (!jobs.empty()) {
				std::cerr << std::format("Join detector: {} region(s) across {} target(s), {} recording render(s)...",
					jobs.size(), detectorTargetsRun, renders.size()) << '\n';
				std::atomic_size_t regionsDone = 0, renderCounter = 0;
				// Four cores left free: this pass is minutes of solid FFT work, and a machine
				// with none spare is unusable for its duration.
				const auto threads = (std::max)(1u, std::thread::hardware_concurrency() > 4
					? std::thread::hardware_concurrency() - 4 : 1u);
				parallel_for(jobs.size(), [&](size_t jobIndex) {
					const auto [i, k] = jobs[jobIndex];
					auto& r = rows[i];
					auto& ta = *audio[i];
					auto& result = r.Regions[k];
					std::shared_ptr<mapped_audio_file> game, build;
					{
						const auto lock = std::scoped_lock(ta.Lock);
						if (!ta.Tried) {
							ta.Tried = true;
							try {
								ta.Game = decode_for_detector(ffmpeg, r.KeptGame, {}, r.GameLoop.Channels,
									tempDir / std::format(L"d{}_game.f32", i));
								ta.Build = decode_for_detector(ffmpeg, r.KeptBuilt, {}, r.BuiltChannels,
									tempDir / std::format(L"d{}_built.f32", i));
							} catch (const std::exception& e) {
								ta.Error = std::string("could not decode: ") + e.what();
							}
							std::error_code ec;
							std::filesystem::remove(r.KeptGame, ec);
							std::filesystem::remove(r.KeptBuilt, ec);
						}
						game = ta.Game;
						build = ta.Build;
					}

					std::vector<std::shared_ptr<mapped_audio_file>> held;
					std::vector<detector_copy_audio> copies;
					std::string error = ta.Error;
					std::set<std::string> seen;
					for (const auto& c : plans[i].Copies) {
						auto& re = *renders.at(renderKey(c));
						{
							const auto lock = std::scoped_lock(re.Lock);
							if (!re.Tried) {
								re.Tried = true;
								try {
									re.Audio = decode_for_detector(ffmpeg, re.File, re.Render, probe(re.File).second,
										tempDir / std::format(L"r{}.f32", renderCounter.fetch_add(1)));
								} catch (const std::exception& e) {
									re.Error = std::format("could not render {}: {}", u8(re.File.filename()), e.what());
								}
							}
							if (re.Audio)
								held.push_back(re.Audio);
							else if (error.empty())
								error = re.Error;
						}
						if (re.Audio)
							copies.push_back({&c, re.Audio->audio()});
					}

					if (error.empty() && game && build) {
						try {
							result = analyse_join_region(game->audio(), build->audio(), r.GameLoop.Channels,
								copies, spans[i][k]);
						} catch (const std::exception& e) {
							error = e.what();
						}
					}
					if (!error.empty()) {
						result = {};
						result.From = spans[i][k].From;
						result.To = spans[i][k].To;
						result.Joins = spans[i][k].Joins;
						result.Error = error.substr(0, 120);
					}

					// Done with what this region read.
					held.clear();
					for (const auto& c : plans[i].Copies) {
						if (!seen.insert(renderKey(c)).second)
							continue;
						auto& re = *renders.at(renderKey(c));
						if (re.Remaining.fetch_sub(1) == 1) {
							const auto lock = std::scoped_lock(re.Lock);
							re.Audio.reset();
						}
					}
					if (ta.Remaining.fetch_sub(1) == 1) {
						const auto lock = std::scoped_lock(ta.Lock);
						ta.Game.reset();
						ta.Build.reset();
					}
					if (const auto n = regionsDone.fetch_add(1) + 1; n % 25 == 0) {
						const auto lock = std::scoped_lock(progressMutex);
						std::cerr << std::format("   regions {}/{}", n, jobs.size()) << '\n';
					}
				}, threads);
			}
			// A target planned but left without regions never had its payloads consumed.
			for (auto& r : rows) {
				std::error_code ec;
				if (!r.KeptGame.empty())
					std::filesystem::remove(r.KeptGame, ec);
				if (!r.KeptBuilt.empty())
					std::filesystem::remove(r.KeptBuilt, ec);
			}
		}
		const auto detectorSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - detectorStarted).count();

		std::vector<const row*> ok, failed;
		for (const auto& r : rows)
			(r.Error.empty() ? ok : failed).push_back(&r);

		std::string table;
		if (asJson) {
			auto out = nlohmann::json::array();
			for (const auto& r : rows) {
				nlohmann::json one{{"target", r.Target}};
				if (!r.Error.empty()) {
					one["error"] = r.Error;
				} else {
					one["weighted"] = r.Weighted;
					one["plain"] = r.Plain;
					one["builtSeconds"] = r.BuiltSeconds;
					one["gameSeconds"] = r.GameSeconds;
					if (r.Envelope.Valid)
						one["envelope"] = {{"rEye", r.Envelope.REye}, {"rFine", r.Envelope.RFine},
							{"dev", r.Envelope.Dev}, {"span", r.Envelope.Span},
							{"hole", r.Envelope.Hole}, {"holeAt", r.Envelope.HoleAt}};
					if (!r.Silence.empty()) {
						auto runs = nlohmann::json::array();
						for (const auto& s : r.Silence)
							runs.push_back({{"from", s.FromSeconds}, {"to", s.ToSeconds},
								{"gameLevelDb", s.GameLevelDb}});
						one["silence"] = std::move(runs);
					}
					if (r.Seam.Valid)
						one["seamRatio"] = r.Seam.Ratio;
					if (r.Spectrum.Valid)
						one["spectrum"] = {{"tilt", r.Spectrum.TiltDbPerDecade},
							{"worstBandDb", r.Spectrum.WorstBandDb}, {"worstBandHz", r.Spectrum.WorstBandHz},
							{"edgeBuiltHz", r.Spectrum.EdgeBuiltHz}, {"edgeGameHz", r.Spectrum.EdgeGameHz},
							{"patchDb", r.Spectrum.PatchDb}, {"patchHz", r.Spectrum.PatchHz},
							{"patchAtSeconds", r.Spectrum.PatchAtSeconds}, {"hfDb", r.Spectrum.HfDb}};
				}
					if (r.Tail.Valid)
						one["tail"] = {{"level", r.Tail.Level}, {"worst", r.Tail.Worst},
							{"match", r.Tail.Match}};
					if (r.Control.Valid)
						one["control"] = {{"level", r.Control.Level}, {"worst", r.Control.Worst},
							{"match", r.Control.Match}};
					if (r.Head.Valid)
						one["head"] = {{"lagMs", r.Head.LagMs}, {"match", r.Head.Match}};
					if (r.AtLoop.Valid)
						one["atLoop"] = {{"lagMs", r.AtLoop.LagMs}, {"match", r.AtLoop.Match}};
					if (r.Lags.Valid)
						one["lags"] = {{"probes", r.Lags.Probes}, {"minMs", r.Lags.MinMs},
							{"maxMs", r.Lags.MaxMs}, {"ppm", r.Lags.Ppm}};
					if (r.Level.Valid)
						one["level"] = {{"medianDb", r.Level.MedianDb}, {"p90AbsDb", r.Level.P90AbsDb}};
					if (!r.Joins.empty()) {
						auto joins = nlohmann::json::array();
						for (const auto& j : r.Joins) {
							nlohmann::json x{{"at", j.Join.AtSeconds}, {"crossfade", j.Join.CrossfadeSeconds},
								{"sameRecording", j.Join.SameRecording}};
							if (j.Window.Valid)
								x["window"] = {{"level", j.Window.Level}, {"worst", j.Window.Worst},
									{"match", j.Window.Match}};
							if (j.Control.Valid)
								x["control"] = {{"level", j.Control.Level}, {"worst", j.Control.Worst},
									{"match", j.Control.Match}};
							if (j.HasBefore)
								x["before"] = {{"db", j.BeforeDb}, {"gameRel", j.GameBeforeRel},
									{"builtRel", j.BuiltBeforeRel}};
							if (j.HasAfter)
								x["after"] = {{"db", j.AfterDb}, {"gameRel", j.GameAfterRel},
									{"builtRel", j.BuiltAfterRel}};
							if (const auto problem = join_problem(j, seamThreshold); !problem.empty())
								x["problem"] = problem;
							if (j.HasSeam)
								x["seam"] = j.Seam;
							joins.push_back(std::move(x));
						}
						one["joins"] = std::move(joins);
					}
					if (!r.Regions.empty()) {
						auto regions = nlohmann::json::array();
						for (const auto& g : r.Regions)
							regions.push_back(region_json(g));
						one["regions"] = std::move(regions);
					}
					if (!r.RegionsNotAnalysable.empty())
						one["regionsNotAnalysable"] = r.RegionsNotAnalysable;
				out.push_back(std::move(one));
			}
			table = out.dump(1);
		} else {
			table = "target,weighted,plain,built_seconds,game_seconds,r_eye,dev,span,hole,hole_at,silence_seconds,seam_ratio";
			if (withSpectrum)
				table += ",tilt_db_per_decade,worst_band_db,worst_band_hz,edge_built_hz,edge_game_hz,patch_db,patch_hz,hf_db";
			table += ",tail_level,tail_worst,tail_match,ctrl_level,ctrl_worst,ctrl_match";
			table += ",head_lag_ms,head_match,loop_lag_ms,loop_match";
			table += ",lag_min_ms,lag_max_ms,lag_ppm,level_db,level_p90_db";
			table += ",joins,joins_wrong,join_at,join_match,join_ctrl_match,join_before_db,join_after_db,join_seam,join_problem";
			table += ",regions,regions_flagged,worst_shape,worst_shape_at,worst_region_level_db,worst_rel_lag_ms,region_problem";
			table += ",error\n";
			for (const auto& r : rows) {
				table += std::format("{},{:.4f},{:.4f},{:.1f},{:.1f},", r.Target, r.Weighted, r.Plain,
					r.BuiltSeconds, r.GameSeconds);
				if (r.Envelope.Valid)
					table += std::format("{:.4f},{:.2f},{:.2f},{:.2f},{:.1f},", r.Envelope.REye,
						r.Envelope.Dev, r.Envelope.Span, r.Envelope.Hole, r.Envelope.HoleAt);
				else
					table += ",,,,,";
				table += std::format("{:.2f},", total_silence(r.Silence));
				table += r.Seam.Valid ? std::format("{:.3f}", r.Seam.Ratio) : "";
				if (withSpectrum) {
					table += r.Spectrum.Valid
						? std::format(",{:.2f},{:.1f},{:.0f},{:.0f},{:.0f},{:.1f},{:.0f},{:.2f}",
							r.Spectrum.TiltDbPerDecade, r.Spectrum.WorstBandDb, r.Spectrum.WorstBandHz,
							r.Spectrum.EdgeBuiltHz, r.Spectrum.EdgeGameHz, r.Spectrum.PatchDb,
							r.Spectrum.PatchHz, r.Spectrum.HfDb)
						// Eight empty fields, one per column above -- a comma short here shifts
						// every later column left by one and the error text lands under hf_db.
						: ",,,,,,,,";
				}
				// A comma short in any of these shifts the error text left by a column.
				table += r.Tail.Valid
					? std::format(",{:.2f},{:.2f},{:.4f}", r.Tail.Level, r.Tail.Worst, r.Tail.Match)
					: ",,,";
				table += r.Control.Valid
					? std::format(",{:.2f},{:.2f},{:.4f}", r.Control.Level, r.Control.Worst, r.Control.Match)
					: ",,,";
				table += r.Head.Valid
					? std::format(",{:.2f},{:.4f}", r.Head.LagMs, r.Head.Match)
					: ",,";
				table += r.AtLoop.Valid
					? std::format(",{:.2f},{:.4f}", r.AtLoop.LagMs, r.AtLoop.Match)
					: ",,";
				table += r.Lags.Valid
					? std::format(",{:.2f},{:.2f},{:.1f}", r.Lags.MinMs, r.Lags.MaxMs, r.Lags.Ppm)
					: ",,,";
				table += r.Level.Valid
					? std::format(",{:.2f},{:.2f}", r.Level.MedianDb, r.Level.P90AbsDb)
					: ",,";
				// The worst join only; every join is in the JSON.
				{
					// The worst join by what is wrong with it, falling back to the lowest match;
					// every join is in the JSON.
					const join_comparison* shown = nullptr;
					size_t wrong = 0;
					for (const auto& j : r.Joins)
						if (join_is_suspect(j, seamThreshold)) {
							++wrong;
							if (!shown)
								shown = &j;
						}
					if (!shown)
						shown = worst_join(r.Joins);
					if (!shown && !r.Joins.empty())
						shown = &r.Joins.front();
					if (shown) {
						auto problem = join_problem(*shown, seamThreshold);
						std::ranges::replace(problem, ',', ' ');
						table += std::format(",{},{},{:.2f},{},{},{},{},{},{}", r.Joins.size(), wrong,
							shown->Join.AtSeconds,
							shown->Window.Valid ? std::format("{:.4f}", shown->Window.Match) : "",
							shown->Control.Valid ? std::format("{:.4f}", shown->Control.Match) : "",
							shown->HasBefore ? std::format("{:.2f}", shown->BeforeDb) : "",
							shown->HasAfter ? std::format("{:.2f}", shown->AfterDb) : "",
							shown->HasSeam ? std::format("{:.3f}", shown->Seam) : "",
							problem);
					} else {
						table += ",,,,,,,,,";
					}
				}
				// The join detector: the worst of every region, and what is wrong with the ones
				// that flag. Every region, with its per-copy curves, is in the JSON.
				if (!r.Regions.empty()) {
					size_t flagged = 0;
					const detector_region_result* worstShape = nullptr;
					const detector_region_result* worstLevel = nullptr;
					auto worstLag = 0.;
					std::string problems;
					for (const auto& g : r.Regions) {
						if (g.flagged())
							++flagged;
						if (const auto p = g.problem(); !p.empty() && (g.flagged() || g.LagFlag || !g.Error.empty()))
							problems += (problems.empty() ? "" : " | ") + p;
						if (!g.Error.empty())
							continue;
						if (!worstShape || g.PeakShape > worstShape->PeakShape)
							worstShape = &g;
						if (!worstLevel || std::abs(g.PeakLevelDb) > std::abs(worstLevel->PeakLevelDb))
							worstLevel = &g;
						worstLag = (std::max)(worstLag, g.RelLagMs);
					}
					std::ranges::replace(problems, ',', ' ');
					table += std::format(",{},{},{},{},{},{},{}", r.Regions.size(), flagged,
						worstShape ? std::format("{:.3f}", worstShape->PeakShape) : "",
						worstShape ? std::format("{:.2f}", worstShape->PeakShapeAt) : "",
						worstLevel ? std::format("{:.2f}", worstLevel->PeakLevelDb) : "",
						worstShape ? std::format("{:.2f}", worstLag) : "",
						problems);
				} else if (!r.RegionsNotAnalysable.empty()) {
					auto why = "not analysable: " + r.RegionsNotAnalysable;
					std::ranges::replace(why, ',', ' ');
					table += ",,,,,,," + why;
				} else {
					table += ",,,,,,,";
				}
				table += ',';
				table += r.Error;
				table += '\n';
			}
		}

		if (const auto outputPath = parser.present<std::string>("--output")) {
			const auto out = argactions::path(*outputPath);
			std::filesystem::create_directories(out.parent_path());
			std::ofstream f(out);
			f << table;
			std::cerr << std::format("Wrote {}.", u8(out)) << '\n';
		} else {
			std::cout << table;
		}

		// The summary, on stderr so it stays out of a redirected table.
		auto sorted = ok;
		std::ranges::sort(sorted, [](const row* a, const row* b) { return a->Weighted < b->Weighted; });
		std::cerr << '\n' << std::format("{} scored, {} could not be read.", ok.size(), failed.size()) << '\n';
		if (!sorted.empty()) {
			const auto median = sorted[sorted.size() / 2]->Weighted;
			size_t below = 0;
			for (const auto* r : sorted)
				if (r->Weighted < minScore)
					below++;
			std::cerr << std::format("median {:.4f}, {} below {:.2f}", median, below, minScore) << '\n';
		}

		size_t silentFiles = 0;
		double silentSeconds = 0;
		for (const auto* r : ok) {
			if (!r->Silence.empty()) {
				silentFiles++;
				silentSeconds += total_silence(r->Silence);
			}
		}
		if (silentFiles)
			std::cerr << std::format("{} file(s) hold {:.1f}s of silence the game's own files do not",
				silentFiles, silentSeconds) << '\n';

		std::vector<const row*> clicky;
		for (const auto* r : ok)
			if (r->Seam.Valid && r->Seam.Ratio > seamThreshold)
				clicky.push_back(r);
		if (!clicky.empty())
			std::cerr << std::format("{} loop seam(s) above {:.2f}", clicky.size(), seamThreshold) << '\n';

		if (withSpectrum) {
			// The top end is the band the 16 kHz score has never been able to see at all, so
			// it gets its own line rather than only a column.
			size_t duller = 0, brighter = 0;
			const row* worstHf = nullptr;
			for (const auto* r : ok) {
				if (!r->Spectrum.Valid)
					continue;
				if (r->Spectrum.HfDb <= -6.)
					duller++;
				else if (r->Spectrum.HfDb >= 6.)
					brighter++;
				if (!worstHf || r->Spectrum.HfDb < worstHf->Spectrum.HfDb)
					worstHf = r;
			}
			std::cerr << std::format("above 5 kHz: {} file(s) at least 6 dB duller, {} at least 6 dB brighter",
				duller, brighter) << '\n';
			if (worstHf)
				std::cerr << std::format("   dullest {} at {:.1f} dB", worstHf->Target, worstHf->Spectrum.HfDb) << '\n';
		}

		// Where the loop ends worse than the middle of the same loop. The comparison is the
		// point of the control window: a poor tail on its own usually means the recording is
		// a poor match throughout, which is not something the build did to it.
		{
			std::vector<const row*> tailWorse;
			for (const auto& r : rows) {
				if (!r.Tail.Valid || !r.Control.Valid)
					continue;
				if (r.Tail.Match < r.Control.Match - 0.05 || r.Tail.Worst < r.Control.Worst - 4.)
					tailWorse.push_back(&r);
			}
			if (!tailWorse.empty()) {
				std::sort(tailWorse.begin(), tailWorse.end(), [](const row* x, const row* y) {
					return (x->Tail.Match - x->Control.Match) < (y->Tail.Match - y->Control.Match);
				});
				std::cerr << std::format("{} file(s) end their loop worse than they run mid-loop; worst:",
					tailWorse.size()) << '\n';
				for (size_t i = 0; i < (std::min<size_t>)(tailWorse.size(), 8); ++i) {
					const auto* r = tailWorse[i];
					std::cerr << std::format("   {:<46} tail {:.3f} at {:+.1f} dB, mid-loop {:.3f} at {:+.1f} dB",
						r->Target.size() > 46 ? r->Target.substr(r->Target.size() - 46) : r->Target,
						r->Tail.Match, r->Tail.Worst, r->Control.Match, r->Control.Worst) << '\n';
				}
			}
		}

		// And where the head does not line up. Only counted where the probe found something
		// to align to -- a confident match at a non-zero lag is a real offset error, while a
		// low match at any lag is a recording that does not correspond there at all.
		{
			std::vector<const row*> offSync;
			// 0.90, calibrated on the library rather than guessed: of 1915 probes 1610 reach
			// it, and among those the lag is 0.00 ms at the median and 0.38 ms at the 90th
			// percentile -- so a confident probe that still reads a millisecond out is
			// saying something. Below about 0.70 the lag is not reliable at all, and a
			// looser bar of 0.50 called out 96 entries whose real problem is that the
			// recording does not correspond at the head, which the weighted score already
			// reports.
			for (const auto& r : rows)
				if (r.Head.Valid && r.Head.Match >= 0.90 && std::abs(r.Head.LagMs) >= 1.0)
					offSync.push_back(&r);
			if (!offSync.empty()) {
				std::sort(offSync.begin(), offSync.end(), [](const row* x, const row* y) {
					return std::abs(x->Head.LagMs) > std::abs(y->Head.LagMs);
				});
				std::cerr << std::format("{} file(s) do not start in sync; worst:", offSync.size()) << '\n';
				for (size_t i = 0; i < (std::min<size_t>)(offSync.size(), 8); ++i) {
					const auto* r = offSync[i];
					std::cerr << std::format("   {:<46} head {:+.2f} ms at {:.3f}{}",
						r->Target.size() > 46 ? r->Target.substr(r->Target.size() - 46) : r->Target,
						r->Head.LagMs, r->Head.Match,
						r->AtLoop.Valid
							? std::format(", loop start {:+.2f} ms", r->AtLoop.LagMs)
							: "") << '\n';
				}
			}

			// Out of sync somewhere along the file, or drifting. The same 0.90 bar and 1 ms
			// as the head; 5 ppm is where a drift reaches a millisecond within 200 s, and
			// the recordings the join campaign had to resample ran 7-13 ppm.
			std::vector<const row*> drifting;
			for (const auto& r : rows)
				if (r.Lags.Valid && ((std::max)(std::abs(r.Lags.MinMs), std::abs(r.Lags.MaxMs)) >= 1.0
					|| std::abs(r.Lags.Ppm) >= 5.))
					drifting.push_back(&r);
			if (!drifting.empty()) {
				std::ranges::sort(drifting, [](const row* x, const row* y) {
					return (std::max)(std::abs(x->Lags.MinMs), std::abs(x->Lags.MaxMs))
						> (std::max)(std::abs(y->Lags.MinMs), std::abs(y->Lags.MaxMs));
				});
				std::cerr << std::format("{} file(s) lose sync along the way; worst:", drifting.size()) << '\n';
				for (size_t i = 0; i < (std::min<size_t>)(drifting.size(), 8); ++i) {
					const auto* r = drifting[i];
					std::cerr << std::format("   {:<46} lag {:+.2f} to {:+.2f} ms over {} probes, {:+.1f} ppm",
						r->Target.size() > 46 ? r->Target.substr(r->Target.size() - 46) : r->Target,
						r->Lags.MinMs, r->Lags.MaxMs, r->Lags.Probes, r->Lags.Ppm) << '\n';
				}
			}

			// A whole-file level error. 3 dB rather than the 0.5-1 dB a listener can pick
			// out side by side: entries whose game master peaks above the recording's
			// headroom sit 1-2.5 dB under by design (apply does not clip), and are not news.
			std::vector<const row*> offLevel;
			for (const auto& r : rows)
				if (r.Level.Valid && std::abs(r.Level.MedianDb) >= 3.)
					offLevel.push_back(&r);
			if (!offLevel.empty()) {
				std::ranges::sort(offLevel, [](const row* x, const row* y) {
					return std::abs(x->Level.MedianDb) > std::abs(y->Level.MedianDb);
				});
				std::cerr << std::format("{} file(s) sit 3 dB or more off the game's level; worst:", offLevel.size()) << '\n';
				for (size_t i = 0; i < (std::min<size_t>)(offLevel.size(), 8); ++i) {
					const auto* r = offLevel[i];
					std::cerr << std::format("   {:<46} {:+.2f} dB (90% within {:.2f} dB)",
						r->Target.size() > 46 ? r->Target.substr(r->Target.size() - 46) : r->Target,
						r->Level.MedianDb, r->Level.P90AbsDb) << '\n';
				}
			}
		}

		// The joins that look broken: matching worse than the same recording does just before
		// them, stepping in level where the game's own file does not, or clicking at a cut.
		{
			struct flagged { const row* Row; const join_comparison* Join; };
			std::vector<flagged> bad;
			size_t checked = 0;
			for (const auto& r : rows)
				for (const auto& j : r.Joins) {
					++checked;
					if (join_is_suspect(j, seamThreshold))
						bad.push_back({&r, &j});
				}
			if (checked) {
				std::cerr << std::format("{} join(s) checked; {} look wrong", checked, bad.size())
					<< (bad.empty() ? "" : "; worst:") << '\n';
				// Worst side first: the side of a join the build gets furthest from the game.
				const auto severity = [](const join_comparison* j) {
					auto v = 0.;
					if (j->HasBefore) v = (std::max)(v, std::abs(j->BeforeDb));
					if (j->HasAfter) v = (std::max)(v, std::abs(j->AfterDb));
					if (j->Window.Valid && j->Control.Valid)
						v = (std::max)(v, 100. * (j->Control.Match - j->Window.Match));
					return v;
				};
				std::sort(bad.begin(), bad.end(), [&](const flagged& x, const flagged& y) {
					return severity(x.Join) > severity(y.Join);
				});
				for (size_t i = 0; i < (std::min<size_t>)(bad.size(), 12); ++i) {
					const auto& [r, j] = bad[i];
					std::cerr << std::format("   {:<40} at {:7.2f}s {:>8}  {}",
						r->Target.size() > 40 ? r->Target.substr(r->Target.size() - 40) : r->Target,
						j->Join.AtSeconds, j->Join.SameRecording ? "loop-out" : "medley",
						join_problem(*j, seamThreshold)) << '\n';
				}
			}
		}

		// The join detector's regions: which arrangements differ from the game's, and which
		// targets it could not model at all, by reason.
		{
			struct flagged { const row* Row; const detector_region_result* Region; };
			std::vector<flagged> bad;
			size_t regions = 0, errors = 0, lagged = 0;
			std::set<const row*> badFiles;
			std::map<std::string, size_t> notAnalysable;
			for (const auto& r : rows) {
				if (!r.RegionsNotAnalysable.empty()) {
					// The reason without the source name, so like counts with like.
					auto why = r.RegionsNotAnalysable;
					if (const auto q = why.find("\" "); why.starts_with("source \"") && q != std::string::npos)
						why = why.substr(q + 2);
					else if (const auto c = why.find("\": "); why.starts_with("source \"") && c != std::string::npos)
						why = why.substr(c + 3);
					++notAnalysable[why];
				}
				for (const auto& g : r.Regions) {
					++regions;
					if (!g.Error.empty())
						++errors;
					if (g.LagFlag)
						++lagged;
					if (g.flagged() || g.LagFlag) {
						bad.push_back({&r, &g});
						if (g.flagged())
							badFiles.insert(&r);
					}
				}
			}
			if (regions || !notAnalysable.empty()) {
				size_t joinFlagged = 0;
				for (const auto& b : bad)
					joinFlagged += b.Region->flagged();
				std::cerr << std::format("{} join region(s) modelled in {} target(s) in {:.0f}s; {} arrangement(s) differ from the game's in {} file(s), {} region(s) hold copies out of step{}",
					regions, detectorTargetsRun, detectorSeconds, joinFlagged, badFiles.size(), lagged,
					errors ? std::format(", {} could not be analysed", errors) : "")
					<< (bad.empty() ? "" : "; worst:") << '\n';
				std::ranges::stable_sort(bad, [](const flagged& x, const flagged& y) {
					return region_severity(*x.Region) > region_severity(*y.Region);
				});
				for (size_t i = 0; i < (std::min<size_t>)(bad.size(), 15); ++i) {
					const auto& [r, g] = bad[i];
					std::cerr << std::format("   {:<40} {:7.1f}-{:<7.1f} shape {:.2f} level {:+5.1f} dB  {}",
						r->Target.size() > 40 ? r->Target.substr(r->Target.size() - 40) : r->Target,
						g->From, g->To, g->PeakShape, g->PeakLevelDb, g->problem()) << '\n';
				}
				if (!notAnalysable.empty()) {
					size_t total = 0;
					for (const auto& [_w, n] : notAnalysable)
						total += n;
					std::cerr << std::format("{} multi-copy target(s) the detector cannot model:", total) << '\n';
					for (const auto& [why, n] : notAnalysable)
						std::cerr << std::format("   {:4} {}", n, why) << '\n';
				}
			}
		}

		if (worstCount && !sorted.empty()) {
			std::cerr << '\n' << std::format("worst {}:", (std::min<size_t>)(worstCount, sorted.size())) << '\n';
			std::cerr << std::format("   {:<46} {:>8} {:>7} {:>7} {:>7}", "target", "weighted", "r_eye", "hole", "silence") << '\n';
			for (size_t i = 0; i < (std::min<size_t>)(worstCount, sorted.size()); ++i) {
				const auto* r = sorted[i];
				std::cerr << std::format("   {:<46} {:>8.4f} {:>7} {:>7} {:>7.2f}",
					r->Target.size() > 46 ? r->Target.substr(r->Target.size() - 46) : r->Target,
					r->Weighted,
					r->Envelope.Valid ? std::format("{:.4f}", r->Envelope.REye) : "--",
					r->Envelope.Valid ? std::format("{:.1f}", r->Envelope.Hole) : "--",
					total_silence(r->Silence)) << '\n';
			}
		}
		if (!failed.empty()) {
			std::cerr << '\n' << std::format("{} could not be read; first few:", failed.size()) << '\n';
			for (size_t i = 0; i < (std::min<size_t>)(6, failed.size()); ++i)
				std::cerr << std::format("   {:<46} {}", failed[i]->Target, failed[i]->Error) << '\n';
		}

		return 0;
	} catch (const std::exception& e) {
		std::cerr << "Error verifying.\n" << e.what() << '\n';
		return -1;
	}
}
