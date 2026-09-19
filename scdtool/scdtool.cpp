#include "pch.h"
#include "apply.h"
#include "extract.h"
#include "match.h"
#include "toscd.h"

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif

	std::vector<std::string> args;
	args.reserve(argc);
	for (int i = 0; i < argc; i++)
		args.emplace_back(xivres::util::unicode::convert<std::string>(argv[i]));

	// Each subcommand is a separate file with its own argument parser (see toscd.cpp,
	// extract.cpp, match.cpp, and apply.cpp), dispatched here so their arguments never
	// collide with each other. Every subcommand gets its own argv beginning at its own
	// name, i.e. args[0] acting as its program name.
	const auto command = args.size() >= 2 ? std::string_view(args[1]) : std::string_view();
	if (command == "toscd")
		return cmd_toscd(std::vector(args.begin() + 1, args.end()));
	if (command == "extract")
		return cmd_extract(std::vector(args.begin() + 1, args.end()));
	if (command == "match")
		return cmd_match(std::vector(args.begin() + 1, args.end()));
	if (command == "apply")
		return cmd_apply(std::vector(args.begin() + 1, args.end()));

	// No subcommand, or one that does not exist: list the commands that do.
	if (command.empty())
		std::cerr << "No command specified." << std::endl << std::endl;
	else if (command != "-h" && command != "--help" && command != "help")
		std::cerr << "Unknown command: " << command << std::endl << std::endl;

	std::cerr
		<< "Usage: scdtool <command> [options]" << std::endl
		<< std::endl
		<< "Commands:" << std::endl
		<< "  toscd    create a single-entry SCD file from a template .scd and an Ogg or WAV input" << std::endl
		<< "  extract  extract one sound entry of a game .scd out as a standalone Ogg or WAV file" << std::endl
		<< "  match    match OST audio tracks to game .scd files by content and fill in a preset" << std::endl
		<< "  apply    write replacement .scd files for the matched sources of a preset" << std::endl
		<< std::endl
		<< "Run `scdtool <command> -h` for the options of a command." << std::endl;
	return -1;
}
