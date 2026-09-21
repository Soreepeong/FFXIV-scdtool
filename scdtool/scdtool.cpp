#include "pch.h"
#include "commands/apply.h"
#include "commands/extract.h"
#include "commands/match.h"
#include "commands/match_disc.h"
#include "commands/toscd.h"
#include "commands/verify.h"

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
	// extract.cpp, match.cpp, match_disc.cpp, and apply.cpp), dispatched here so their
	// arguments never collide with each other. Every subcommand gets its own argv beginning
	// at its own name, i.e. args[0] acting as its program name.
	const auto command = args.size() >= 2 ? std::string_view(args[1]) : std::string_view();
	if (command == "toscd")
		return cmd_toscd(std::vector(args.begin() + 1, args.end()));
	if (command == "extract")
		return cmd_extract(std::vector(args.begin() + 1, args.end()));
	if (command == "match")
		return cmd_match(std::vector(args.begin() + 1, args.end()));
	if (command == "match-disc")
		return cmd_match_disc(std::vector(args.begin() + 1, args.end()));
	if (command == "apply")
		return cmd_apply(std::vector(args.begin() + 1, args.end()));
	if (command == "verify")
		return cmd_verify(std::vector(args.begin() + 1, args.end()));

	// No subcommand, or one that does not exist: list the commands that do.
	if (command.empty())
		std::cerr << "No command specified.\n\n";
	else if (command != "-h" && command != "--help" && command != "help")
		std::cerr << "Unknown command: " << command << '\n' << '\n';

	std::cerr
		<< "Usage: scdtool <command> [options]\n"
		<< '\n'
		<< "Commands:\n"
		<< "  toscd        create a single-entry SCD file from a template .scd and an Ogg or WAV input\n"
		<< "  extract      extract one sound entry of a game .scd out as a standalone Ogg or WAV file\n"
		<< "  match        match OST audio tracks to game .scd files by content and fill in a preset\n"
		<< "  match-disc   match a Blu-ray disc's .m2ts clips to OST tracks and output the mapping (json or csv)\n"
		<< "  apply        write replacement .scd files for the matched sources of a preset\n"
		<< '\n'
		<< "Run `scdtool <command> -h` for the options of a command.\n";
	return -1;
}
