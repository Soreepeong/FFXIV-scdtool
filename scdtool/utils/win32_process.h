#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Runs the given executable with the given arguments, discarding stderr and stdin,
// and returns whatever it wrote to stdout. Throws std::runtime_error if the process
// could not be started or exited with a nonzero code.
std::vector<uint8_t> run_process_capture_stdout(const std::filesystem::path& exe, const std::vector<std::wstring>& args);

// Same, but captures stderr instead of stdout. ffmpeg reports the output of analysis
// filters such as ebur128 on stderr, so measuring loudness needs this direction.
std::vector<uint8_t> run_process_capture_stderr(const std::filesystem::path& exe, const std::vector<std::wstring>& args);
