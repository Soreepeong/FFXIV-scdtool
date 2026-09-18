#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Runs the given executable with the given arguments, discarding stderr and stdin,
// and returns whatever it wrote to stdout. Throws std::runtime_error if the process
// could not be started or exited with a nonzero code.
std::vector<uint8_t> run_process_capture_stdout(const std::filesystem::path& exe, const std::vector<std::wstring>& args);
