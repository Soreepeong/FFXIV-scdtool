#include "pch.h"
#include "audio_match.h"

#include "win32_process.h"

#include <cmath>
#include <complex>
#include <numbers>

namespace {
	using cplx = std::complex<double>;

	size_t next_pow2(size_t n) {
		size_t v = 1;
		while (v < n)
			v <<= 1;
		return v;
	}

	// Iterative in-place radix-2 Cooley-Tukey FFT. a.size() must be a power of two.
	void fft(std::vector<cplx>& a, bool invert) {
		const auto n = a.size();
		for (size_t i = 1, j = 0; i < n; ++i) {
			size_t bit = n >> 1;
			for (; j & bit; bit >>= 1)
				j ^= bit;
			j ^= bit;
			if (i < j)
				std::swap(a[i], a[j]);
		}

		for (size_t len = 2; len <= n; len <<= 1) {
			const double ang = 2 * std::numbers::pi / static_cast<double>(len) * (invert ? 1 : -1);
			const cplx wlen(std::cos(ang), std::sin(ang));
			for (size_t i = 0; i < n; i += len) {
				cplx w(1);
				for (size_t j = 0; j < len / 2; ++j) {
					const auto u = a[i + j];
					const auto v = a[i + j + len / 2] * w;
					a[i + j] = u + v;
					a[i + j + len / 2] = u - v;
					w *= wlen;
				}
			}
		}

		if (invert) {
			for (auto& x : a)
				x /= static_cast<double>(n);
		}
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
	const auto hopCount = samples.size() / static_cast<size_t>(hopSamples);

	std::vector<float> envelope(hopCount);
	for (size_t i = 0; i < hopCount; ++i) {
		double sumSquares = 0;
		for (size_t j = 0; j < static_cast<size_t>(hopSamples); ++j) {
			const double v = samples[i * hopSamples + j];
			sumSquares += v * v;
		}
		envelope[i] = static_cast<float>(std::sqrt(sumSquares / hopSamples));
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
	if (const auto byFraction = static_cast<int64_t>(minOverlapFraction * static_cast<double>(std::min(la, lb)));
		byFraction > minOverlap)
		minOverlap = byFraction;

	envelope_correlation_result best;
	for (int64_t o = -maxOff; o <= maxOff; ++o) {
		const int64_t iA = std::max<int64_t>(0, o);
		const int64_t iB = std::max<int64_t>(0, -o);
		const int64_t len = std::min<int64_t>(static_cast<int64_t>(la), o + static_cast<int64_t>(lb)) - iA;
		if (len < minOverlap || iA >= static_cast<int64_t>(la) || iB >= static_cast<int64_t>(lb))
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
