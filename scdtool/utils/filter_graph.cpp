#include "pch.h"
#include "filter_graph.h"

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <functional>
#include <set>
#include <stdexcept>

namespace {
	// Everything a filtergraph description reads as structure rather than as text. A value
	// holding any of these has to be quoted or the graph parses as something else -- a
	// `volume` expression with a comma in it becomes a second filter, which is exactly the
	// class of mistake this whole form exists to remove.
	constexpr auto NeedsQuoting = ":,;[]'\\ \t";

	std::string number_to_text(const nlohmann::json& value) {
		// nlohmann prints a double that happens to be integral as "254.0"; ffmpeg accepts
		// that, but a preset round-tripping through this should not gain a decimal point it
		// did not have. Integers stay integers.
		if (value.is_number_integer())
			return std::format("{}", value.get<int64_t>());
		if (value.is_number_unsigned())
			return std::format("{}", value.get<uint64_t>());
		auto text = std::format("{}", value.get<double>());
		return text;
	}

	std::string value_to_text(const nlohmann::json& value) {
		if (value.is_string())
			return value.get<std::string>();
		if (value.is_boolean())
			return value.get<bool>() ? "1" : "0";
		if (value.is_number())
			return number_to_text(value);
		throw std::runtime_error(std::format(
			"a filter argument has to be a string, a number or a boolean; got {}", value.type_name()));
	}

	// One filter: "name", "name=raw", {"name": null}, {"name": "raw"} or {"name": {k: v}}.
	std::string emit_filter(const nlohmann::json& filterJson) {
		if (filterJson.is_string()) {
			// Passed through untouched. An author who has a working chain should not have to
			// translate it to use the rest of this form.
			return filterJson.get<std::string>();
		}
		if (!filterJson.is_object() || filterJson.size() != 1)
			throw std::runtime_error(
				"a filter is either a string, or an object with exactly one key naming the filter");

		const auto& name = filterJson.begin().key();
		const auto& args = filterJson.begin().value();
		if (name.empty())
			throw std::runtime_error("a filter's name cannot be empty");
		if (args.is_null())
			return name;
		if (args.is_string() || args.is_number() || args.is_boolean())
			return std::format("{}={}", name, escape_filter_value(value_to_text(args)));
		if (!args.is_object())
			throw std::runtime_error(std::format(
				"filter \"{}\" takes its arguments as an object, a bare value, or null", name));

		std::string out = name;
		auto first = true;
		for (const auto& [key, value] : args.items()) {
			out += first ? '=' : ':';
			first = false;
			out += key;
			out += '=';
			out += escape_filter_value(value_to_text(value));
		}
		return first ? name : out;
	}

	std::string strip_brackets(std::string label) {
		if (label.size() >= 2 && label.front() == '[' && label.back() == ']')
			label = label.substr(1, label.size() - 2);
		return label;
	}
}

std::string escape_filter_value(const std::string& value) {
	if (value.find_first_of(NeedsQuoting) == std::string::npos)
		return value;
	// Single quotes make everything inside literal, and the only thing that can end them is
	// another single quote -- so one in the value is written by closing, escaping it outside
	// the quotes, and reopening. A backslash is escaped for the level below.
	std::string out = "'";
	for (const auto ch : value) {
		if (ch == '\'')
			out += "'\\''";
		else if (ch == '\\')
			out += "\\\\";
		else
			out += ch;
	}
	out += '\'';
	return out;
}

filter_graph compile_filter_graph(const nlohmann::json& graphJson, const std::string& outNameHint) {
	if (!graphJson.is_object() || graphJson.empty())
		throw std::runtime_error("a filterGraph is an object of output-label -> {in, filters}");

	filter_graph result;
	std::set<size_t> usedInputs;
	std::set<std::string> consumed;   // labels some chain reads
	std::map<std::string, std::string> chains;
	std::map<std::string, std::vector<std::string>> reads;   // label -> the labels it consumes

	for (const auto& [rawLabel, node] : graphJson.items()) {
		const auto label = strip_brackets(rawLabel);
		if (label.empty())
			throw std::runtime_error("a filterGraph output label cannot be empty");

		const nlohmann::json* inputs = nullptr;
		const nlohmann::json* filters = nullptr;
		if (node.is_object()) {
			if (const auto it = node.find("in"); it != node.end())
				inputs = &*it;
			if (const auto it = node.find("filters"); it != node.end())
				filters = &*it;
		} else {
			throw std::runtime_error(std::format(
				"filterGraph entry \"{}\" is an object with \"in\" and \"filters\"", label));
		}
		if (!inputs || !inputs->is_array() || inputs->empty())
			throw std::runtime_error(std::format(
				"filterGraph entry \"{}\" needs a non-empty \"in\" array", label));

		std::string chain;
		for (const auto& one : *inputs) {
			if (one.is_number_unsigned() || one.is_number_integer()) {
				const auto index = one.get<int64_t>();
				if (index < 0)
					throw std::runtime_error(std::format(
						"filterGraph entry \"{}\" names input {}, which is not a slot", label, index));
				usedInputs.insert(static_cast<size_t>(index));
				chain += std::format("[{}:a]", index);
			} else if (one.is_string()) {
				const auto from = strip_brackets(one.get<std::string>());
				consumed.insert(from);
				reads[label].push_back(from);
				chain += std::format("[{}]", from);
			} else {
				throw std::runtime_error(std::format(
					"filterGraph entry \"{}\" takes an input-file index or another label", label));
			}
		}

		if (filters && filters->is_array() && !filters->empty()) {
			auto first = true;
			for (const auto& one : *filters) {
				if (!first)
					chain += ',';
				first = false;
				chain += emit_filter(one);
			}
		} else {
			// No filters is legitimate: it renames, which is how a single input becomes the
			// graph's output without doing anything to it. anull is the identity filter.
			chain += "anull";
		}
		chain += std::format("[{}]", label);
		reads.try_emplace(label);
		chains.emplace(label, std::move(chain));
	}

	// Every label an input names has to be produced by some chain, or ffmpeg fails with a
	// message about the graph rather than about the preset.
	for (const auto& from : consumed) {
		if (!chains.contains(from))
			throw std::runtime_error(std::format(
				"filterGraph reads \"{}\", which no entry produces", from));
	}

	if (!outNameHint.empty()) {
		result.OutLabel = strip_brackets(outNameHint);
		if (!chains.contains(result.OutLabel))
			throw std::runtime_error(std::format(
				"filterGraph names \"{}\" as its output, which no entry produces", result.OutLabel));
	} else {
		// The sink: the one label nothing else reads. More than one means the graph builds
		// two things and has not said which is the source, which is an authoring mistake
		// rather than something to guess at.
		std::vector<std::string> sinks;
		for (const auto& label : chains | std::views::keys)
			if (!consumed.contains(label))
				sinks.push_back(label);
		if (sinks.size() != 1)
			throw std::runtime_error(std::format(
				"filterGraph has {} possible outputs ({}); name one with filterComplexOutName",
				sinks.size(), sinks.empty() ? std::string("none -- every label is read, so it is a cycle")
					: std::format("{}", sinks.size() > 4 ? "several" : "see the labels")));
		result.OutLabel = sinks.front();
	}

	// Emitted producers-first rather than in key order. ffmpeg links chains by label whatever
	// order they are written in, so this does not change which graph is built -- but it does
	// change the order ffmpeg constructs the filters in, and that is enough to move a sum:
	// against the hand-written BGM_EX4_Event_15 graph, emitting alphabetically (`res` before
	// the three chains it mixes) differed in 3 of 18069624 samples by one LSB. It also reads
	// the way a person would write it, which matters when the compiled string is what an
	// error message quotes.
	std::vector<std::string> order;
	std::set<std::string> placed, active;
	std::function<void(const std::string&)> visit = [&](const std::string& label) {
		if (placed.contains(label))
			return;
		if (!active.insert(label).second)
			throw std::runtime_error(std::format("filterGraph has a cycle through \"{}\"", label));
		for (const auto& from : reads.at(label))
			visit(from);
		active.erase(label);
		placed.insert(label);
		order.push_back(label);
	};
	for (const auto& label : chains | std::views::keys)
		visit(label);

	auto first = true;
	for (const auto& label : order) {
		if (!first)
			result.Description += ';';
		first = false;
		result.Description += chains.at(label);
	}
	result.UsedInputs.assign(usedInputs.begin(), usedInputs.end());
	return result;
}
