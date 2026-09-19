#pragma once

// Each cmd_* function implements one subcommand's body, given its already-parsed
// argparse::ArgumentParser (see scdtool.cpp for the subcommand definitions and
// dispatch). Returns the process exit code.
int cmd_single(const argparse::ArgumentParser& args);
int cmd_match_disc(const argparse::ArgumentParser& args);
