#include "pch.h"
#include "verify_audio.h"

#include "audio_match.h"
#include "stats.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
	struct pearson_result {
		double R = 0.;
		double Dev = 0.;
		double Live = 0.;
		bool Valid = false;
	};

	// Correlation over the buckets where at least one of the two is above the floor. Scoring
	// the silent ones too would report agreement wherever both files are simply not playing,
	// which on a padded cue is most of its length.
	pearson_result pearson_live(std::span<const double> a, std::span<const double> b) {
		const auto n = (std::min)(a.size(), b.size());
		std::vector<size_t> live;
		for (size_t i = 0; i < n; ++i)
			if (a[i] > EnvelopeFloorDb + 1.0 || b[i] > EnvelopeFloorDb + 1.0)
				live.push_back(i);
		if (live.size() < 8)
			return {};

		double ma = 0, mb = 0;
		for (const auto i : live) {
			ma += a[i];
			mb += b[i];
		}
		ma /= static_cast<double>(live.size());
		mb /= static_cast<double>(live.size());

		double saa = 0, sbb = 0, sab = 0;
		std::vector<double> diffs;
		diffs.reserve(live.size());
		for (const auto i : live) {
			const auto va = a[i] - ma, vb = b[i] - mb;
			saa += va * va;
			sbb += vb * vb;
			sab += va * vb;
			diffs.push_back(std::abs(va - vb));
		}
		const auto den = std::sqrt(saa * sbb);
		return {
			.R = den > 0 ? sab / den : 0.,
			.Dev = median_of(diffs),
			.Live = static_cast<double>(live.size()) / static_cast<double>(n),
			.Valid = true,
		};
	}

	std::vector<double> coarsen(std::span<const double> e, size_t k) {
		const auto n = e.size() / k;
		if (!n)
			return {e.begin(), e.end()};
		std::vector<double> out(n);
		for (size_t i = 0; i < n; ++i)
			out[i] = *std::max_element(e.begin() + i * k, e.begin() + (i + 1) * k);
		return out;
	}

	// (depth in dB, seconds in) of the worst sustained place the build is quieter than the
	// game's own file. Measured over a one-second window, after subtracting the median level
	// difference so a quieter master is not read as a hole.
	std::pair<double, double> worst_hole(std::span<const double> ca, std::span<const double> cb) {
		const auto n = (std::min)(ca.size(), cb.size());
		std::vector<double> liveDiff;
		std::vector<bool> live(n, false);
		for (size_t i = 0; i < n; ++i) {
			if (ca[i] > EnvelopeFloorDb + 1.0 || cb[i] > EnvelopeFloorDb + 1.0) {
				live[i] = true;
				liveDiff.push_back(cb[i] - ca[i]);
			}
		}
		if (liveDiff.size() < 8)
			return {0., 0.};
		const auto bias = median_of(liveDiff);

		std::vector<double> d(n, 0.);
		for (size_t i = 0; i < n; ++i)
			if (live[i])
				d[i] = cb[i] - ca[i] - bias;

		const auto k = (std::max<size_t>)(1, static_cast<size_t>(std::llround(
			1.0 / (EnvelopeBucketSeconds * static_cast<double>(EnvelopeCoarseFactor)))));
		if (d.size() < k)
			return {*std::ranges::max_element(d), 0.};

		// Running mean over k, i.e. numpy's convolve(..., mode="valid").
		double acc = std::accumulate(d.begin(), d.begin() + k, 0.);
		auto best = acc, bestAt = 0.;
		size_t bestIndex = 0;
		for (size_t i = 1; i + k <= d.size(); ++i) {
			acc += d[i + k - 1] - d[i - 1];
			if (acc > best) {
				best = acc;
				bestIndex = i;
			}
		}
		bestAt = (static_cast<double>(bestIndex) + static_cast<double>(k) / 2.)
			* EnvelopeBucketSeconds * static_cast<double>(EnvelopeCoarseFactor);
		return {best / static_cast<double>(k), bestAt};
	}
}

std::vector<double> peak_envelope_db(std::span<const float> samples, size_t bucketSamples) {
	if (!bucketSamples)
		return {};
	const auto n = samples.size() / bucketSamples;
	std::vector<double> out(n);
	for (size_t i = 0; i < n; ++i) {
		double peak = 0;
		for (size_t j = 0; j < bucketSamples; ++j)
			peak = (std::max)(peak, std::abs(static_cast<double>(samples[i * bucketSamples + j])));
		out[i] = (std::max)(20.0 * std::log10((std::max)(peak, 1e-12)), EnvelopeFloorDb);
	}
	return out;
}

build_score_result build_score(std::span<const float> built, std::span<const float> game) {
	const auto n = (std::min)(built.size(), game.size());
	if (n < AnalysisRateHz * 3)
		return {};

	const auto fa = logmel_from_samples(built.subspan(0, n));
	const auto fb = logmel_from_samples(game.subspan(0, n));
	const auto m = (std::min)(fa.size(), fb.size()) / LogMelBands;
	if (!m)
		return {};

	// The weight is the game file's own RMS over each frame's window, so the frames that
	// carry the music decide the score and the padding does not.
	constexpr size_t Fft = 1024, Hop = 160;
	double sumW = 0, sumCosW = 0, sumCos = 0;
	for (size_t f = 0; f < m; ++f) {
		double dot = 0;
		for (size_t b = 0; b < LogMelBands; ++b)
			dot += static_cast<double>(fa[f * LogMelBands + b]) * fb[f * LogMelBands + b];
		sumCos += dot;

		double sq = 0;
		for (size_t i = 0; i < Fft; ++i) {
			const auto v = static_cast<double>(game[f * Hop + i]);
			sq += v * v;
		}
		const auto w = std::sqrt(sq / static_cast<double>(Fft));
		sumW += w;
		sumCosW += dot * w;
	}
	const auto plain = sumCos / static_cast<double>(m);
	return {
		.Weighted = sumW > 0 ? sumCosW / sumW : plain,
		.Plain = plain,
		.Valid = true,
	};
}

envelope_comparison compare_envelopes(std::span<const double> builtDb, std::span<const double> gameDb) {
	const auto fine = pearson_live(builtDb, gameDb);
	if (!fine.Valid)
		return {};

	const auto ca = coarsen(builtDb, EnvelopeCoarseFactor);
	const auto cb = coarsen(gameDb, EnvelopeCoarseFactor);
	const auto eye = pearson_live(ca, cb);

	std::vector<double> loud;
	for (const auto v : cb)
		if (v > EnvelopeFloorDb + 1.0)
			loud.push_back(v);
	double span = 0.;
	if (loud.size() > 1) {
		const auto mean = std::accumulate(loud.begin(), loud.end(), 0.) / static_cast<double>(loud.size());
		double acc = 0;
		for (const auto v : loud)
			acc += (v - mean) * (v - mean);
		span = std::sqrt(acc / static_cast<double>(loud.size()));
	}

	const auto [hole, holeAt] = worst_hole(ca, cb);
	return {
		.REye = eye.Valid ? eye.R : fine.R,
		.RFine = fine.R,
		.Dev = fine.Dev,
		.Span = span,
		.Hole = hole,
		.HoleAt = holeAt,
		.Audible = fine.Live,
		.Valid = true,
	};
}

std::vector<silence_run> silence_gaps(
	std::span<const double> builtDb, std::span<const double> gameDb, double loopEndSeconds) {

	const auto n = (std::min)(builtDb.size(), gameDb.size());
	std::vector<silence_run> out;
	for (size_t i = 0; i < n;) {
		if (!(builtDb[i] <= SilentDb + 0.01 && gameDb[i] >= LoudDb)) {
			++i;
			continue;
		}
		const auto start = i;
		double level = 0;
		while (i < n && builtDb[i] <= SilentDb + 0.01 && gameDb[i] >= LoudDb) {
			level += gameDb[i];
			++i;
		}
		auto from = static_cast<double>(start) * EnvelopeBucketSeconds;
		auto to = static_cast<double>(i) * EnvelopeBucketSeconds;
		if (to - from < SilenceMinRunSeconds)
			continue;
		// Silence at or past the loop end is never played, so it is not a defect.
		if (loopEndSeconds > 0.) {
			if (from >= loopEndSeconds)
				continue;
			to = (std::min)(to, loopEndSeconds);
		}
		out.push_back({from, to, level / static_cast<double>(i - start)});
	}
	return out;
}

seam_result loop_seam_ratio(
	std::span<const float> samples, size_t loopStartSample, size_t loopEndSample, size_t rate) {

	if (!loopEndSample || loopEndSample <= loopStartSample || !rate)
		return {};
	const auto w = (std::max<size_t>)(4, static_cast<size_t>(SeamWindowSeconds * static_cast<double>(rate)));
	if (loopEndSample > samples.size() || loopStartSample + w > samples.size() || loopStartSample < 1)
		return {};

	const auto jump = std::abs(static_cast<double>(samples[loopEndSample - 1])
		- static_cast<double>(samples[loopStartSample]));

	double peak = 0;
	const auto scan = [&](size_t from, size_t to) {
		for (size_t i = from; i + 1 < to; ++i)
			peak = (std::max)(peak, std::abs(static_cast<double>(samples[i + 1])
				- static_cast<double>(samples[i])));
	};
	scan(loopStartSample, loopStartSample + w);
	scan(loopEndSample > w ? loopEndSample - w : 0, loopEndSample);

	return {
		.Ratio = peak > 0 ? jump / peak : std::numeric_limits<double>::infinity(),
		.Jump = jump,
		.MaxLocalDelta = peak,
		.Valid = true,
	};
}
