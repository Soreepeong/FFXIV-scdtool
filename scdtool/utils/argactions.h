#pragma once

#include <filesystem>
#include <string>

// The argument actions the commands share, in the shape argparse wants them: each takes an
// argument's utf-8 text and returns the value the command body should work with, so a bad
// argument is reported while parsing it rather than part way through a run. They are plain
// functions, so an argument can be handed one directly:
//
//     parser.add_argument("--disc").required().action(argactions::existing_directory);
//
// See toscd.cpp for the call sites this pattern was pulled out of.
namespace argactions {
	// The path as given: widened, but neither absolutised nor checked. For arguments that may be
	// bare names something else resolves -- ffmpeg and ffprobe, which CreateProcessW looks up in
	// PATH -- or whose existence only matters later.
	std::filesystem::path path(const std::string& u8path);

	// The same, absolutised, so that everything downstream (and every message that names the
	// path) uses one form no matter how it was spelled on the command line.
	std::filesystem::path absolute_path(const std::string& u8path);
	std::filesystem::path absolute_path_or_stdout(const std::string& u8path);

	// ... and required to name a file / a directory that is already there.
	std::filesystem::path existing_file(const std::string& u8path);
	std::filesystem::path existing_directory(const std::string& u8path);

	// A game installation: ":global", ":china" or ":korea" autodetect that client's installation,
	// anything else is a path to the installation directory itself. Throws if autodetection comes
	// up empty.
	std::filesystem::path installation_root(const std::string& spec);
}
