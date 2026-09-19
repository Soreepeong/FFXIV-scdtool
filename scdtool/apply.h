#pragma once

#include <string>
#include <vector>

// `scdtool apply`: turns the matched entries of a preset produced by `scdtool match`
// into actual replacement .scd files, using the game's file as the template so the
// surrounding tables and every other sound entry are carried over unchanged.
// `args` is the subcommand's own argv, with args[0] acting as its program name.
int cmd_apply(const std::vector<std::string>& args);
