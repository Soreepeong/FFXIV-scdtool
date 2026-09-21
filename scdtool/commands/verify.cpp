#include "pch.h"
#include "verify.h"

#include "utils/argactions.h"
#include "utils/audio_match.h"
#include "utils/misc.h"
#include "utils/win32_process.h"
#include "utils/hca_payload.h"
#include "utils/substitute_codec.h"
#include "utils/verify_audio.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <ranges>

namespace {
	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}

	struct loop_info {
		size_t StartSample = 0;
		size_t EndSample = 0;
		size_t Rate = 0;
		size_t TotalSamples = 0;
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

		loop_info info{.Rate = static_cast<size_t>(item.Header->SamplingRate)};
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

	struct row {
		std::string Target;
		double Weighted = 0., Plain = 0.;
		double BuiltSeconds = 0., GameSeconds = 0.;
		envelope_comparison Envelope;
		std::vector<silence_run> Silence;
		seam_result Seam;
		spectrum_comparison Spectrum;
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
				"  seam      whether the loop point clicks, judged from the built file alone. Above\n"
				"            about 1 the seam is a bigger jump than anything happening near it.\n"
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

		const auto tempDir = std::filesystem::temp_directory_path() / L"scdtool_verify";
		std::filesystem::create_directories(tempDir);
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
				unwrap_entry(installation.get_file(target), entryIndex, stem.wstring() + L"_b", gameAudio);

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
				out.push_back(std::move(one));
			}
			table = out.dump(1);
		} else {
			table = "target,weighted,plain,built_seconds,game_seconds,r_eye,dev,span,hole,hole_at,silence_seconds,seam_ratio";
			if (withSpectrum)
				table += ",tilt_db_per_decade,worst_band_db,worst_band_hz,edge_built_hz,edge_game_hz,patch_db,patch_hz,hf_db";
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

		std::error_code ec;
		std::filesystem::remove_all(tempDir, ec);
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "Error verifying.\n" << e.what() << '\n';
		return -1;
	}
}
