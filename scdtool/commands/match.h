#pragma once

#include <string>
#include <vector>

// `scdtool match`: fills in "source" for preset items whose target .scd audio
// can be confidently matched, by audio content correlation, against a directory
// of candidate OST files. See match.cpp for the JSON schema this reads/writes.
// `args` is the subcommand's own argv, with args[0] acting as its program name
// (i.e. args should start with "match").
int cmd_match(const std::vector<std::string>& args);
