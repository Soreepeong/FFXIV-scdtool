#include "pch.h"
#include "argactions.h"

namespace {
	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}
}

std::filesystem::path argactions::path(const std::string& u8path) {
	return xivres::util::unicode::convert<std::wstring>(u8path);
}

std::filesystem::path argactions::absolute_path(const std::string& u8path) {
	auto path = argactions::path(u8path);
	if (!path.is_absolute())
		path = std::filesystem::absolute(path);
	return path;
}

std::filesystem::path argactions::absolute_path_or_stdout(const std::string& u8path) {
	if (u8path.empty() || u8path == "-")
		return {};

	const auto path = absolute_path(u8path);
	if (!std::filesystem::is_regular_file(path))
		throw std::runtime_error(std::format("Not an existing file: {}", u8(path)));
	return path;
}

std::filesystem::path argactions::existing_file(const std::string& u8path) {
	const auto path = absolute_path(u8path);
	if (!std::filesystem::is_regular_file(path))
		throw std::runtime_error(std::format("Not an existing file: {}", u8(path)));
	return path;
}

std::filesystem::path argactions::existing_directory(const std::string& u8path) {
	const auto path = absolute_path(u8path);
	if (!std::filesystem::is_directory(path))
		throw std::runtime_error(std::format("Not an existing directory: {}", u8(path)));
	return path;
}

std::filesystem::path argactions::installation_root(const std::string& spec) {
	if (spec == ":global") {
		auto root = xivres::installation::find_installation_global();
		if (root.empty())
			throw std::runtime_error("Could not autodetect global client installation path.");
		return root;
	}
	if (spec == ":china") {
		auto root = xivres::installation::find_installation_china();
		if (root.empty())
			throw std::runtime_error("Could not autodetect Chinese client installation path.");
		return root;
	}
	if (spec == ":korea") {
		auto root = xivres::installation::find_installation_korea();
		if (root.empty())
			throw std::runtime_error("Could not autodetect Korean client installation path.");
		return root;
	}
	return path(spec);
}
