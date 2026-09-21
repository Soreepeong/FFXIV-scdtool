#pragma once

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

// In-place radix-2 FFT, power-of-two lengths only.
//
// Header-only and shared rather than duplicated: the envelope correlation and the log-mel
// spectrogram in audio_match.cpp used it first, and the third-octave spectrogram comparison
// needs the same thing at a different length. Two copies of a transform is two places for a
// sign or a normalisation to drift apart, and a difference there is invisible in every test
// short of comparing the two outputs directly.
using fft_cplx = std::complex<double>;

inline void fft(std::vector<fft_cplx>& a, bool invert) {
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
		const fft_cplx wlen(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len) {
			fft_cplx w(1);
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

// Symmetric Hann window, matching numpy's np.hanning: 0.5 - 0.5*cos(2*pi*i/(n-1)). The
// periodic variant differs in the last sample, which is a fraction of a dB on a single bin
// and enough to make two implementations disagree in the fourth decimal.
inline std::vector<double> hann(size_t n) {
	std::vector<double> w(n);
	for (size_t i = 0; i < n; ++i)
		w[i] = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n - 1));
	return w;
}
