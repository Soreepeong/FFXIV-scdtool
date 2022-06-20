#include "pch.h"

struct audio_time_point_t {
	enum class Mode {
		Samples,
		TimePoint,
		Empty,
	};

	Mode Mode = Mode::Empty;
	uint64_t Samples = 0;
	std::chrono::microseconds TimePoint = std::chrono::microseconds(0);

	static audio_time_point_t from_string(const std::string& str) {
		char* ep;
		if (const auto val = strtoull(&str[0], &ep, 0); ep == str.data() + str.size())
			return audio_time_point_t{Mode::Samples, val};
		if (const auto val = strtod(&str[0], &ep); ep == str.data() + str.size())
			return audio_time_point_t{Mode::TimePoint, 0, std::chrono::microseconds(static_cast<uint64_t>(val * 1000000.))};
		throw std::runtime_error("Invalid time point value");
	}
};

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif

	argparse::ArgumentParser parser;
	try {
		std::vector<std::string> args;
		args.reserve(argc);
		for (int i = 0; i < argc; i++)
			args.emplace_back(xivres::util::unicode::convert<std::string>(argv[i]));

		parser
			.add_description("Create a single-entry SCD file.")
			.add_epilog(std::format("\n"
				R"(Usage examples:)" "\n"
				R"(* {0} -t "C:\Program Files (x86)\SquareEnix\FINAL FANTASY XIV - A Realm Reborn\game::music/ex2/BGM_EX2_System_Title.scd")" "\n"
				R"(  -i replacement.ogg -c ogg -oq 1.0 --loop-begin 1234 --loop-end 5.00 -o result.scd)" "\n",
				xivres::util::unicode::convert<std::string>(std::filesystem::path(argv[0]).filename().u8string())));

		parser
			.add_argument("-t", "--template")
			.required()
			.help(R"(specify template scd file; if given as A::B format, then the file B will be searched from the game installation at A (specify "game" directory, or use ":global", ":china", or ":korea" or auto detect))")
			.action([](const std::string& u8path) -> std::shared_ptr<xivres::stream> {
				const auto components = xivres::util::split(u8path, std::string("::"), 1);
				if (components.empty())
					throw std::runtime_error("Path not specified.");

				if (components.size() == 1)
					return std::make_shared<xivres::file_stream>(xivres::util::unicode::convert<std::wstring>(u8path));

				std::filesystem::path path;
				if (components[0] == ":global") {
					path = xivres::installation::find_installation_global();
					if (path.empty())
						throw std::runtime_error("Could not autodetect global client installation path.");

				} else if (components[0] == ":china") {
					path = xivres::installation::find_installation_china();
					if (path.empty())
						throw std::runtime_error("Could not autodetect Chinese client installation path.");

				} else if (components[0] == ":korea") {
					path = xivres::installation::find_installation_korea();
					if (path.empty())
						throw std::runtime_error("Could not autodetect Korean client installation path.");

				} else
					path = xivres::util::unicode::convert<std::wstring>(components[0]);
				return xivres::installation(path).get_file(components[1]);
			});
		parser
			.add_argument("-i", "--input")
			.help("specify input ogg or wav file")
			.action([](const std::string& u8path) -> std::shared_ptr<xivres::stream> {
				std::filesystem::path path = xivres::util::unicode::convert<std::wstring>(u8path);
				if (!path.is_absolute())
					path = absolute(path);
				if (!exists(path))
					throw std::runtime_error(std::format("Path does not exist: {}", xivres::util::unicode::convert<std::string>(path.u8string())));
				return std::make_shared<xivres::file_stream>(path);
			});
		parser
			.add_argument("-o", "--output")
			.required()
			.help("specify output scd file path, including .scd extension")
			.action([](const std::string& u8path) -> std::filesystem::path {
				std::filesystem::path path = xivres::util::unicode::convert<std::wstring>(u8path);
				if (!path.is_absolute())
					path = absolute(path);
				return path;
			});
		parser
			.add_argument("-c", "--codec")
			.default_value(xivres::sound::sound_entry_format::Empty)
			.required()
			.help("specify codec (valid: copy(default), pcm, and ogg)")
			.action([](const std::string& str) -> xivres::sound::sound_entry_format {
				const auto strl = xivres::util::unicode::convert<std::string>(str, &xivres::util::unicode::lower);
				if (strl == "copy")
					return xivres::sound::sound_entry_format::Empty;
				if (strl == "pcm")
					return xivres::sound::sound_entry_format::WaveFormatPcm;
				if (strl == "ogg")
					return xivres::sound::sound_entry_format::Ogg;
				throw std::runtime_error("Invalid codec");
			});
		parser
			.add_argument("-oq", "--ogg-quality")
			.default_value(1.f)
			.required()
			.help("specify ogg quality, if using ogg codec")
			.action([](const std::string& str) { return strtof(&str[0], nullptr); });
		parser
			.add_argument("--loop-begin")
			.default_value(audio_time_point_t())
			.required()
			.help("specify loop begin point (integer=samples, float=seconds)")
			.action([](const std::string& str) { return audio_time_point_t::from_string(str); });
		parser
			.add_argument("--loop-end")
			.default_value(audio_time_point_t())
			.required()
			.help("specify loop end point (integer=samples, float=seconds)")
			.action([](const std::string& str) { return audio_time_point_t::from_string(str); });

		parser.parse_args(args);

	} catch (const std::exception& e) {
		std::cerr
			<< "Error parsing arguments. Use -h to show help." << std::endl
			<< e.what() << std::endl;
		return -1;
	}

	const auto templateStream = parser.get<std::shared_ptr<xivres::stream>>("-t");
	const auto inputStream = parser.get<std::shared_ptr<xivres::stream>>("-i");
	const auto outputPath = parser.get<std::filesystem::path>("-o");
	const auto oggQuality = (std::max)(0.f, (std::min)(1.f, parser.get<float>("-oq")));
	auto codec = parser.get<xivres::sound::sound_entry_format>("-c");
	auto loopBegin = parser.get<audio_time_point_t>("--loop-begin");
	auto loopEnd = parser.get<audio_time_point_t>("--loop-end");

	try {
		const auto templateScd = xivres::sound::reader(templateStream);
		
		xivres::sound::reader::sound_item::audio_info sourceInfo;
		if (std::string_view(inputStream->read_vector<char>(0, 4)) == "RIFF") {
			auto soundItem = xivres::sound::writer::sound_item::make_from_wave(inputStream->as_linear_reader<uint8_t>());
			sourceInfo.Channels = soundItem.Header.ChannelCount;
			sourceInfo.SamplingRate = soundItem.Header.SamplingRate;
			sourceInfo.Data.resize(soundItem.Data.size() * sizeof(float) / sizeof(int16_t));
			const auto int16Span = xivres::util::span_cast<int16_t>(soundItem.Data);
			const auto floatSpan = xivres::util::span_cast<float>(sourceInfo.Data);
			for (size_t i = 0; i < int16Span.size(); i++)
				floatSpan[i] = static_cast<float>(int16Span[i]) / 32768.f;
			if (codec == xivres::sound::sound_entry_format::Empty)
				codec = xivres::sound::sound_entry_format::WaveFormatPcm;

		} else if (std::string_view(inputStream->read_vector<char>(0, 4)) == "OggS") {
			sourceInfo = xivres::sound::reader::sound_item::decode_ogg(inputStream->read_vector<uint8_t>());
			if (codec == xivres::sound::sound_entry_format::Empty)
				codec = xivres::sound::sound_entry_format::Ogg;

		} else {
			throw std::runtime_error("Input file is not a valid WAV or OGG file.");
		}

		xivres::sound::writer::sound_item newEntry;
		if (codec == xivres::sound::sound_entry_format::Ogg) {
			if (loopBegin.Mode == audio_time_point_t::Mode::TimePoint)
				loopBegin.Samples = static_cast<size_t>(std::chrono::duration_cast<std::chrono::duration<float>>(loopBegin.TimePoint).count() * static_cast<float>(sourceInfo.SamplingRate));
			if (loopEnd.Mode == audio_time_point_t::Mode::TimePoint)
				loopEnd.Samples = static_cast<size_t>(std::chrono::duration_cast<std::chrono::duration<float>>(loopEnd.TimePoint).count() * static_cast<float>(sourceInfo.SamplingRate));
			if (loopBegin.Mode != audio_time_point_t::Mode::Empty && loopEnd.Mode == audio_time_point_t::Mode::Empty)
				loopEnd.Samples = sourceInfo.Data.size() / sizeof(float) / sourceInfo.Channels;
			newEntry = xivres::sound::writer::sound_item::make_from_ogg_encode(
				sourceInfo.Channels,
				sourceInfo.SamplingRate,
				loopBegin.Samples,
				loopEnd.Samples,
				xivres::memory_stream(std::span(sourceInfo.Data)).as_linear_reader<uint8_t>(),
				[&](size_t blockIndex) {
					std::cerr << std::format("\rEncoding: block {} out of {}", blockIndex, sourceInfo.Data.size() / sizeof(float) / sourceInfo.Channels) << std::flush;
					return true;
				},
				{},
				oggQuality
			);
			std::cerr << std::endl;

		} else if (codec == xivres::sound::sound_entry_format::WaveFormatPcm) {
			newEntry.Data.resize(sourceInfo.Data.size() * sizeof(int16_t) / sizeof(float));
			const auto int16Span = xivres::util::span_cast<int16_t>(newEntry.Data);
			const auto floatSpan = xivres::util::span_cast<float>(sourceInfo.Data);
			for (size_t i = 0; i < int16Span.size(); i++)
				int16Span[i] = static_cast<int16_t>((std::max)(-32768, (std::min)(32767, static_cast<int>(floatSpan[i] * 32768.f))));
			newEntry.Header.StreamSize = static_cast<uint32_t>(newEntry.Data.size());
			newEntry.Header.ChannelCount = static_cast<uint32_t>(sourceInfo.Channels);
			newEntry.Header.SamplingRate = static_cast<uint32_t>(sourceInfo.SamplingRate);
		}

		auto newScd = xivres::sound::writer();
		newScd.set_table_1(templateScd.read_table_1());
		newScd.set_table_2(templateScd.read_table_2());
		newScd.set_table_4(templateScd.read_table_4());
		newScd.set_table_5(templateScd.read_table_5());
		newScd.set_sound_item(0, newEntry);

		const auto result = newScd.export_to_bytes();
		const auto hFile = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
			throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()));

		if (DWORD w{}; !WriteFile(hFile, &result[0], static_cast<DWORD>(result.size()), &w, nullptr) || w != result.size())
			throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()));

		CloseHandle(hFile);

		std::cerr << "Done!" << std::endl;
		
	} catch (const std::exception& e) {
		std::cerr
			<< "Error processing data." << std::endl
			<< e.what() << std::endl;
		return -1;
	}
	return 0;
}
