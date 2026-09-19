#pragma once

#include <string>
#include <vector>

// `scdtool extract`: writes one sound entry of a game .scd out as a standalone Ogg or
// WAV file. Mainly so a generated replacement can be compared against the file it
// replaces, but useful on its own for listening to a shipped track.
// `args` is the subcommand's own argv, with args[0] acting as its program name.
int cmd_extract(const std::vector<std::string>& args);
