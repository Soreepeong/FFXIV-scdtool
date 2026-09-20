#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A Vorbis encoder whose output decodes back to bit-identical 16-bit PCM.
//
// Every stage people take for lossy about Vorbis is encoder policy rather than a limit of
// the format. The MDCT window satisfies Princen-Bradley, so the transform pair is exactly
// invertible in exact arithmetic; the floor is a pointwise multiply by values from the
// normative inverse-dB table, and with floor1 at zero partitions it is one known constant;
// the residue codebooks are author-defined, so nothing caps precision but the eight-stage
// cascade and float32. What *is* real is that Vorbis does not specify bit-exact decoder
// output, so the guarantee is "exact against this decoder", which is libvorbis -- the same
// code the game's Miles decoder derives from.
//
// Ported from the Python prototype in scratch/lossless-ogg, which established the design and
// measured every constant in it. Two of its nine modules are gone here: the Ogg page writer
// is libogg, and the decode-in-the-loop is libvorbis in process rather than an ffmpeg
// subprocess per iteration.
namespace lossless_vorbis {

	// How the playback path turns a decoded float into an integer sample. Measured in the
	// game's Miles decoder: the scale is 32767.0f everywhere, but the conversion is not
	// consistent -- the mono and stereo bulk loops use CVTPS2DQ, which rounds to nearest
	// even, while the unaligned head, the tail remainder and the whole 3-or-more-channel
	// path use CVTTSS2SI, which truncates toward zero.
	//
	// Aiming at an exact integer is wrong for a truncating decoder: any negative error drops
	// the sample by one. Each mode names the point to aim at, offset away from zero, and how
	// far the decoded value may stray before the integer changes.
	enum class rounding {
		Round,      // x in [n-0.5, n+0.5)  -> aim n,      tolerance 0.5
		Truncate,   // x in [n, n+1)        -> aim n+0.5,  tolerance 0.5
		Either,     // the intersection     -> aim n+0.25, tolerance 0.25
	};

	struct options {
		size_t BlockSize = 1024;
		// Bins of one channel covered by a residue partition. The partition size written to
		// the header is this times the channel count: type-2 residue interleaves channels and
		// steps through the vector as offset/ch, so a partition that is not a whole number of
		// channel-groups desynchronises the round-robin and corrupts every channel.
		size_t BinsPerPartition = 8;
		// Sizes the coefficient grid from an RMS model. Right for ordinary material,
		// optimistic where coefficient errors correlate, which is what the retry loop is for.
		double LsbBudget = 0.05;
		// How far inside the mode's tolerance the worst sample has to sit before the stream is
		// accepted. The headroom is what absorbs a different decoder build disagreeing.
		double MinMargin = 4.0;
		size_t MaxIterations = 4;
		size_t MaxAttempts = 4;
		rounding Rounding = rounding::Either;
		std::vector<std::string> Comments;   // Vorbis comment tags, e.g. "LoopStart=1234"
	};

	struct result {
		std::vector<uint8_t> Ogg;
		bool Exact = false;
		size_t Mismatches = 0;      // samples the decoder would not reproduce
		double WorstErrorLsb = 0.;  // worst |error| against the aim point, in output LSBs
		double BitsPerSample = 0.;
		size_t Iterations = 0;
		size_t Attempts = 0;
		size_t Levels = 0;          // values per cascade rung
		size_t Stages = 0;          // rungs in the ladder
	};

	// `samples` is interleaved 16-bit PCM, `channels` wide. Throws on failure to produce a
	// stream at all; a stream that came out inexact is returned with Exact false and the
	// count, for the caller to judge.
	result encode(const std::vector<int16_t>& samples, size_t channels, size_t rate,
		const options& opts = {});

	// Decode an Ogg Vorbis stream to interleaved float32 through libvorbis, in process.
	// Exposed because verifying a stream is the same operation the encoder does internally.
	std::vector<float> decode(const std::vector<uint8_t>& ogg, size_t& channelsOut);

}
