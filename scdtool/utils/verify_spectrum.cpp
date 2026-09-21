#include "pch.h"
#include "verify_audio.h"

#include "fft.h"
#include "stats.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace {
	constexpr auto Bins = SpectrumFft / 2 + 1;

	// Power spectrogram in dB, frames x Bins, floored.
	std::vector<float> spectrogram_db(std::span<const float> x) {
		if (x.size() < SpectrumFft)
			return {};
		const auto frames = 1 + (x.size() - SpectrumFft) / SpectrumHop;
		static const auto win = hann(SpectrumFft);

		std::vector<float> out(frames * Bins);
		std::vector<fft_cplx> buf(SpectrumFft);
		for (size_t f = 0; f < frames; ++f) {
			for (size_t i = 0; i < SpectrumFft; ++i)
				buf[i] = fft_cplx(static_cast<double>(x[f * SpectrumHop + i]) * win[i], 0.);
			fft(buf, false);
			for (size_t k = 0; k < Bins; ++k)
				out[f * Bins + k] = static_cast<float>((std::max)(
					20.0 * std::log10((std::max)(std::abs(buf[k]), 1e-10)), SpectrumFloorDb));
		}
		return out;
	}

	struct band_plan {
		std::vector<std::pair<size_t, size_t>> BinRange;   // [lo, hi) per band
		std::vector<double> Centre;
	};

	// Third-octave bands from 40 Hz to just under Nyquist. A band catching no bin is dropped
	// rather than reported on, which is what keeps the bottom of the range from testifying
	// about nothing.
	band_plan third_octave_plan(size_t rateHz) {
		constexpr double Lo = 40.0;
		const auto hi = static_cast<double>(rateHz) / 2.0 * 0.98;
		const auto count = static_cast<int>(std::floor(std::log2(hi / Lo) * 3));
		band_plan plan;
		for (int i = 0; i < count; ++i) {
			const auto a = Lo * std::pow(2.0, static_cast<double>(i) / 3.0);
			const auto b = Lo * std::pow(2.0, static_cast<double>(i + 1) / 3.0);
			// The Python selects bins by `(freqs >= a) & (freqs < b)` over
			// rfftfreq(Fft, 1/rate), i.e. freq[k] = k * rate / Fft.
			const auto scale = static_cast<double>(SpectrumFft) / static_cast<double>(rateHz);
			const auto first = static_cast<size_t>(std::ceil(a * scale));
			auto last = static_cast<size_t>(std::ceil(b * scale));
			last = (std::min)(last, Bins);
			if (last > first) {
				plan.BinRange.emplace_back(first, last);
				plan.Centre.push_back(std::sqrt(a * b));
			}
		}
		return plan;
	}

	// Long-term average spectrum per band, in dB.
	std::vector<double> ltas(const std::vector<float>& spec, size_t frames, const band_plan& plan) {
		std::vector<double> mean(Bins, 0.);
		for (size_t f = 0; f < frames; ++f)
			for (size_t k = 0; k < Bins; ++k)
				mean[k] += spec[f * Bins + k];
		for (auto& v : mean)
			v /= static_cast<double>(frames);

		std::vector<double> out(plan.Centre.size(), 0.);
		for (size_t b = 0; b < plan.Centre.size(); ++b) {
			const auto [first, last] = plan.BinRange[b];
			double acc = 0;
			for (size_t k = first; k < last; ++k)
				acc += mean[k];
			out[b] = acc / static_cast<double>(last - first);
		}
		return out;
	}

	// The highest frequency still carrying content: where the spectrogram stops.
	//
	// Two corrections, both from readings that turned out to be artefacts. Taking the *first*
	// band to fall below the threshold measures spectral slope rather than bandwidth -- on
	// ordinary music the bass is loudest, so 60 dB down arrives around 7 kHz on files that
	// plainly run to 20 kHz. And a bare threshold on the topmost band is a cliff: on
	// BGM_EX5_Field_Yak_Night the two spectrograms are indistinguishable by eye, both dark
	// above 3.6 kHz, and this reported 4561 Hz against 14482 Hz -- one band hovering either
	// side of the line, 58 dB down, 5 dB apart. So the end has to be a *sustained* fall,
	// scanning down from the top; a single band poking above the floor is not bandwidth.
	double edge_frequency(const std::vector<double>& l, const std::vector<double>& centres, size_t run = 3) {
		if (l.empty())
			return 0.;
		const auto thresh = *std::ranges::max_element(l) - SpectrumEdgeDropDb;
		size_t below = 0;
		for (size_t i = l.size(); i-- > 0;) {
			if (l[i] < thresh) {
				below++;
				continue;
			}
			// The first band from the top with content in it. If the dark stretch above it
			// was not sustained, that darkness is a band or two of noise rather than the end
			// of the spectrum, and the content reaches the top.
			return below >= run ? centres[i] : centres.back();
		}
		return centres.front();
	}
}

spectrum_comparison compare_spectra(
	std::span<const float> built, std::span<const float> game, size_t rateHz) {

	const auto n = (std::min)(built.size(), game.size());
	if (n < SpectrumFft * 8 || !rateHz)
		return {};

	const auto sa = spectrogram_db(built.subspan(0, n));
	const auto sb = spectrogram_db(game.subspan(0, n));
	const auto frames = (std::min)(sa.size(), sb.size()) / Bins;
	if (frames < 4)
		return {};

	const auto plan = third_octave_plan(rateHz);
	if (plan.Centre.size() < 4)
		return {};
	const auto la = ltas(sa, frames, plan);
	const auto lb = ltas(sb, frames, plan);

	// Which bands the game's own file actually puts energy in. The same guard as `span` in
	// the envelope comparison, one axis over: a band at the noise floor in both files has
	// nothing to agree or disagree about, and differencing two floors gives a large
	// meaningless number. The first run of this reported "+45.7 dB at 18 kHz" on a pair whose
	// 18 kHz band is silent in both.
	const auto peak = *std::ranges::max_element(lb);
	std::vector<size_t> live;
	for (size_t b = 0; b < lb.size(); ++b)
		if (lb[b] >= peak - SpectrumAudibleDb)
			live.push_back(b);
	if (live.size() < 4)
		return {};

	std::vector<double> d(la.size());
	for (size_t b = 0; b < la.size(); ++b)
		d[b] = la[b] - lb[b];
	std::vector<double> liveDiff;
	liveDiff.reserve(live.size());
	for (const auto b : live)
		liveDiff.push_back(d[b]);
	const auto bias = median_of(liveDiff);
	for (auto& v : d)
		v -= bias;

	spectrum_comparison out{.Valid = true};

	// Tilt: least-squares slope of the band difference against log10(frequency).
	if (live.size() > 2) {
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		for (const auto b : live) {
			const auto x = std::log10(plan.Centre[b]);
			sx += x;
			sy += d[b];
			sxx += x * x;
			sxy += x * d[b];
		}
		const auto m = static_cast<double>(live.size());
		if (const auto den = m * sxx - sx * sx; den != 0)
			out.TiltDbPerDecade = (m * sxy - sx * sy) / den;
	}

	auto worst = live.front();
	for (const auto b : live)
		if (std::abs(d[b]) > std::abs(d[worst]))
			worst = b;
	out.WorstBandDb = d[worst];
	out.WorstBandHz = plan.Centre[worst];

	out.EdgeBuiltHz = edge_frequency(la, plan.Centre);
	out.EdgeGameHz = edge_frequency(lb, plan.Centre);

	// The robust top-end statistic: how far the build's high bands sit from the game's, in
	// dB, averaged rather than thresholded. `edge` answers the same question through a
	// crossing, which is unstable wherever the spectrum lies near the floor.
	double hfSum = 0;
	size_t hfCount = 0;
	for (const auto b : live) {
		if (plan.Centre[b] >= SpectrumHfFromHz) {
			hfSum += d[b];
			hfCount++;
		}
	}
	out.HfDb = hfCount >= 2 ? hfSum / static_cast<double>(hfCount) : 0.;

	// The worst sustained band-and-time hole, smoothed over a second: a single frame of a
	// single band is noise, and the thing worth finding is a band that goes away and stays
	// away long enough to hear.
	const auto secondsPerFrame = static_cast<double>(SpectrumHop) / static_cast<double>(rateHz);
	const auto k = (std::max<size_t>)(1, static_cast<size_t>(std::llround(1.0 / secondsPerFrame)));
	if (frames < k)
		return out;

	std::vector<double> diff(frames), sorted;
	sorted.reserve(frames);
	auto deepest = 0.;
	for (const auto b : live) {
		const auto [first, last] = plan.BinRange[b];
		const auto width = static_cast<double>(last - first);
		sorted.clear();
		for (size_t f = 0; f < frames; ++f) {
			double acc = 0;
			for (size_t kk = first; kk < last; ++kk)
				acc += static_cast<double>(sb[f * Bins + kk]) - static_cast<double>(sa[f * Bins + kk]);
			diff[f] = acc / width;
			sorted.push_back(diff[f]);
		}
		const auto bandBias = median_of(sorted);

		double acc = 0;
		for (size_t f = 0; f < k; ++f)
			acc += diff[f] - bandBias;
		auto best = acc;
		size_t bestAt = 0;
		for (size_t f = 1; f + k <= frames; ++f) {
			acc += diff[f + k - 1] - diff[f - 1];
			if (acc > best) {
				best = acc;
				bestAt = f;
			}
		}
		// Compared uncapped and capped only on the way out. Comparing against the stored
		// value after capping lets a 70 dB band displace a 200 dB one, because the stored
		// value is 60 by then -- so the reported band would be whichever deep hole happened
		// to come last rather than the worst.
		if (const auto depth = best / static_cast<double>(k); depth > deepest) {
			deepest = depth;
			// Capped: against a -120 dB floor a band simply silent in the build reads as a
			// hundred-plus dB "hole", which is true but is the whole-file silence the census
			// already lists rather than a spectral finding. At the cap, go read that.
			out.PatchDb = (std::min)(depth, SpectrumPatchCapDb);
			out.PatchHz = plan.Centre[b];
			out.PatchAtSeconds = (static_cast<double>(bestAt) + static_cast<double>(k) / 2.)
				* secondsPerFrame;
		}
	}
	return out;
}
