#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
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

// Every locally-best alignment, not just the winner, best score first.
//
// A release structured intro + loop + loop correlates almost equally well at each loop
// pass, because an envelope is near-periodic over a loop. Returning only the maximum
// therefore picks between them by noise: measured over presets-new, 65 of 583 entries had
// an offset one or more loop bodies late, which plays loop content where the game's
// play-once intro belongs. The peaks are all plausible and the envelope cannot rank them,
// so hand them to a caller that can.
struct offset_candidate {
	double OffsetSeconds = 0.;
	double Score = -2.;
	double OverlapSeconds = 0.;
};

std::vector<offset_candidate> envelope_offset_candidates(
	const std::vector<float>& envA,
	const std::vector<float>& envB,
	double envRateHz,
	double maxOffsetSeconds,
	double minOverlapSeconds,
	size_t maxCandidates = 12,
	double minSeparationSeconds = 2.0);

// Log-mel spectrogram, row-major [frame][band], each frame mean-centred across bands and
// then scaled to unit length, so comparing an alignment is a plain dot product.
//
// This is the signal that tells loop passes apart, which an envelope cannot: it carries
// timbre, so the same loudness shape played on different instruments no longer matches.
constexpr size_t LogMelBands = 64;
constexpr double LogMelFrameRateHz = 100.;   // 160-sample hop at 16 kHz

std::vector<float> decode_logmel(
	const std::filesystem::path& ffmpeg,
	const std::filesystem::path& mediaFile,
	double maxSeconds = 0.);

// The decode every one of these measurements starts from: mono, 16 kHz, signed 16-bit.
// Exposed because the verification metrics need the samples themselves and not only the
// spectrogram -- the level weight that makes a build score readable is the game file's own
// RMS per frame, and the peak envelope is a different reduction of the same samples. One
// decode feeding all of them also means a sweep runs ffmpeg twice per file rather than six
// times.
constexpr size_t AnalysisRateHz = 16000;

std::vector<int16_t> decode_mono_16k(
	const std::filesystem::path& ffmpeg,
	const std::filesystem::path& mediaFile,
	double maxSeconds = 0.);

// The same decode at a rate of the caller's choosing. The loop-seam measurement needs the
// file's own rate: a click is a single-sample discontinuity, and resampling to 16 kHz spreads
// it over neighbouring samples until it reads as ordinary content.
std::vector<int16_t> decode_mono(
	const std::filesystem::path& ffmpeg,
	const std::filesystem::path& mediaFile,
	size_t rateHz,
	double maxSeconds = 0.);

// Float rather than int16, and not as a convenience: ffmpeg's `-ac 1` downmix scales
// differently for integer output than for float. On BGM_EX4_Raid_10 the game file's peak
// around 3.3s reads -3.56 dB decoded as f32le and -6.57 dB as s16le -- exactly 3.01 dB, a
// constant factor, which propagated straight into the measured depth of every deep hole.
// Every threshold and every score already written down came from the float path, so that is
// the one to match.
std::vector<float> decode_mono_float(
	const std::filesystem::path& ffmpeg,
	const std::filesystem::path& mediaFile,
	size_t rateHz = AnalysisRateHz,
	double maxSeconds = 0.);

// Log-mel of already-decoded samples, so a caller holding them does not decode again.
std::vector<float> logmel_from_samples(std::span<const float> samples);

// Mean per-frame cosine similarity between a span of `target` and `source` read from
// `offsetSeconds`, i.e. target frame t is compared against source frame t - offset.
// Returns -2 if the span does not lie inside both. Pass the game file's pre-loop region
// as the span: that is where alignments genuinely differ, and averaging over the whole
// file dilutes it away -- BGM_MJI_01 scores 0.9959 vs 0.9944 over 240s and 0.9957 vs
// 0.8457 over its 2s intro, the same decision hidden 100x over.
double spectral_similarity_at(
	const std::vector<float>& target,
	const std::vector<float>& source,
	double offsetSeconds,
	double fromSeconds,
	double toSeconds);
