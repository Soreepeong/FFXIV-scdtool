#pragma once

#include <cstdint>
#include <span>
#include <vector>

// What a built .scd is measured by, against the game's own file it replaces.
//
// Four metrics, because each is blind to what the others catch, and every one of those blind
// spots was found the hard way:
//
//   build score       the per-frame log-mel cosine, weighted by the game file's own level.
//                     The verdict. Blind to anything local: a two-second dropout in a
//                     two-minute piece moves it about a percent, which is inside the spread
//                     between a good match and a very good one.
//   peak envelope     the fb2k seekbar view. `hole` is the metric the score cannot supply,
//                     and the one a player cannot fail to notice.
//   silence           stretches where the build is digitally silent and the game is not,
//                     reported against the loop end because silence past it is never played.
//   loop seam         whether the loop point clicks, judged from the built file alone.
//
// The constants are the Python originals' and are not free parameters -- each was set from a
// full sweep of the library and is recorded with its reasoning in the scratch scripts these
// replace. Changing one invalidates every score already written down.

// Per-bucket peak envelope in dB, floored, at `bucketSeconds` resolution. The reduction fb2k
// draws, and the one Audacity's waveform pane draws.
constexpr double EnvelopeBucketSeconds = 0.020;
constexpr size_t EnvelopeCoarseFactor = 10;      // 200 ms, a seekbar column
constexpr double EnvelopeFloorDb = -70.0;

std::vector<double> peak_envelope_db(std::span<const int16_t> samples, size_t bucketSamples);

// The level-weighted log-mel cosine, and the plain mean for comparison.
//
// A cosine between two digital silences is noise, and a good many targets are short cues
// padded to a round length; unweighted those read as disagreement when every audible second
// matches. Only the span the two share is scored: a replacement routinely runs past the file
// it replaces and the game plays to its loop end regardless.
struct build_score_result {
	double Weighted = 0.;
	double Plain = 0.;
	bool Valid = false;
};

build_score_result build_score(std::span<const int16_t> built, std::span<const int16_t> game);

// (r_eye, r_fine, dev, span, hole, holeAt, audible) between two dB envelopes.
//
// `span` is the standard deviation of the game file's own coarse envelope, and it is what
// makes the correlations readable: a correlation is a ratio to the variance present, so on a
// track with no variance -- a dense battle piece compressed to a flat block, which this game
// has hundreds of -- it measures noise. Below `FlatSpanDb` the envelope view is not failing
// the file, it is declining to testify, and `Dev` is the number to read.
constexpr double FlatSpanDb = 2.0;

struct envelope_comparison {
	double REye = 0.;        // 200 ms columns: the picture a seekbar shows
	double RFine = 0.;       // 20 ms buckets: sensitive to exact alignment
	double Dev = 0.;         // median absolute deviation between the two, dB
	double Span = 0.;        // the game file's own coarse spread, dB
	double Hole = 0.;        // worst sustained place the build is quieter, dB
	double HoleAt = 0.;      // seconds into the file
	double Audible = 0.;     // fraction of buckets above the floor
	bool Valid = false;
};

envelope_comparison compare_envelopes(std::span<const double> builtDb, std::span<const double> gameDb);

// A stretch where the build is silent and the game is plainly playing.
constexpr double SilentDb = -70.0;
constexpr double LoudDb = -40.0;
constexpr double SilenceMinRunSeconds = 0.1;

struct silence_run {
	double FromSeconds = 0.;
	double ToSeconds = 0.;
	double GameLevelDb = 0.;
};

// `loopEndSeconds` <= 0 means the file does not loop, so nothing is trimmed.
std::vector<silence_run> silence_gaps(
	std::span<const double> builtDb, std::span<const double> gameDb, double loopEndSeconds);

// seam jump / largest local delta either side of it, at the file's own rate.
//
// Scale-invariant rather than amplitude-normalised, which is what makes it comparable across
// tracks: calibrated against a known pair, a buggy build read 1.38-5.72 and the game's own
// files 0.10-0.60. Above about 1 the seam is a bigger jump than anything happening nearby,
// which ordinary percussive content does not explain. Detects an acoustic click only -- a
// loop that is musically misaligned but lands somewhere locally continuous reads clean.
constexpr double SeamWindowSeconds = 0.020;

struct seam_result {
	double Ratio = 0.;
	double Jump = 0.;
	double MaxLocalDelta = 0.;
	bool Valid = false;
};

seam_result loop_seam_ratio(
	std::span<const int16_t> samples, size_t loopStartSample, size_t loopEndSample, size_t rate);
