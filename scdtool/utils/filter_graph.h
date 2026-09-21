#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// An ffmpeg filter graph written as JSON rather than as one escaped string.
//
// The string form is what `presets-manual/` has always used, and it is unreadable for the
// same reason every nested escaping scheme is: a filtergraph description separates filters
// with `,`, their arguments with `:`, and chains with `;`, so any argument that *contains*
// one of those has to be quoted, and a quote inside that has to be escaped again:
//
//     [0:a]atrim=242.649:254.487,volume='max(0,min(4.238,t-16.433))/4.238':eval=frame[ra]
//
// Written as JSON the delimiters are structure instead of punctuation, and nothing needs
// escaping at the author's end -- `emit_filter_graph` puts the quoting back:
//
//     "filterGraph": {
//       "ra": {"in": [0], "filters": [
//         {"atrim": {"start": 242.649, "end": 254.487}},
//         {"volume": {"volume": "max(0,min(4.238,t-16.433))/4.238", "eval": "frame"}}
//       ]},
//       "res": {"in": ["ra", "rb"], "filters": [{"amix": {"inputs": 2, "duration": "longest"}}]}
//     }
//
// Keyed by the name of the chain's *output*, which is what makes an object the right shape:
// a label is produced exactly once, so the keys cannot collide, while the inputs cannot be
// keys -- 20 of the 23 hand-written graphs feed a filter from several labels at once and the
// widest feeds `amix` from seven, so an "input -> output" mapping could not hold them.
//
// An entry of `in` is either a number -- the index of one of the source's `inputFiles`, the
// `[2:a]` of the string form -- or a string naming another chain's output. Order is kept:
// `amix=duration=first` reads it.
struct filter_graph {
	// The `-filter_complex` argument, ready to pass to ffmpeg.
	std::string Description;
	// The label to `-map`, without brackets.
	std::string OutLabel;
	// Which `inputFiles` slots the graph actually reads, ascending. A slot nothing reads
	// does not have to be resolved to a file.
	std::vector<size_t> UsedInputs;
};

// Compiles the object form above. `outNameHint` is the preset's `filterComplexOutName` when
// it has one -- brackets optional; when it is empty the sink is found instead, which is the
// one label no other chain consumes. Throws std::runtime_error with a reason a preset author
// can act on: an unknown label, a cycle, or more than one possible sink.
filter_graph compile_filter_graph(const nlohmann::json& graphJson, const std::string& outNameHint = {});

// Quotes and escapes one filter argument value the way a filtergraph description needs.
// Exposed for tests; `compile_filter_graph` applies it to every value it emits.
std::string escape_filter_value(const std::string& value);
