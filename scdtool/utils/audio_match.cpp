#include "pch.h"
#include "audio_match.h"

#include "fft.h"

#include "win32_process.h"

#include <complex>
#include <numbers>
#include <utility>

namespace {
	using cplx = fft_cplx;

	size_t next_pow2(size_t n) {
		size_t v = 1;
		while (v < n)
			v <<= 1;
		return v;
	}

	// The FFT itself lives in fft.h: the third-octave spectrogram comparison needs the same
	// transform at a different length, and two copies of one is two places for a sign or a
	// normalisation to drift apart.

	constexpr size_t LogMelFft = 1024;
	constexpr size_t LogMelHop = 160;
	constexpr size_t LogMelRate = 16000;
	constexpr size_t LogMelBins = LogMelFft / 2 + 1;

	// Triangular mel-spaced filterbank, 20 Hz to Nyquist, built once.
	const std::vector<float>& mel_filterbank() {
		static const std::vector<float> filters = [] {
			const auto hz2mel = [](double f) { return 2595. * std::log10(1. + f / 700.); };
			const auto mel2hz = [](double m) { return 700. * (std::pow(10., m / 2595.) - 1.); };

			const auto lo = hz2mel(20.);
			const auto hi = hz2mel(static_cast<double>(LogMelRate) / 2.);

			std::vector<size_t> edge(LogMelBands + 2);
			for (size_t i = 0; i < edge.size(); ++i) {
				const auto hz = mel2hz(lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(LogMelBands + 1));
				edge[i] = (std::min)(static_cast<size_t>(std::floor((LogMelFft + 1) * hz / LogMelRate)), LogMelBins - 1);
			}

			std::vector<float> fb(LogMelBands * LogMelBins, 0.f);
			for (size_t i = 0; i < LogMelBands; ++i) {
				const auto l = edge[i], c = edge[i + 1], r = edge[i + 2];
				for (size_t k = l; k < c; ++k)
					fb[i * LogMelBins + k] = static_cast<float>(static_cast<double>(k - l) / static_cast<double>(c - l));
				for (size_t k = c; k < r; ++k)
					fb[i * LogMelBins + k] = static_cast<float>(1. - static_cast<double>(k - c) / static_cast<double>(r - c));
			}
			return fb;
		}();
		return filters;
	}

	const std::vector<float>& hann_window() {
		static const std::vector<float> w = [] {
			std::vector<float> v(LogMelFft);
			// numpy's hanning: symmetric, endpoints zero.
			for (size_t i = 0; i < LogMelFft; ++i)
				v[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(LogMelFft - 1)));
			return v;
		}();
		return w;
	}
}

std::vector<float> decode_envelope(const std::filesystem::path& ffmpeg, const std::filesystem::path& mediaFile, int hopSamples) {
	const auto pcmBytes = run_process_capture_stdout(ffmpeg, {
		L"-v", L"error",
		L"-i", mediaFile.wstring(),
		L"-map", L"0:a:0",
		L"-ac", L"1",
		L"-ar", L"16000",
		L"-f", L"s16le",
		L"-",
	});

	const auto samples = xivres::util::span_cast<const int16_t>(pcmBytes);
	const auto hop = static_cast<size_t>(hopSamples);
	const auto hopCount = samples.size() / hop;

	std::vector<float> envelope(hopCount);
	for (size_t i = 0; i < hopCount; ++i) {
		double sumSquares = 0;
		for (size_t j = 0; j < hop; ++j) {
			const double v = samples[i * hop + j];
			sumSquares += v * v;
		}
		envelope[i] = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(hop)));
	}
	return envelope;
}

double best_envelope_correlation(
	const std::vector<float>& envA,
	const std::vector<float>& envB,
	double envRateHz,
	double maxOffsetSeconds,
	double minOverlapSeconds,
	double minOverlapFraction) {
	return best_envelope_correlation_ex(envA, envB, envRateHz, maxOffsetSeconds, minOverlapSeconds, minOverlapFraction).Score;
}

envelope_correlation_result best_envelope_correlation_ex(
	const std::vector<float>& envA,
	const std::vector<float>& envB,
	double envRateHz,
	double maxOffsetSeconds,
	double minOverlapSeconds,
	double minOverlapFraction) {

	const auto la = envA.size();
	const auto lb = envB.size();
	if (!la || !lb)
		return {};

	double meanA = 0, meanB = 0;
	for (const auto v : envA) meanA += v;
	for (const auto v : envB) meanB += v;
	meanA /= static_cast<double>(la);
	meanB /= static_cast<double>(lb);

	std::vector<double> zA(la), zB(lb);
	for (size_t i = 0; i < la; ++i) zA[i] = envA[i] - meanA;
	for (size_t i = 0; i < lb; ++i) zB[i] = envB[i] - meanB;

	// Prefix sums of squared, zero-meaned envelopes: used to compute the L2 norm
	// of any contiguous sub-range in O(1), for cosine-similarity normalization.
	std::vector<double> prefixA(la + 1, 0.), prefixB(lb + 1, 0.);
	for (size_t i = 0; i < la; ++i) prefixA[i + 1] = prefixA[i] + zA[i] * zA[i];
	for (size_t i = 0; i < lb; ++i) prefixB[i + 1] = prefixB[i] + zB[i] * zB[i];

	const auto nfft = next_pow2(la + lb);
	std::vector<cplx> fa(nfft, cplx(0, 0)), fb(nfft, cplx(0, 0));
	for (size_t i = 0; i < la; ++i) fa[i] = zA[i];
	for (size_t i = 0; i < lb; ++i) fb[i] = zB[i];

	fft(fa, false);
	fft(fb, false);
	for (size_t i = 0; i < nfft; ++i)
		fa[i] *= std::conj(fb[i]);
	fft(fa, true);

	const auto maxOff = static_cast<int64_t>(maxOffsetSeconds * envRateHz);
	auto minOverlap = static_cast<int64_t>(minOverlapSeconds * envRateHz);
	minOverlap = std::max(minOverlap,
		static_cast<int64_t>(minOverlapFraction * static_cast<double>(std::min(la, lb))));

	envelope_correlation_result best;
	for (int64_t o = -maxOff; o <= maxOff; ++o) {
		const int64_t iA = std::max<int64_t>(0, o);
		const int64_t iB = std::max<int64_t>(0, -o);
		const int64_t len = std::min<int64_t>(static_cast<int64_t>(la), o + static_cast<int64_t>(lb)) - iA;
		if (len < minOverlap || std::cmp_greater_equal(iA, la) || std::cmp_greater_equal(iB, lb))
			continue;

		const auto normA = std::sqrt(prefixA[iA + len] - prefixA[iA]);
		const auto normB = std::sqrt(prefixB[iB + len] - prefixB[iB]);
		const auto denom = normA * normB;
		if (denom <= 0)
			continue;

		const auto idx = o >= 0 ? static_cast<size_t>(o) : nfft + static_cast<size_t>(o);
		const auto cos = fa[idx].real() / denom;
		if (cos > best.Score) {
			best.Score = cos;
			best.OffsetSeconds = static_cast<double>(o) / envRateHz;
			best.OverlapSeconds = static_cast<double>(len) / envRateHz;
		}
	}
	return best;
}

std::vector<offset_candidate> envelope_offset_candidates(
	const std::vector<float>& envA,
	const std::vector<float>& envB,
	double envRateHz,
	double maxOffsetSeconds,
	double minOverlapSeconds,
	size_t maxCandidates,
	double minSeparationSeconds) {

	const auto la = envA.size();
	const auto lb = envB.size();
	if (!la || !lb || !maxCandidates)
		return {};

	double meanA = 0, meanB = 0;
	for (const auto v : envA) meanA += v;
	for (const auto v : envB) meanB += v;
	meanA /= static_cast<double>(la);
	meanB /= static_cast<double>(lb);

	std::vector<double> zA(la), zB(lb);
	for (size_t i = 0; i < la; ++i) zA[i] = envA[i] - meanA;
	for (size_t i = 0; i < lb; ++i) zB[i] = envB[i] - meanB;

	std::vector<double> prefixA(la + 1, 0.), prefixB(lb + 1, 0.);
	for (size_t i = 0; i < la; ++i) prefixA[i + 1] = prefixA[i] + zA[i] * zA[i];
	for (size_t i = 0; i < lb; ++i) prefixB[i + 1] = prefixB[i] + zB[i] * zB[i];

	const auto nfft = next_pow2(la + lb);
	std::vector<cplx> fa(nfft, cplx(0, 0)), fb(nfft, cplx(0, 0));
	for (size_t i = 0; i < la; ++i) fa[i] = zA[i];
	for (size_t i = 0; i < lb; ++i) fb[i] = zB[i];

	fft(fa, false);
	fft(fb, false);
	for (size_t i = 0; i < nfft; ++i)
		fa[i] *= std::conj(fb[i]);
	fft(fa, true);

	const auto maxOff = static_cast<int64_t>(maxOffsetSeconds * envRateHz);
	const auto minOverlap = static_cast<int64_t>(minOverlapSeconds * envRateHz);

	// Score every qualifying offset, then keep local maxima. The fractional-overlap rule
	// best_envelope_correlation_ex applies is deliberately not used here: it exists to stop
	// a short chance-correlated window winning outright, but this function's job is to
	// enumerate plausible alignments for a caller that will rank them on a better signal.
	std::vector<std::pair<double, int64_t>> peaks;
	double prev = -2., cur = -2.;
	int64_t curOff = 0;
	bool hasCur = false;
	const auto scoreAt = [&](int64_t o) -> double {
		const int64_t iA = (std::max<int64_t>)(0, o);
		const int64_t iB = (std::max<int64_t>)(0, -o);
		const int64_t len = (std::min<int64_t>)(static_cast<int64_t>(la), o + static_cast<int64_t>(lb)) - iA;
		if (len < minOverlap || std::cmp_greater_equal(iA, la) || std::cmp_greater_equal(iB, lb))
			return -2.;
		const auto normA = std::sqrt(prefixA[iA + len] - prefixA[iA]);
		const auto normB = std::sqrt(prefixB[iB + len] - prefixB[iB]);
		if (normA * normB <= 0)
			return -2.;
		const auto idx = o >= 0 ? static_cast<size_t>(o) : nfft + static_cast<size_t>(o);
		return fa[idx].real() / (normA * normB);
	};

	for (int64_t o = -maxOff; o <= maxOff + 1; ++o) {
		const auto next = o <= maxOff ? scoreAt(o) : -2.;
		if (hasCur && cur > prev && cur >= next && cur > -2.)
			peaks.emplace_back(cur, curOff);
		prev = cur;
		cur = next;
		curOff = o;
		hasCur = true;
	}

	std::ranges::sort(peaks, [](const auto& a, const auto& b) { return a.first > b.first; });

	// Greedy non-maximum suppression: two offsets a fraction of a second apart describe the
	// same alignment, and would otherwise crowd out the genuinely different loop passes that
	// are the whole point of returning more than one.
	const auto minSep = static_cast<int64_t>(minSeparationSeconds * envRateHz);
	std::vector<offset_candidate> out;
	for (const auto& [score, o] : peaks) {
		if (out.size() >= maxCandidates)
			break;
		if (std::ranges::any_of(out, [&](const offset_candidate& c) {
			return std::abs(c.OffsetSeconds - static_cast<double>(o) / envRateHz) * envRateHz < static_cast<double>(minSep);
		}))
			continue;
		const int64_t iA = (std::max<int64_t>)(0, o);
		const int64_t len = (std::min<int64_t>)(static_cast<int64_t>(la), o + static_cast<int64_t>(lb)) - iA;
		out.push_back({static_cast<double>(o) / envRateHz, score, static_cast<double>(len) / envRateHz});
	}
	return out;
}

std::vector<int16_t> decode_mono_16k(
	const std::filesystem::path& ffmpeg,
	const std::filesystem::path& mediaFile,
	double maxSeconds) {
	return decode_mono(ffmpeg, mediaFile, AnalysisRateHz, maxSeconds);
}

std::vector<int16_t> decode_mono(
	const std::filesystem::path& ffmpeg,
	const std::filesystem::path& mediaFile,
	size_t rateHz,
	double maxSeconds) {

	std::vector<std::wstring> args{
		L"-v", L"error",
		L"-i", mediaFile.wstring(),
	};
	if (maxSeconds > 0) {
		args.emplace_back(L"-t");
		args.emplace_back(xivres::util::unicode::convert<std::wstring>(std::format("{:.3f}", maxSeconds)));
	}
	args.insert(args.end(), {L"-map", L"0:a:0", L"-ac", L"1", L"-ar", std::to_wstring(rateHz),
		L"-f", L"s16le", L"-"});

	const auto pcmBytes = run_process_capture_stdout(ffmpeg, args);
	const auto decoded = xivres::util::span_cast<const int16_t>(pcmBytes);
	return {decoded.begin(), decoded.end()};
}

std::vector<float> decode_mono_float(
	const std::filesystem::path& ffmpeg,
	const std::filesystem::path& mediaFile,
	size_t rateHz,
	double maxSeconds) {

	std::vector<std::wstring> args{
		L"-v", L"error",
		L"-i", mediaFile.wstring(),
	};
	if (maxSeconds > 0) {
		args.emplace_back(L"-t");
		args.emplace_back(xivres::util::unicode::convert<std::wstring>(std::format("{:.3f}", maxSeconds)));
	}
	args.insert(args.end(), {L"-map", L"0:a:0", L"-ac", L"1", L"-ar", std::to_wstring(rateHz),
		L"-f", L"f32le", L"-"});

	const auto pcmBytes = run_process_capture_stdout(ffmpeg, args);
	const auto decoded = xivres::util::span_cast<const float>(pcmBytes);
	return {decoded.begin(), decoded.end()};
}

std::vector<float> decode_logmel(
	const std::filesystem::path& ffmpeg,
	const std::filesystem::path& mediaFile,
	double maxSeconds) {
	return logmel_from_samples(decode_mono_float(ffmpeg, mediaFile, AnalysisRateHz, maxSeconds));
}

std::vector<float> logmel_from_samples(std::span<const float> samples) {
	if (samples.size() < LogMelFft)
		return {};

	const auto& fb = mel_filterbank();
	const auto& win = hann_window();
	const auto frames = 1 + (samples.size() - LogMelFft) / LogMelHop;

	std::vector<float> out(frames * LogMelBands);
	std::vector<cplx> buf(LogMelFft);
	std::vector<double> mag(LogMelBins);

	for (size_t f = 0; f < frames; ++f) {
		// Scaled to +/-1 like the reference implementation's float decode; the frame is
		// mean-centred below, so a constant scale would cancel anyway -- but the epsilon
		// inside the logarithm would not, and it is what keeps silence well-behaved.
		for (size_t i = 0; i < LogMelFft; ++i)
			buf[i] = cplx(static_cast<double>(samples[f * LogMelHop + i]) * win[i], 0.);
		fft(buf, false);
		for (size_t k = 0; k < LogMelBins; ++k)
			mag[k] = std::abs(buf[k]);

		const auto row = out.data() + f * LogMelBands;
		double mean = 0;
		for (size_t m = 0; m < LogMelBands; ++m) {
			double acc = 0;
			const auto* filt = fb.data() + m * LogMelBins;
			for (size_t k = 0; k < LogMelBins; ++k)
				acc += mag[k] * filt[k];
			row[m] = static_cast<float>(std::log10(acc + 1e-6));
			mean += row[m];
		}
		mean /= static_cast<double>(LogMelBands);

		// Centre, then normalise to unit length, so scoring an alignment is a plain dot
		// product. Centring is what makes this measure timbre rather than spectral tilt:
		// without it nearly any music scores ~0.9 against any other.
		double norm = 0;
		for (size_t m = 0; m < LogMelBands; ++m) {
			row[m] = static_cast<float>(row[m] - mean);
			norm += static_cast<double>(row[m]) * row[m];
		}
		if (norm > 0) {
			const auto inv = static_cast<float>(1. / std::sqrt(norm));
			for (size_t m = 0; m < LogMelBands; ++m)
				row[m] *= inv;
		}
	}
	return out;
}

double spectral_similarity_at(
	const std::vector<float>& target,
	const std::vector<float>& source,
	double offsetSeconds,
	double fromSeconds,
	double toSeconds) {

	const auto tFrames = static_cast<int64_t>(target.size() / LogMelBands);
	const auto sFrames = static_cast<int64_t>(source.size() / LogMelBands);
	if (tFrames <= 0 || sFrames <= 0)
		return -2.;

	// Same convention as the envelope correlation and as apply's rebasing: target frame t
	// holds the source's content at t - offset.
	const auto shift = std::llround(offsetSeconds * LogMelFrameRateHz);
	auto f0 = (std::max<int64_t>)(0, std::llround(fromSeconds * LogMelFrameRateHz));
	auto f1 = (std::min)(tFrames, std::llround(toSeconds * LogMelFrameRateHz));
	f0 = (std::max)(f0, shift);                  // source index f - shift must be >= 0
	f1 = (std::min)(f1, sFrames + shift);
	if (f1 - f0 < 16)
		return -2.;

	double acc = 0;
	for (int64_t f = f0; f < f1; ++f) {
		const auto* a = target.data() + f * LogMelBands;
		const auto* b = source.data() + (f - shift) * LogMelBands;
		double dot = 0;
		for (size_t m = 0; m < LogMelBands; ++m)
			dot += static_cast<double>(a[m]) * b[m];
		acc += dot;
	}
	return acc / static_cast<double>(f1 - f0);
}
