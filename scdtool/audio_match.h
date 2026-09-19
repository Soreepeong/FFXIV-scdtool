#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

// Decodes the given media file's first audio stream to mono 16kHz PCM via ffmpeg,
// then reduces it to an RMS envelope sampled at (16000 / hopSamples) Hz. This is
// the signal used for cross-correlation-based matching: a short-time energy curve
// is far cheaper to correlate than raw audio, and is invariant to any lossy vs.
// lossless re-encode of the same underlying performance.
std::vector<float> decode_envelope(const std::filesystem::path& ffmpeg, const std::filesystem::path& mediaFile, int hopSamples = 80);

// Cross-correlates two envelopes (as produced by decode_envelope, same hop rate)
// over offsets in [-maxOffsetSeconds, +maxOffsetSeconds], and returns the best
// normalized (cosine similarity, range roughly [-1, 1]) score found over alignments
// that overlap by at least minOverlapSeconds AND by at least minOverlapFraction of the
// shorter envelope. Returns -2 if no alignment qualifies.
//
// The fraction requirement matters once the offset range is wide: taken over enough
// offsets, some short window of any candidate will correlate well by chance, which
// inflates every candidate's score toward 1 and destroys the margin over the true
// match. A genuine OST/SCD pair is a contiguous excerpt, so its correct alignment
// covers nearly the whole shorter signal.
double best_envelope_correlation(
	const std::vector<float>& envA,
	const std::vector<float>& envB,
	double envRateHz,
	double maxOffsetSeconds,
	double minOverlapSeconds,
	double minOverlapFraction = 0.8);

// Like best_envelope_correlation, but also reports where the best alignment sits and
// how long it is, which is what makes a result reviewable by hand.
struct envelope_correlation_result {
	double Score = -2.;
	double OffsetSeconds = 0.;
	double OverlapSeconds = 0.;
};

envelope_correlation_result best_envelope_correlation_ex(
	const std::vector<float>& envA,
	const std::vector<float>& envB,
	double envRateHz,
	double maxOffsetSeconds,
	double minOverlapSeconds,
	double minOverlapFraction = 0.8);
