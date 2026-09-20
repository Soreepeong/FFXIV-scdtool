#include "pch.h"
#include "extract.h"

#include "utils/argactions.h"
#include "utils/substitute_codec.h"

#include <nlohmann/json.hpp>

namespace {
	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}
}

int cmd_extract(const std::vector<std::string>& args) {
	argparse::ArgumentParser parser("scdtool extract");
	try {
		parser
			.add_description("Extract one sound entry of a game .scd to a standalone Ogg, FLAC or WAV file.")
			.add_epilog(
				"Useful for comparing a replacement made by `scdtool apply` against the file it\n"
				"replaces. The format follows the entry: the game's own files come out as Ogg or\n"
				"WAV, and one written by `apply --audio-format flac` or `wav` comes out as what its\n"
				"payload holds -- with the extension picked for you when --output names none.\n"
				"With --loop-info, prints the entry's loop points and length as JSON instead,\n"
				"without writing any audio.");
		parser.add_argument("--game").help(R"(game installation path, or :global/:china/:korea to autodetect; omit to read --input as a standalone .scd file on disk)");
		parser.add_argument("--input").required().help("the .scd to read: a path inside the game when --game is given, otherwise a file path");
		parser.add_argument("--output").help("file to write (.ogg or .wav, by the entry's format)");
		parser.add_argument("--entry-index").default_value(0u).scan<'u', uint32_t>().help("sound entry index to extract (default: 0)");
		parser.add_argument("--loop-info").default_value(false).implicit_value(true).help("print loop points and length as JSON instead of writing audio");
		parser.add_argument("--raw").default_value(false).implicit_value(true).help("copy the whole .scd container byte-for-byte instead of unwrapping one sound entry -- for tools that read the .scd format directly");
		parser.parse_args(args);
	} catch (const std::exception& e) {
		std::cerr
			<< "Error parsing arguments. Use `extract -h` to show help.\n"
			<< e.what() << '\n';
		return -1;
	}

	try {
		const auto gameSpec = parser.present<std::string>("--game").value_or(std::string());
		const auto inputPath = parser.get<std::string>("--input");
		const auto entryIndex = parser.get<uint32_t>("--entry-index");
		const auto loopInfo = parser.get<bool>("--loop-info");

		// With --game the input names a file inside the installation; without it the input
		// is a .scd sitting on disk, which is how a generated replacement is inspected.
		std::shared_ptr<xivres::stream> stream;
		if (gameSpec.empty()) {
			auto path = argactions::path(inputPath);
			if (!std::filesystem::exists(path))
				throw std::runtime_error(std::format("File not found: {}", u8(path)));
			stream = std::make_shared<xivres::file_stream>(path);
		} else {
			const xivres::installation installation(argactions::installation_root(gameSpec));
			stream = installation.get_file(inputPath);
		}

		if (parser.get<bool>("--raw")) {
			const auto outputPath = parser.present<std::string>("--output");
			if (!outputPath)
				throw std::runtime_error("--output is required with --raw.");
			auto out = argactions::path(*outputPath);
			if (out.extension().empty())
				out.replace_extension(L".scd");
			std::filesystem::create_directories(out.parent_path());
			std::vector<uint8_t> bytes(static_cast<size_t>(stream->size()));
			[[maybe_unused]] const auto read = stream->read(0, bytes.data(), static_cast<std::streamsize>(bytes.size()));
			std::ofstream f(out, std::ios::binary);
			if (!f)
				throw std::runtime_error(std::format("Could not create {}", u8(out)));
			f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			std::cerr << std::format("Wrote {} ({} bytes, raw container).", u8(out), bytes.size()) << '\n';
			return 0;
		}

		const xivres::sound::reader scd(stream);
		if (scd.sound_item_count() <= entryIndex)
			throw std::runtime_error(std::format("{} has only {} sound entries, cannot read index {}.",
				inputPath, scd.sound_item_count(), entryIndex));

		const auto item = scd.read_sound_item(entryIndex);

		if (loopInfo) {
			nlohmann::json res{
				{"input", inputPath},
				{"entryIndex", entryIndex},
				{"format", static_cast<uint32_t>(*item.Header->Format)},
				{"channels", static_cast<uint32_t>(item.Header->ChannelCount)},
				{"samplingRate", static_cast<uint32_t>(item.Header->SamplingRate)},
				{"streamSize", static_cast<uint32_t>(item.Header->StreamSize)},
				{"headerLoopStartOffset", static_cast<uint32_t>(item.Header->LoopStartOffset)},
				{"headerLoopEndOffset", static_cast<uint32_t>(item.Header->LoopEndOffset)},
				{"flags", static_cast<uint32_t>(*item.Header->Flags)},
			};
			// What kind of stream the file says this is. A 4- or 6-channel music entry is
			// DynamixStream -- engine-switched stems -- rather than a surround mix, and that
			// is a statement the file makes rather than something to infer from the channel
			// count. Absent for files whose sound table does not reach this entry.
			if (const auto descriptor = scd.read_sound_descriptor(entryIndex))
				res["soundType"] = static_cast<uint32_t>(descriptor->Type);
			// A format-6 entry whose payload is not Vorbis -- written by `apply --audio-format
			// flac` or `wav`, for a decoder-substitution hook -- has nothing to hand an Ogg
			// decoder, so it answers out of its own header instead.
			if (const auto payload = substitute_codec::inspect(item);
				payload.Kind != substitute_codec::payload::Vorbis) {
				res["payload"] = payload.Kind == substitute_codec::payload::Wave ? "wav" : "flac";
				res["totalSamples"] = payload.TotalFrames;
				res["durationSeconds"] = static_cast<double>(payload.TotalFrames)
					/ static_cast<double>(payload.SamplingRate ? payload.SamplingRate : 1);
				// The entry's loop fields are byte offsets into the payload, which is a sample
				// index again only where the payload is linear.
				if (payload.Kind == substitute_codec::payload::Wave && payload.Channels) {
					const auto frameBytes = payload.Channels * sizeof(int16_t);
					res["loopStartSample"] = static_cast<size_t>(item.Header->LoopStartOffset) / frameBytes;
					res["loopEndSample"] = static_cast<size_t>(item.Header->LoopEndOffset) / frameBytes;
				}
			} else if (item.Header->Format == xivres::sound::sound_entry_format::Ogg) {
				const auto info = item.get_ogg_decoded();
				const auto channels = info.Channels ? info.Channels : 1;
				res["loopStartSample"] = info.LoopStartBlockIndex;
				res["loopEndSample"] = info.LoopEndBlockIndex;
				res["totalSamples"] = info.Data.size() / sizeof(float) / channels;
				res["durationSeconds"] = static_cast<double>(res["totalSamples"].get<size_t>()) / static_cast<double>(info.SamplingRate ? info.SamplingRate : 1);
			}
			std::cout << res.dump(2) << '\n';
			return 0;
		}

		const auto outputPath = parser.present<std::string>("--output");
		if (!outputPath)
			throw std::runtime_error("--output is required unless --loop-info is given.");

		std::vector<uint8_t> bytes;
		const wchar_t* ext;
		if (const auto payload = substitute_codec::payload_of(item);
			payload != substitute_codec::payload::Vorbis) {
			// Header region then data, which for these is the whole file -- the same
			// relationship the Ogg path has between its header pages and its data pages, and
			// the reason nothing here has to know what a FLAC or RIFF stream is made of.
			bytes = substitute_codec::payload_file(item);
			ext = substitute_codec::payload_extension(payload);
		} else if (item.Header->Format == xivres::sound::sound_entry_format::Ogg) {
			bytes = item.get_ogg_file();
			ext = L".ogg";
		} else if (item.Header->Format == xivres::sound::sound_entry_format::WaveFormatPcm) {
			bytes = item.get_wav_file();
			ext = L".wav";
		} else {
			throw std::runtime_error("Entry is neither Ogg nor PCM wave.");
		}

		auto out = argactions::path(*outputPath);
		if (out.extension().empty())
			out.replace_extension(ext);
		std::filesystem::create_directories(out.parent_path());
		std::ofstream f(out, std::ios::binary);
		if (!f)
			throw std::runtime_error(std::format("Could not create {}", u8(out)));
		f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::cerr << std::format("Wrote {} ({} bytes).", u8(out), bytes.size()) << '\n';
		return 0;

	} catch (const std::exception& e) {
		std::cerr
			<< "Error processing data.\n"
			<< e.what() << '\n';
		return -1;
	}
}
