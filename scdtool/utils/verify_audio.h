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

std::vector<double> peak_envelope_db(std::span<const float> samples, size_t bucketSamples);

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

build_score_result build_score(std::span<const float> built, std::span<const float> game);

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
	std::span<const float> samples, size_t loopStartSample, size_t loopEndSample, size_t rate);

// ---------------------------------------------------------------------------------------
// The Audacity spectrogram pane, as the envelope above is its waveform pane.
//
// Not already covered by the build score, for three reasons. **Bandwidth**: the log-mel runs
// at 16 kHz, so nothing above 8 kHz has ever been measured here -- a codec lowpass, a
// resample's rolloff, cymbals and air are all outside it, and a hard line across the top of a
// spectrogram is the first thing an eye catches. **Level**: the cosine is computed on
// mean-centred unit vectors, which is what makes it immune to gain and also what makes it
// blind to spectral tilt. **Locality**: it is a whole-file average, so a band that drops out
// for a few seconds moves it by a fraction of a percent -- the same blind spot the envelope
// work hit, one axis over.
constexpr size_t SpectrumMaxRateHz = 48000;   // past this the game's own files carry nothing
constexpr size_t SpectrumFft = 2048;
constexpr size_t SpectrumHop = 1024;
constexpr double SpectrumFloorDb = -120.0;
constexpr double SpectrumEdgeDropDb = 60.0;   // how far below the peak counts as "gone dark"
constexpr double SpectrumAudibleDb = 60.0;    // quieter than this below peak is floor, not content
constexpr double SpectrumHfFromHz = 5000.0;   // where "the top end" starts
constexpr double SpectrumPatchCapDb = 60.0;   // deeper is the silence the census already lists

struct spectrum_comparison {
	double TiltDbPerDecade = 0.;   // positive: the build is brighter than the game's file
	double WorstBandDb = 0.;       // largest difference in any third-octave band
	double WorstBandHz = 0.;
	double EdgeBuiltHz = 0.;       // where each spectrogram goes dark at the top
	double EdgeGameHz = 0.;
	double PatchDb = 0.;           // worst sustained band-and-time hole
	double PatchHz = 0.;
	double PatchAtSeconds = 0.;
	double HfDb = 0.;              // mean band difference above SpectrumHfFromHz
	bool Valid = false;
};

// `built` and `game` must already be decoded mono at `rateHz`, the lower of the two files'
// rates capped at SpectrumMaxRateHz: a 96 kHz build against a 44.1 kHz game file has nothing
// to compare above 22 kHz, and resampling the game's file up would invent it.
spectrum_comparison compare_spectra(
	std::span<const float> built, std::span<const float> game, size_t rateHz);
