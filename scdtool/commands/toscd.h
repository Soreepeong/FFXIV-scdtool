#pragma once

#include <string>
#include <vector>

// `scdtool toscd`: builds a single-entry replacement .scd from a template .scd and an
// Ogg or WAV input, carrying over the template's tables and every other sound entry
// unchanged. This used to be the default (no-subcommand) behaviour of `scdtool`; the
// flags are unchanged, only the subcommand name is new.
// `args` is the subcommand's own argv, with args[0] acting as its program name.
int cmd_toscd(const std::vector<std::string>& args);
