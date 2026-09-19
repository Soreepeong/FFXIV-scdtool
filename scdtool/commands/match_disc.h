#pragma once

#include <string>
#include <vector>

// `scdtool match-disc`: matches the .m2ts clips of a Blu-ray/BDMV disc to a directory
// of OST audio files by audio content correlation, and outputs the resulting clip ->
// track mapping (plus the clips that could not be matched) as json or csv, to stdout by
// default. See match_disc.cpp for the exact schemas.
// `args` is the subcommand's own argv, with args[0] acting as its program name
// (i.e. args should start with "match-disc").
int cmd_match_disc(const std::vector<std::string>& args);
