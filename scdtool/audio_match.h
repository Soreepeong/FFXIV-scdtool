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
// normalized (cosine similarity, range roughly [-1, 1]) score found at any offset
// whose overlap is at least minOverlapSeconds long. Returns -2 if no offset has
// sufficient overlap.
double best_envelope_correlation(
	const std::vector<float>& envA,
	const std::vector<float>& envB,
	double envRateHz,
	double maxOffsetSeconds,
	double minOverlapSeconds);
