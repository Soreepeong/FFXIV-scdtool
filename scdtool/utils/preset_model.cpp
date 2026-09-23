#include "pch.h"
#include "preset_model.h"

#include "filter_graph.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>

namespace {
	std::string u8(const std::filesystem::path& p) {
		return xivres::util::unicode::convert<std::string>(p.wstring());
	}
}

std::vector<std::filesystem::path> resolve_search_directories(
	const std::filesystem::path& ostDir,
	const nlohmann::json& config,
	const std::string& album) {

	const auto search = config.find("searchDirectories");
	if (search == config.end() || !search->is_object())
		return {ostDir};

	std::string wanted = album;
	if (wanted.empty()) {
		for (const auto& [name, spec] : search->items()) {
			if (wanted.empty() || (spec.is_object() && spec.value("default", false)))
				wanted = name;
			if (spec.is_object() && spec.value("default", false))
				break;
		}
	} else if (!search->contains(album)) {
		// Naming a directory the preset never declared would reach a release the user was
		// never told this preset needs, so it resolves to nothing instead.
		return {};
	}
	if (wanted.empty())
		return {};

	const auto lower = [](std::string text) {
		std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	};
	const auto target = lower(wanted);
	std::vector<std::filesystem::path> dirs;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(ostDir, ec)) {
		if (!entry.is_directory())
			continue;
		const auto name = lower(u8(entry.path().filename()));
		if (name == target || (name.size() > target.size() && name.ends_with(target)))
			dirs.push_back(entry.path());
	}
	return dirs;
}

std::optional<std::filesystem::path> resolve_source_name(
	const std::filesystem::path& ostDir,
	const nlohmann::json& config,
	const std::vector<std::filesystem::path>& dirs,
	const nlohmann::json& patterns) {

	// (pattern, the directories to look in -- empty meaning the album's own)
	std::vector<std::pair<std::string, std::vector<std::filesystem::path>>> alternatives;
	const auto add = [&](const nlohmann::json& one) {
		if (one.is_string()) {
			alternatives.emplace_back(one.get<std::string>(), dirs);
		} else if (one.is_object() && one.contains("pattern")) {
			auto scoped = dirs;
			if (const auto directory = one.find("directory"); directory != one.end() && directory->is_string())
				scoped = resolve_search_directories(ostDir, config, directory->get<std::string>());
			alternatives.emplace_back(one.at("pattern").get<std::string>(), std::move(scoped));
		}
	};

	if (patterns.is_object() && patterns.contains("inputFiles")) {
		const auto& files = patterns.at("inputFiles");
		if (files.is_array() && !files.empty()) {
			// Only the first entry: this path replaces one stream, so a list of files to
			// join is not something it can honour, and taking the first silently would be
			// worse than the miss that not resolving produces.
			if (files.size() > 1)
				return std::nullopt;
			for (const auto& one : files.front().is_array() ? files.front() : nlohmann::json::array({files.front()}))
				add(one);
		}
	} else if (patterns.is_array()) {
		for (const auto& one : patterns)
			add(one);
	} else {
		add(patterns);
	}

	for (const auto& [pattern, where] : alternatives) {
		std::regex re;
		try {
			re = std::regex(pattern, std::regex::icase);
		} catch (const std::regex_error&) {
			continue;
		}
		std::vector<std::filesystem::path> hits;
		for (const auto& dir : where) {
			std::error_code ec;
			// Recursive: a release with more tracks than one disc holds keeps the rest in
			// a subdirectory, and 131 targets resolved to nothing while this only looked
			// at the album's top level.
			for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
				if (!entry.is_regular_file())
					continue;
				if (std::regex_search(u8(entry.path().filename()), re))
					hits.push_back(entry.path());
			}
		}
		if (hits.size() == 1)
			return hits.front();
		// The same track in two encodings is one track, and the album's own lossless copy
		// is the one to take -- an .mp3 beside a .flac is a convenience copy, not a rival.
		if (hits.size() > 1) {
			std::vector<std::filesystem::path> lossless;
			for (const auto& hit : hits) {
				const auto extension = hit.extension();
				if (extension == L".flac" || extension == L".wav")
					lossless.push_back(hit);
			}
			if (lossless.size() == 1)
				return lossless.front();
			throw std::runtime_error(std::format("\"{}\" names {} files; it has to name one.", pattern, hits.size()));
		}
	}
	return std::nullopt;
}

std::string collect_config_target_path(const nlohmann::json& target) {
	if (const auto path = target.find("path"); path != target.end()) {
		if (path->is_string())
			return path->get<std::string>();
		if (path->is_array() && !path->empty() && path->front().is_string())
			return path->front().get<std::string>();
	}
	return "(unnamed target)";
}

std::optional<config_target> read_config_target(
	const std::filesystem::path& ostDir,
	const nlohmann::json& config,
	const nlohmann::json& sourceSpec,
	const nlohmann::json& target,
	std::vector<std::pair<std::string, std::string>>& unresolved) {

	if (target.value("enable", true) == false)
		return std::nullopt;
	// Whether this file's offsets are complete, which decides what an absent one means.
	const auto offsetsAreFitted = config.value("offsetsAreFitted", false);
	// A target that needs no offset and no filter says so by leaving `segments` out
	// altogether -- it plays its source from the start, whole. 54 targets, most of them
	// Orchestrion rolls, are written that way, and skipping them for want of the key
	// lost every one.
	const auto segmentsJson = target.find("segments");
	const auto hasSegments = segmentsJson != target.end() && segmentsJson->is_array() && !segmentsJson->empty();
	if (!hasSegments && target.contains("segments"))
		return std::nullopt;   // present but empty: the entry says nothing to build

	std::vector<std::string> paths;
	if (const auto path = target.find("path"); path != target.end()) {
		if (path->is_string())
			paths.push_back(path->get<std::string>());
		else if (path->is_array())
			for (const auto& one : *path)
				if (one.is_string())
					paths.push_back(one.get<std::string>());
	}
	if (paths.empty())
		return std::nullopt;

	// "source" is either one list of alternatives -- the implicit name "source" -- or
	// an object of named ones, which is what a multi-source segment refers to.
	std::map<std::string, nlohmann::json> named;
	if (sourceSpec.is_object())
		for (const auto& [name, patterns] : sourceSpec.items())
			named.emplace(name, patterns);
	else
		named.emplace("source", sourceSpec);

	const auto dirs = resolve_search_directories(ostDir, config);
	std::map<std::string, std::filesystem::path> resolved;
	std::map<std::string, std::shared_ptr<apply_source_graph>> graphs;
	for (const auto& [name, patterns] : named) {
		// A source may be built by a filter graph rather than read whole from one file:
		// several inputs layered rather than sequenced, which is what BGM_EX4_Event_15's
		// three copies of one recording at 0s, 75.195s and 150.390s are. `filterGraph` is
		// the JSON form; `filterComplex` is the escaped string the hand-written presets
		// have always used, and both compile to the same ffmpeg argument.
		const auto hasGraph = patterns.is_object()
			&& (patterns.contains("filterGraph") || patterns.contains("filterComplex"));
		if (hasGraph) {
			try {
				auto built = std::make_shared<apply_source_graph>();
				const auto outName = patterns.value("filterComplexOutName", std::string{});
				std::vector<size_t> wanted;
				if (const auto graphJson = patterns.find("filterGraph"); graphJson != patterns.end()) {
					const auto compiled = compile_filter_graph(*graphJson, outName);
					built->Description = compiled.Description;
					built->OutLabel = compiled.OutLabel;
					wanted = compiled.UsedInputs;
				} else {
					built->Description = patterns.at("filterComplex").get<std::string>();
					if (outName.empty())
						throw std::runtime_error("a filterComplex needs a filterComplexOutName");
					built->OutLabel = outName.size() >= 2 && outName.front() == '['
						? outName.substr(1, outName.size() - 2) : outName;
					// The string form names its inputs as [N:a] inside the description, so
					// which slots it reads is not knowable without parsing it. Resolve them
					// all; a slot it does not read costs one lookup and nothing else.
					const auto& files = patterns.at("inputFiles");
					for (size_t i = 0; i < files.size(); ++i)
						wanted.push_back(i);
				}

				// ffmpeg numbers its inputs by the order they are passed, so every slot up
				// to the highest one read has to be present even if nothing reads it.
				const auto& files = patterns.at("inputFiles");
				const auto slots = wanted.empty() ? size_t{0} : wanted.back() + 1;
				if (slots > files.size())
					throw std::runtime_error(std::format(
						"the graph reads input {} but inputFiles has {} slot(s)", slots - 1, files.size()));
				const std::set used(wanted.begin(), wanted.end());
				for (size_t i = 0; i < slots; ++i) {
					if (!used.contains(i) || (files[i].is_array() && files[i].empty())) {
						// An empty slot means the game's own audio in the old importer's
						// presets. That file does not exist yet -- it is staged per target,
						// later -- so the slot is carried as an intention and filled in at
						// render time. A slot nothing reads only has to hold ffmpeg's
						// numbering, and stays empty in both senses.
						built->Inputs.push_back({.IsTarget = used.contains(i)});
						continue;
					}
					const auto file = resolve_source_name(ostDir, config, dirs, files[i]);
					if (!file)
						throw std::runtime_error(std::format("no file for input {}", i));
					built->Inputs.push_back({.Path = *file});
				}
				graphs.emplace(name, built);
				resolved.emplace(name, built->Inputs.empty() ? std::filesystem::path{} : built->Inputs.front().Path);
				continue;
			} catch (const std::exception& e) {
				unresolved.emplace_back(paths.front(), std::format(
					"source \"{}\" is built by a filter graph: {}", name, e.what()));
				return std::nullopt;
			}
		}
		const auto file = resolve_source_name(ostDir, config, dirs, patterns);
		if (!file) {
			unresolved.emplace_back(paths.front(), std::format("no file for \"{}\"", name));
			return std::nullopt;
		}
		resolved.emplace(name, *file);
	}

	// Every name a segment uses has to be one the item defines. Two entries referred to
	// their recording by its stem while declaring it as a bare array -- which names it
	// `source` -- and the offset attached to the stem name was quietly dropped, leaving
	// BGM_EX5_Boss_Battle03 a second out of step and 0.10 worse than the previous build.
	// Silence is the wrong answer to that, so it is reported and the entry left alone.
	if (hasSegments) {
		for (const auto& segmentJson : *segmentsJson) {
			std::vector<std::string> used;
			for (const auto& key : {"sourceOffsets", "sourceFilters"})
				if (const auto section = segmentJson.find(key); section != segmentJson.end() && section->is_object())
					for (const auto& [name, _spec] : section->items())
						used.push_back(name);
			if (const auto channels = segmentJson.find("channels"); channels != segmentJson.end() && channels->is_array())
				for (const auto& channel : *channels)
					used.push_back(channel.value("source", std::string("source")));
			for (const auto& name : used) {
				if (name == "target" || resolved.contains(name))
					continue;
				unresolved.emplace_back(paths.front(), std::format(
					"segment names source \"{}\", which the item does not define", name));
				return std::nullopt;
			}
		}
	}

	// `"target"` names the game's own audio. It is validated above like any other name
	// but resolves to no file, so it is registered here with an empty path and swapped
	// for the staged template at build time. Without this the name passed validation and
	// then threw in the assembler -- "maps a channel from source \"target\", which it
	// does not define" -- which is a promise the builder could not keep.
	auto usesTarget = false;
	if (hasSegments) {
		for (const auto& segmentJson : *segmentsJson) {
			for (const auto& key : {"sourceOffsets", "sourceFilters"})
				if (const auto section = segmentJson.find(key); section != segmentJson.end() && section->is_object())
					for (const auto& [name, _spec] : section->items())
						usesTarget = usesTarget || name == "target";
			if (const auto channels = segmentJson.find("channels"); channels != segmentJson.end() && channels->is_array())
				for (const auto& channel : *channels)
					usesTarget = usesTarget || channel.value("source", std::string("source")) == "target";
		}
	}
	if (usesTarget)
		resolved.emplace("target", std::filesystem::path{});

	std::vector<apply_segment> segments;
	if (!hasSegments) {
		// One default span covering the whole of the single source it names. More than
		// one source with no segments to route them through says nothing about which
		// channel each feeds, so there is nothing to build.
		if (resolved.size() != 1)
			return std::nullopt;
		apply_segment segment;
		segment.Sources.emplace(resolved.begin()->first, apply_segment_source{
			.Path = resolved.begin()->second,
			.Graph = graphs.contains(resolved.begin()->first) ? graphs.at(resolved.begin()->first) : nullptr});
		// Two entries, taken in order: the single-source path below reads only the source
		// and the offset from this, and derives the channel count from the game's own file.
		segment.Channels.emplace_back(resolved.begin()->first, 0);
		segment.Channels.emplace_back(resolved.begin()->first, 1);
		segments.push_back(std::move(segment));
	}
	static const nlohmann::json NoSegments = nlohmann::json::array();
	for (const auto& segmentJson : hasSegments ? *segmentsJson : NoSegments) {
		apply_segment segment{
			.Length = segmentJson.value("length", 0.),
			.CrossfadeSeconds = segmentJson.value("crossfadeSeconds", 0.),
			.StartSeconds = segmentJson.value("startSeconds", -1.),
			.FadeInSeconds = segmentJson.value("fadeInSeconds", -1.),
			.FadeOutSeconds = segmentJson.value("fadeOutSeconds", -1.),
		};
		for (const auto& [name, path] : resolved)
			segment.Sources.emplace(name, apply_segment_source{
				.Path = path,
				.Graph = graphs.contains(name) ? graphs.at(name) : nullptr,
				.IsTarget = name == "target",
				.Fitted = offsetsAreFitted});
		if (const auto offsets = segmentJson.find("sourceOffsets"); offsets != segmentJson.end() && offsets->is_object()) {
			for (const auto& [name, spec] : offsets->items()) {
				if (const auto source = segment.Sources.find(name); source != segment.Sources.end()) {
					source->second.Offset = spec.is_object() ? spec.value("offset", 0.) : spec.get<double>();
					source->second.Stated = true;
				}
			}
		}
		if (const auto shape = segmentJson.find("crossfadeShape"); shape != segmentJson.end() && shape->is_string())
			segment.CrossfadeEqualPower = shape->get<std::string>() != "linear";
		if (const auto thresholds = segmentJson.find("sourceThresholds"); thresholds != segmentJson.end() && thresholds->is_object()) {
			for (const auto& [name, value] : thresholds->items())
				if (value.is_number())
					segment.Thresholds.emplace(name, value.get<double>());
		}
		if (const auto filters = segmentJson.find("sourceFilters"); filters != segmentJson.end() && filters->is_object()) {
			for (const auto& [name, filter] : filters->items()) {
				if (const auto source = segment.Sources.find(name); source != segment.Sources.end() && filter.is_string())
					source->second.Filter = xivres::util::unicode::convert<std::wstring>(filter.get<std::string>());
			}
		}
		if (const auto channels = segmentJson.find("channels"); channels != segmentJson.end() && channels->is_array()) {
			for (const auto& channel : *channels)
				segment.Channels.emplace_back(channel.value("source", std::string("source")),
					channel.value("channel", size_t{0}));
		}
		if (segment.Channels.empty())
			return std::nullopt;
		segments.push_back(std::move(segment));
	}
	if (segments.empty())
		return std::nullopt;

	return config_target{
		.Paths = std::move(paths),
		.Segments = std::move(segments),
		.Note = target.value("# comment", std::string{}),
	};
}

std::vector<std::filesystem::path> release_ordered_presets(const std::vector<std::filesystem::path>& roots) {
	std::vector<std::pair<std::string, std::filesystem::path>> files;
	for (const auto& root : roots) {
		std::vector<std::filesystem::path> found;
		if (std::filesystem::is_directory(root)) {
			for (const auto& entry : std::filesystem::directory_iterator(root))
				if (entry.is_regular_file() && entry.path().extension() == L".json")
					found.push_back(entry.path());
		} else {
			found.push_back(root);
		}
		for (auto& one : found) {
			std::string name;
			try {
				std::ifstream f(one, std::ios::binary);
				nlohmann::json head;
				f >> head;
				name = head.value("name", std::string{});
			} catch (const std::exception&) {
				// Unreadable here is reported where it is loaded.
			}
			files.emplace_back(name.empty() ? "ÿ" + u8(one.filename()) : name, std::move(one));
		}
	}
	std::ranges::sort(files);
	std::vector<std::filesystem::path> res;
	res.reserve(files.size());
	for (auto& [_name, path] : files)
		res.push_back(std::move(path));
	return res;
}
