#include "pch.h"
#include "lossless_vorbis.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <numeric>
#include <queue>
#include <stdexcept>

#include <ogg/ogg.h>
#include <vorbis/codec.h>

namespace {

	using namespace lossless_vorbis;

	// ------------------------------------------------------------------ bit packing ---
	//
	// Vorbis packs least-significant-bit-first within each byte, which is the opposite of
	// most codecs. Everything in the setup header and in audio packets goes through here.
	class bit_writer {
		std::vector<uint8_t> m_buf;
		uint64_t m_acc = 0;
		int m_bits = 0;

	public:
		void write(uint32_t value, int bits) {
			if (bits <= 0)
				return;
			if (bits > 32)
				throw std::runtime_error("bit_writer: width over 32");
			if (bits < 32)
				value &= (1u << bits) - 1;
			m_acc |= static_cast<uint64_t>(value) << m_bits;
			m_bits += bits;
			while (m_bits >= 8) {
				m_buf.push_back(static_cast<uint8_t>(m_acc & 0xFF));
				m_acc >>= 8;
				m_bits -= 8;
			}
		}

		[[nodiscard]] std::vector<uint8_t> take() {
			auto out = std::move(m_buf);
			if (m_bits)
				out.push_back(static_cast<uint8_t>(m_acc & 0xFF));
			m_buf.clear();
			m_acc = 0;
			m_bits = 0;
			return out;
		}
	};

	// Vorbis ilog(): number of significant bits. ilog(0) == 0, ilog(1) == 1.
	constexpr int ilog(uint32_t x) {
		int n = 0;
		while (x) {
			n++;
			x >>= 1;
		}
		return n;
	}

	// ------------------------------------------------------------- packed float32 ---
	//
	// Vorbis stores codebook scalars in a 21-bit-mantissa format of its own, not IEEE.

	double float32_unpack(uint32_t x) {
		auto mantissa = static_cast<double>(x & 0x1FFFFF);
		const auto exponent = static_cast<int>((x & 0x7FE00000) >> 21);
		if (x & 0x80000000)
			mantissa = -mantissa;
		return std::ldexp(mantissa, exponent - 788);
	}

	uint32_t float32_pack(double value) {
		uint32_t sign = 0;
		if (value < 0) {
			sign = 0x80000000;
			value = -value;
		}
		if (value == 0)
			return sign;
		int exponent = 0;
		const auto mantissaFraction = std::frexp(value, &exponent);   // 0.5 <= m < 1
		auto mantissa = static_cast<int64_t>(std::llround(mantissaFraction * (1 << 21)));
		exponent -= 21;
		if (mantissa >= (1 << 21)) {          // rounding pushed into the next binade
			mantissa >>= 1;
			exponent += 1;
		}
		const auto stored = exponent + 788;
		if (stored < 0 || stored >= 1024)
			throw std::runtime_error("lossless: value outside the packed-float exponent range");
		return sign | (static_cast<uint32_t>(stored) << 21) | static_cast<uint32_t>(mantissa);
	}

	// -------------------------------------------------------------------- huffman ---
	//
	// Vorbis wants a complete tree, which plain Huffman over the symbols that occur gives.
	// Depth is limited by raising a floor under the counts and rebuilding rather than by
	// package-merge: it costs a fraction of a bit and only fires on pathological skew.
	std::vector<int> huffman_build(const std::vector<uint64_t>& counts) {
		const auto n = counts.size();
		std::vector<size_t> used;
		for (size_t i = 0; i < n; i++)
			if (counts[i] > 0)
				used.push_back(i);

		if (used.empty())
			used = n >= 2 ? std::vector<size_t>{0, 1} : std::vector<size_t>{0};
		if (used.size() == 1) {
			// A book with one used entry is the format's genuinely ambiguous case, so pad.
			for (size_t i = 0; i < n; i++) {
				if (i != used[0]) {
					used.push_back(i);
					break;
				}
			}
		}
		std::vector<int> lengths(n, 0);
		if (used.size() == 1) {
			lengths[used[0]] = 1;
			return lengths;
		}

		// (weight, tiebreak, members). The tiebreak reproduces the Python heap's ordering so
		// equal-weight groups merge in the same order and the lengths come out identical.
		struct node {
			uint64_t Weight;
			size_t Tie;
			std::vector<size_t> Members;
			bool operator>(const node& other) const {
				return Weight != other.Weight ? Weight > other.Weight : Tie > other.Tie;
			}
		};
		std::priority_queue<node, std::vector<node>, std::greater<>> heap;
		for (size_t i = 0; i < used.size(); i++)
			heap.push({(std::max<uint64_t>)(counts[used[i]], 1), i, {used[i]}});

		auto tie = used.size();
		while (heap.size() > 1) {
			auto a = heap.top();
			heap.pop();
			auto b = heap.top();
			heap.pop();
			for (const auto i : a.Members)
				lengths[i]++;
			for (const auto i : b.Members)
				lengths[i]++;
			a.Members.insert(a.Members.end(), b.Members.begin(), b.Members.end());
			heap.push({a.Weight + b.Weight, tie++, std::move(a.Members)});
		}
		return lengths;
	}

	std::vector<int> huffman_lengths(std::vector<uint64_t> counts, int maxLen = 24) {
		for (int attempt = 0; attempt < 40; attempt++) {
			auto lengths = huffman_build(counts);
			if (*std::ranges::max_element(lengths) <= maxLen)
				return lengths;
			const auto total = std::accumulate(counts.begin(), counts.end(), uint64_t{0});
			const auto floorValue = (std::max<uint64_t>)(1, total >> maxLen);
			auto raised = counts;
			for (auto& c : raised)
				if (c > 0)
					c = (std::max)(c, floorValue);
			if (raised == counts)
				break;
			counts = std::move(raised);
		}
		return huffman_build(counts);
	}

	// ------------------------------------------------------------------- codebook ---

	uint32_t bitreverse(uint32_t value, int bits) {
		uint32_t out = 0;
		for (int i = 0; i < bits; i++)
			out = (out << 1) | ((value >> i) & 1);
		return out;
	}

	struct codebook {
		int Dim = 1;
		int Entries = 0;
		std::vector<int> Lengths;
		int MapType = 0;
		double QMin = 0.;
		double QDelta = 0.;
		int ValueBits = 0;
		int SequenceP = 0;
		std::vector<int> QuantList;

		std::vector<uint32_t> Codewords;
		std::vector<float> Values;        // maptype 1, dim 1: one value per entry

		void finish() {
			Codewords = assign_codewords();
			if (MapType == 1)
				Values = unquantize();
		}

		// Codeword assignment, reproducing libvorbis' _make_words exactly.
		//
		// This is *not* the textbook canonical code. libvorbis walks the entries in index
		// order and hands each the lowest free node at its depth, keeping a per-depth marker
		// cursor and re-dangling the longer markers under the node it just took. For a book
		// whose lengths happen to be non-decreasing with index the two agree, which is why
		// uniform-length books work either way -- but for a trained classbook they produce
		// different (both valid) prefix codes, and the decoder only knows about this one.
		[[nodiscard]] std::vector<uint32_t> assign_codewords() const {
			uint32_t marker[33] = {};
			std::vector<uint32_t> codes(Entries, 0);
			for (int i = 0; i < Entries; i++) {
				const auto length = Lengths[i];
				if (length <= 0)
					continue;
				auto entry = marker[length];
				if (length < 32 && (entry >> length))
					throw std::runtime_error("lossless: code lengths overpopulate the tree");
				codes[i] = entry;

				int j = length;
				while (j > 0) {
					if (marker[j] & 1) {
						// This node is taken; jump to the next branch up.
						if (j == 1)
							marker[1]++;
						else
							marker[j] = marker[j - 1] << 1;
						break;
					}
					marker[j]++;
					j--;
				}
				// Everything longer was dangling off the node just taken; re-hang it.
				for (j = length + 1; j < 33; j++) {
					if ((marker[j] >> 1) == entry) {
						entry = marker[j];
						marker[j] = marker[j - 1] << 1;
					} else {
						break;
					}
				}
			}
			std::vector<uint32_t> out(Entries, 0);
			for (int i = 0; i < Entries; i++)
				if (Lengths[i])
					out[i] = bitreverse(codes[i], Lengths[i]);
			return out;
		}

		// Reproduce _book_unquantize for maptype 1, dim 1. libvorbis evaluates
		// fabs(q)*delta + mindel with fabs promoting to double and stores into a C float, so
		// the expression rounds exactly once, to float32.
		[[nodiscard]] std::vector<float> unquantize() const {
			const auto mindel = static_cast<double>(static_cast<float>(float32_unpack(float32_pack(QMin))));
			const auto delta = static_cast<double>(static_cast<float>(float32_unpack(float32_pack(QDelta))));
			std::vector<float> out(static_cast<size_t>(Entries));
			for (int j = 0; j < Entries; j++)
				out[j] = static_cast<float>(std::abs(static_cast<double>(QuantList[j])) * delta + mindel);
			return out;
		}

		void pack(bit_writer& bw) const {
			bw.write(0x564342, 24);
			bw.write(static_cast<uint32_t>(Dim), 16);
			bw.write(static_cast<uint32_t>(Entries), 24);

			const auto uniform = std::ranges::all_of(Lengths,
				[&](int l) { return l == Lengths[0]; }) && Lengths[0] > 0;
			if (uniform) {
				bw.write(1, 1);                      // ordered
				bw.write(static_cast<uint32_t>(Lengths[0] - 1), 5);
				bw.write(static_cast<uint32_t>(Entries), ilog(static_cast<uint32_t>(Entries)));
			} else {
				bw.write(0, 1);
				const auto sparse = std::ranges::any_of(Lengths, [](int l) { return l == 0; });
				bw.write(sparse ? 1 : 0, 1);
				for (const auto l : Lengths) {
					if (sparse)
						bw.write(l ? 1 : 0, 1);
					if (l)
						bw.write(static_cast<uint32_t>(l - 1), 5);
				}
			}

			bw.write(static_cast<uint32_t>(MapType), 4);
			if (MapType == 1 || MapType == 2) {
				bw.write(float32_pack(QMin), 32);
				bw.write(float32_pack(QDelta), 32);
				bw.write(static_cast<uint32_t>(ValueBits - 1), 4);
				bw.write(static_cast<uint32_t>(SequenceP), 1);
				for (const auto q : QuantList)
					bw.write(static_cast<uint32_t>(q), ValueBits);
			}
		}
	};

	// dim-1 maptype-1 book of `levels` evenly spaced values. With no lengths the codewords
	// are flat, which needs a power of two so the tree is exactly complete.
	codebook uniform_scalar_book(int levels, double delta, double qmin,
		const std::vector<int>* lengths = nullptr) {
		codebook cb;
		cb.Dim = 1;
		cb.Entries = levels;
		cb.MapType = 1;
		cb.QMin = qmin;
		cb.QDelta = delta;
		cb.ValueBits = ilog(static_cast<uint32_t>(levels - 1));
		cb.SequenceP = 0;
		cb.QuantList.resize(static_cast<size_t>(levels));
		std::iota(cb.QuantList.begin(), cb.QuantList.end(), 0);
		cb.Lengths = lengths ? *lengths : std::vector<int>(static_cast<size_t>(levels), cb.ValueBits);
		cb.finish();
		return cb;
	}

	// ---------------------------------------------------------------------- MDCT ---
	//
	// Analysis matched to the decoder's synthesis convention, which was measured against
	// libvorbis rather than taken from memory:
	//     y[t] += X[k] * w[t] * cos(2*pi/n * (t + 0.5 + n/4) * (k + 0.5))
	// with the Vorbis window w[t] = sin(pi/2 * sin(pi/n * (t+0.5))^2), which satisfies
	// Princen-Bradley, so the pair is exactly invertible in exact arithmetic.
	class mdct {
		size_t m_n;
		size_t m_half;
		std::vector<double> m_forward;   // [k * n + t], already windowed and scaled

	public:
		explicit mdct(size_t n) : m_n(n), m_half(n / 2), m_forward(n * n / 2) {
			const auto scale = 4.0 / static_cast<double>(n);
			std::vector<double> window(n);
			for (size_t t = 0; t < n; t++) {
				const auto s = std::sin(std::numbers::pi / static_cast<double>(n)
					* (static_cast<double>(t) + 0.5));
				window[t] = std::sin(std::numbers::pi / 2.0 * s * s);
			}
			for (size_t k = 0; k < m_half; k++) {
				for (size_t t = 0; t < n; t++) {
					const auto angle = 2.0 * std::numbers::pi / static_cast<double>(n)
						* (static_cast<double>(t) + 0.5 + static_cast<double>(n) / 4.0)
						* (static_cast<double>(k) + 0.5);
					m_forward[k * n + t] = std::cos(angle) * window[t] * scale;
				}
			}
		}

		[[nodiscard]] size_t half() const { return m_half; }

		// One block of `n` samples -> `n/2` coefficients, accumulated into `out`.
		void analyse(const double* block, double* out) const {
			for (size_t k = 0; k < m_half; k++) {
				const auto* row = &m_forward[k * m_n];
				double acc = 0.;
				for (size_t t = 0; t < m_n; t++)
					acc += block[t] * row[t];
				out[k] = acc;
			}
		}
	};

	// Where the decoder places each packet's window: block b spans padded [b*h, b*h+n), with
	// the signal offset h into the padding, so every output sample is covered by exactly two.
	size_t block_count(size_t frames, size_t half) {
		return (frames + half - 1) / half + 1;
	}

	// -------------------------------------------------------------- stream layout ---

	struct ladder {
		int Levels = 16;
		int Stages = 1;
		double Delta0 = 1.;

		[[nodiscard]] int ratio() const { return Levels - 2; }

		// Rung c holds values in [-(levels/2)*d_c, (levels/2 - 1)*d_c] and has to cover the
		// previous rung's rounding interval of +-d_{c-1}/2. The binding side is the positive
		// one, needing (levels/2 - 1)/ratio >= 1/2, i.e. ratio <= levels - 2. Using levels-1
		// looks like it should work and quietly clips the top of every rung.
		[[nodiscard]] std::vector<double> deltas() const {
			std::vector<double> out(static_cast<size_t>(Stages));
			for (int s = 0; s < Stages; s++)
				out[s] = Delta0 / std::pow(static_cast<double>(ratio()), s);
			return out;
		}
	};

	constexpr int LevelChoices[] = {8, 16, 32, 64, 128, 256};

	// (stages, delta0) for one rung width, or nothing if it needs more than the eight the
	// format allows.
	bool ladder_for(double peak, double stepTarget, int levels, int& stagesOut, double& delta0Out) {
		if (peak <= 0) {
			stagesOut = 1;
			delta0Out = (std::max)(stepTarget, 1e-30);
			return true;
		}
		const auto delta0 = peak / (levels / 2 - 1);
		const auto need = std::log(delta0 / stepTarget) / std::log(static_cast<double>(levels - 2));
		const auto stages = (std::max)(1, static_cast<int>(std::ceil(need)) + 1);
		if (stages > 8)
			return false;
		stagesOut = stages;
		delta0Out = delta0;
		return true;
	}

	// Cost every feasible ladder against the real partition peaks and keep the cheapest. The
	// estimate counts residue payload only; classification codewords are a couple of bits per
	// partition and do not move the ranking.
	ladder plan_ladder(const std::vector<double>& partitionPeaks, double stepTarget, size_t partitionSize) {
		const auto peak = partitionPeaks.empty() ? 0.
			: *std::ranges::max_element(partitionPeaks);
		ladder best;
		double bestBits = 0;
		bool have = false;
		for (const auto levels : LevelChoices) {
			int stages;
			double delta0;
			if (!ladder_for(peak, stepTarget, levels, stages, delta0))
				continue;
			const auto ratio = static_cast<double>(levels - 2);
			std::vector<double> span(static_cast<size_t>(stages));
			for (int c = 0; c < stages; c++)
				span[c] = (levels / 2 - 1) * (delta0 / std::pow(ratio, c));
			const auto finest = delta0 / std::pow(ratio, stages - 1);
			double bits = 0;
			for (const auto p : partitionPeaks) {
				int cls = 0;
				for (int c = 0; c < stages; c++)
					if (p <= span[c])
						cls = c;
				if (p <= finest / 2.0)
					cls = stages;
				bits += static_cast<double>(stages - cls);
			}
			bits *= static_cast<double>(partitionSize) * std::log2(static_cast<double>(levels));
			if (!have || bits < bestBits) {
				have = true;
				bestBits = bits;
				best = {levels, stages, delta0};
			}
		}
		if (!have) {
			const auto levels = LevelChoices[std::size(LevelChoices) - 1];
			return {levels, 8, peak / (levels / 2 - 1)};
		}
		return best;
	}

	// Coefficient grid fine enough that quantisation noise stays inside `lsbBudget` output
	// LSBs. A unit coefficient produces a unit-amplitude windowed cosine -- the combined
	// floor x IMDCT gain is exactly 1.0, measured -- so `half` independent coefficient errors
	// of at most d/2 add to about sqrt(half/3)*d RMS in the time domain.
	double coefficient_step(double lsbBudget, double fullScale, size_t half) {
		return lsbBudget / fullScale / std::sqrt(static_cast<double>(half) / 3.0);
	}

	// ------------------------------------------------------------------- headers ---

	void put_u32(std::vector<uint8_t>& out, uint32_t v) {
		out.push_back(static_cast<uint8_t>(v & 0xFF));
		out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
		out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
	}

	std::vector<uint8_t> id_header(size_t channels, size_t rate, int blockSizeLog2) {
		std::vector<uint8_t> out{1, 'v', 'o', 'r', 'b', 'i', 's'};
		put_u32(out, 0);                                   // vorbis version
		out.push_back(static_cast<uint8_t>(channels));
		put_u32(out, static_cast<uint32_t>(rate));
		put_u32(out, 0);                                   // bitrate maximum
		put_u32(out, 0);                                   // bitrate nominal
		put_u32(out, 0);                                   // bitrate minimum
		out.push_back(static_cast<uint8_t>((blockSizeLog2 << 4) | blockSizeLog2));
		out.push_back(1);                                  // framing
		return out;
	}

	std::vector<uint8_t> comment_header(const std::vector<std::string>& comments) {
		static constexpr char Vendor[] = "scdtool lossless vorbis";
		std::vector<uint8_t> out{3, 'v', 'o', 'r', 'b', 'i', 's'};
		put_u32(out, static_cast<uint32_t>(sizeof Vendor - 1));
		out.insert(out.end(), Vendor, Vendor + sizeof Vendor - 1);
		put_u32(out, static_cast<uint32_t>(comments.size()));
		for (const auto& c : comments) {
			put_u32(out, static_cast<uint32_t>(c.size()));
			out.insert(out.end(), c.begin(), c.end());
		}
		out.push_back(1);                                  // framing
		return out;
	}

	struct residue2 {
		int Begin = 0;
		int End = 0;
		int PartitionSize = 0;
		int Classifications = 0;
		int ClassBook = 0;
		std::vector<int> Cascade;                 // one 8-bit mask per classification
		std::vector<std::array<int, 8>> Books;    // [classification][stage] -> book index
	};

	std::vector<uint8_t> setup_header(const std::vector<codebook>& books, int floorMultiplier,
		int floorRangeBits, const residue2& residue, size_t channels) {
		bit_writer bw;
		bw.write(5, 8);
		for (const char c : std::string_view("vorbis"))
			bw.write(static_cast<uint8_t>(c), 8);

		bw.write(static_cast<uint32_t>(books.size() - 1), 8);
		for (const auto& b : books)
			b.pack(bw);

		bw.write(0, 6);                           // one placeholder time domain transform
		bw.write(0, 16);

		// floor1 with zero partitions: the curve is one constant from the normative table.
		bw.write(0, 6);                           // one floor
		bw.write(1, 16);                          // floor type 1
		bw.write(0, 5);                           // partitions
		bw.write(static_cast<uint32_t>(floorMultiplier - 1), 2);
		bw.write(static_cast<uint32_t>(floorRangeBits), 4);

		bw.write(0, 6);                           // one residue
		bw.write(2, 16);
		bw.write(static_cast<uint32_t>(residue.Begin), 24);
		bw.write(static_cast<uint32_t>(residue.End), 24);
		bw.write(static_cast<uint32_t>(residue.PartitionSize - 1), 24);
		bw.write(static_cast<uint32_t>(residue.Classifications - 1), 6);
		bw.write(static_cast<uint32_t>(residue.ClassBook), 8);
		for (const auto c : residue.Cascade) {
			bw.write(static_cast<uint32_t>(c & 7), 3);
			if (const auto high = c >> 3) {
				bw.write(1, 1);
				bw.write(static_cast<uint32_t>(high), 5);
			} else {
				bw.write(0, 1);
			}
		}
		for (int c = 0; c < residue.Classifications; c++)
			for (int stage = 0; stage < 8; stage++)
				if (residue.Cascade[c] & (1 << stage))
					bw.write(static_cast<uint32_t>(residue.Books[c][stage]), 8);

		bw.write(0, 6);                           // one mapping
		bw.write(0, 16);                          // mapping type 0
		bw.write(0, 1);                           // one submap
		bw.write(0, 1);                           // no channel coupling
		bw.write(0, 2);                           // reserved
		bw.write(0, 8);                           // submap unused
		bw.write(0, 8);                           // submap floor
		bw.write(0, 8);                           // submap residue

		bw.write(0, 6);                           // one mode
		bw.write(0, 1);                           // blockflag
		bw.write(0, 16);                          // window type
		bw.write(0, 16);                          // transform type
		bw.write(0, 8);                           // mapping

		bw.write(1, 1);                           // framing
		static_cast<void>(channels);
		return bw.take();
	}

	// ------------------------------------------------------------ libvorbis decode ---

	struct ogg_sync_guard {
		ogg_sync_state State{};
		ogg_sync_guard() { ogg_sync_init(&State); }
		~ogg_sync_guard() { ogg_sync_clear(&State); }
		ogg_sync_guard(const ogg_sync_guard&) = delete;
		ogg_sync_guard& operator=(const ogg_sync_guard&) = delete;
	};

}

namespace lossless_vorbis {

	std::vector<float> decode(const std::vector<uint8_t>& ogg, size_t& channelsOut) {
		ogg_sync_guard sync;
		ogg_stream_state stream{};
		vorbis_info info{};
		vorbis_comment comment{};
		vorbis_dsp_state dsp{};
		vorbis_block block{};
		bool streamOpen = false, dspOpen = false, blockOpen = false, infoOpen = false;

		const auto cleanup = [&] {
			if (blockOpen) vorbis_block_clear(&block);
			if (dspOpen) vorbis_dsp_clear(&dsp);
			if (infoOpen) { vorbis_comment_clear(&comment); vorbis_info_clear(&info); }
			if (streamOpen) ogg_stream_clear(&stream);
		};

		try {
			auto* buffer = ogg_sync_buffer(&sync.State, static_cast<long>(ogg.size()));
			std::memcpy(buffer, ogg.data(), ogg.size());
			ogg_sync_wrote(&sync.State, static_cast<long>(ogg.size()));

			ogg_page page;
			if (ogg_sync_pageout(&sync.State, &page) != 1)
				throw std::runtime_error("lossless: not an Ogg stream");
			ogg_stream_init(&stream, ogg_page_serialno(&page));
			streamOpen = true;
			vorbis_info_init(&info);
			vorbis_comment_init(&comment);
			infoOpen = true;
			if (ogg_stream_pagein(&stream, &page) < 0)
				throw std::runtime_error("lossless: first page rejected");

			ogg_packet packet;
			for (int got = 0; got < 3;) {
				const auto r = ogg_stream_packetout(&stream, &packet);
				if (r == 0) {
					if (ogg_sync_pageout(&sync.State, &page) != 1)
						throw std::runtime_error("lossless: stream ended inside the headers");
					ogg_stream_pagein(&stream, &page);
					continue;
				}
				if (r < 0 || vorbis_synthesis_headerin(&info, &comment, &packet) < 0)
					throw std::runtime_error("lossless: bad Vorbis header");
				got++;
			}

			channelsOut = static_cast<size_t>(info.channels);
			if (vorbis_synthesis_init(&dsp, &info))
				throw std::runtime_error("lossless: vorbis_synthesis_init failed");
			dspOpen = true;
			vorbis_block_init(&dsp, &block);
			blockOpen = true;

			std::vector<float> out;
			auto drain = [&] {
				float** pcm;
				int frames;
				while ((frames = vorbis_synthesis_pcmout(&dsp, &pcm)) > 0) {
					const auto base = out.size();
					out.resize(base + static_cast<size_t>(frames) * channelsOut);
					for (int i = 0; i < frames; i++)
						for (size_t c = 0; c < channelsOut; c++)
							out[base + static_cast<size_t>(i) * channelsOut + c] = pcm[c][i];
					vorbis_synthesis_read(&dsp, frames);
				}
			};

			for (bool eos = false; !eos;) {
				const auto r = ogg_sync_pageout(&sync.State, &page);
				if (r != 1)
					break;
				ogg_stream_pagein(&stream, &page);
				while (ogg_stream_packetout(&stream, &packet) == 1) {
					if (vorbis_synthesis(&block, &packet) == 0)
						vorbis_synthesis_blockin(&dsp, &block);
					drain();
				}
				if (ogg_page_eos(&page))
					eos = true;
			}
			drain();
			cleanup();
			return out;
		} catch (...) {
			cleanup();
			throw;
		}
	}

}

namespace {

	// ------------------------------------------------------------------- encoding ---

	struct plan {
		size_t Channels = 0;
		size_t BlockSize = 0;
		size_t Half = 0;
		size_t BinsPerPartition = 0;
		size_t PartitionSize = 0;      // bins * channels
		size_t PartitionsPerBlock = 0;
		ladder Ladder;
		int FloorY = 255;
		int FloorMultiplier = 1;

		[[nodiscard]] int classes() const { return Ladder.Stages + 1; }
	};

	// One audio packet. `blockIndex[stage]` is the rung-stage index array for this block;
	// `classRow` is the per-partition class.
	std::vector<uint8_t> encode_packet(const plan& p, const std::vector<std::vector<uint16_t>>& blockIndex,
		const std::vector<uint8_t>& classRow, const std::vector<codebook>& books, int floorYBits) {
		bit_writer bw;
		bw.write(0, 1);                          // audio packet
		// One mode means ilog(0) == 0 bits of mode number, and blockflag 0 means no window
		// flags follow. Then one floor per channel: nonzero, and both posts at the same y.
		for (size_t c = 0; c < p.Channels; c++) {
			bw.write(1, 1);
			bw.write(static_cast<uint32_t>(p.FloorY), floorYBits);
			bw.write(static_cast<uint32_t>(p.FloorY), floorYBits);
		}

		const auto& classBook = books[0];
		const auto classWords = static_cast<size_t>(classBook.Dim);
		const auto partitions = p.PartitionsPerBlock;

		// Pass 0 interleaves the classification codewords with the partition data: read a
		// classword covering the next `classWords` partitions, emit those partitions' data,
		// repeat. Later passes carry data only.
		for (int pass = 0; pass < p.Ladder.Stages; pass++) {
			size_t part = 0;
			while (part < partitions) {
				if (pass == 0) {
					uint32_t temp = 0;
					for (size_t i = 0; i < classWords; i++)
						temp = temp * static_cast<uint32_t>(p.classes())
							+ (part + i < partitions ? classRow[part + i] : 0);
					bw.write(classBook.Codewords[temp], classBook.Lengths[temp]);
				}
				for (size_t i = 0; i < classWords && part < partitions; i++, part++) {
					const auto rung = classRow[part] + pass;
					if (rung >= p.Ladder.Stages)
						continue;
					const auto& book = books[static_cast<size_t>(1 + rung)];
					const auto base = part * p.PartitionSize;
					const auto& idx = blockIndex[static_cast<size_t>(rung)];
					for (size_t k = 0; k < p.PartitionSize; k++) {
						const auto e = idx[base + k];
						bw.write(book.Codewords[e], book.Lengths[e]);
					}
				}
			}
		}
		return bw.take();
	}

}

namespace lossless_vorbis {

	result encode(const std::vector<int16_t>& samples, size_t channels, size_t rate,
		const options& opts) {
		if (!channels)
			throw std::runtime_error("lossless: zero channels");
		if (samples.size() % channels)
			throw std::runtime_error("lossless: sample count is not a whole number of frames");

		const auto frames = samples.size() / channels;
		const auto blockSize = opts.BlockSize;
		const auto half = blockSize / 2;
		if (half % opts.BinsPerPartition)
			throw std::runtime_error(std::format(
				"lossless: bins per partition {} must divide half-blocksize {}",
				opts.BinsPerPartition, half));

		// The scale the game's decoder multiplies by, measured in its Miles code: 32767.0f,
		// not 32768. Encoding against the wrong one puts every target a fraction of an LSB off
		// its aim point, which the encoder's own check cannot see because it uses the same
		// wrong scale on both sides -- it came out exact and decoded 3.9% of samples
		// differently under the two conversions.
		constexpr double FullScale = 32767.0;
		const auto [bias, tolerance] = [&] {
			switch (opts.Rounding) {
				case rounding::Round: return std::pair{0.0, 0.5};
				case rounding::Truncate: return std::pair{0.5, 0.5};
				default: return std::pair{0.25, 0.25};
			}
		}();

		plan p;
		p.Channels = channels;
		p.BlockSize = blockSize;
		p.Half = half;
		p.BinsPerPartition = opts.BinsPerPartition;
		p.PartitionSize = opts.BinsPerPartition * channels;
		p.PartitionsPerBlock = half / opts.BinsPerPartition;

		// Aim away from zero by `bias` so the decoder's conversion lands on the integer.
		const auto blocks = block_count(frames, half);
		std::vector<double> target(frames * channels);
		for (size_t i = 0; i < target.size(); i++) {
			const auto s = static_cast<double>(samples[i]);
			target[i] = (s + (s > 0 ? bias : s < 0 ? -bias : 0.0)) / FullScale;
		}

		const mdct transform(blockSize);

		// Coefficients per block, laid out as residue-2 interleaves them:
		// [bin0ch0, bin0ch1, ..., bin1ch0, ...]
		const auto width = half * channels;
		std::vector<double> coeffs(blocks * width);
		const auto analyse_into = [&](const std::vector<double>& signal, std::vector<double>& out, bool add) {
			std::vector<double> block(blockSize), spectrum(half);
			for (size_t b = 0; b < blocks; b++) {
				for (size_t c = 0; c < channels; c++) {
					// The signal sits `half` into the padding, so block b starts at b*half - half.
					const auto start = static_cast<ptrdiff_t>(b * half) - static_cast<ptrdiff_t>(half);
					for (size_t t = 0; t < blockSize; t++) {
						const auto idx = start + static_cast<ptrdiff_t>(t);
						block[t] = idx < 0 || static_cast<size_t>(idx) >= frames
							? 0.0 : signal[static_cast<size_t>(idx) * channels + c];
					}
					transform.analyse(block.data(), spectrum.data());
					for (size_t k = 0; k < half; k++) {
						auto& slot = out[b * width + k * channels + c];
						if (add)
							slot += spectrum[k];
						else
							slot = spectrum[k];
					}
				}
			}
		};
		analyse_into(target, coeffs, false);

		const auto step = coefficient_step(opts.LsbBudget, FullScale, half);

		result best;
		result previous;
		bool havePrevious = false;
		auto budget = opts.LsbBudget;

		for (size_t attempt = 1; attempt <= (std::max<size_t>)(1, opts.MaxAttempts); attempt++) {
			const auto attemptStep = coefficient_step(budget, FullScale, half);

			std::vector<double> partitionPeaks(blocks * p.PartitionsPerBlock);
			for (size_t b = 0; b < blocks; b++) {
				for (size_t q = 0; q < p.PartitionsPerBlock; q++) {
					double peak = 0;
					for (size_t k = 0; k < p.PartitionSize; k++)
						peak = (std::max)(peak, std::abs(coeffs[b * width + q * p.PartitionSize + k]));
					partitionPeaks[b * p.PartitionsPerBlock + q] = peak;
				}
			}
			p.Ladder = plan_ladder(partitionPeaks, attemptStep, p.PartitionSize);

			result attemptBest;
			auto working = coeffs;
			for (size_t iteration = 1; iteration <= (std::max<size_t>)(1, opts.MaxIterations); iteration++) {
				// --- classify -------------------------------------------------------------
				const auto deltas = p.Ladder.deltas();
				std::vector<uint8_t> classes(blocks * p.PartitionsPerBlock);
				for (size_t b = 0; b < blocks; b++) {
					for (size_t q = 0; q < p.PartitionsPerBlock; q++) {
						double peak = 0;
						for (size_t k = 0; k < p.PartitionSize; k++)
							peak = (std::max)(peak, std::abs(working[b * width + q * p.PartitionSize + k]));
						int cls = 0;
						for (int c = 0; c < p.Ladder.Stages; c++)
							if (peak <= (p.Ladder.Levels / 2 - 1) * deltas[static_cast<size_t>(c)])
								cls = c;
						if (peak <= deltas.back() / 2.0)
							cls = p.Ladder.Stages;
						classes[b * p.PartitionsPerBlock + q] = static_cast<uint8_t>(cls);
					}
				}

				// --- quantise, mirroring the decoder's float32 accumulate -----------------
				std::vector<codebook> valueBooks;
				for (const auto d : deltas)
					valueBooks.push_back(uniform_scalar_book(p.Ladder.Levels, d,
						-d * (p.Ladder.Levels / 2)));

				std::vector<std::vector<uint16_t>> index(static_cast<size_t>(p.Ladder.Stages),
					std::vector<uint16_t>(blocks * width, 0));
				std::vector<float> accumulated(blocks * width, 0.f);
				for (int s = 0; s < p.Ladder.Stages; s++) {
					const auto& values = valueBooks[static_cast<size_t>(s)].Values;
					const auto lo = static_cast<double>(values[0]);
					const auto stepSize = static_cast<double>(values[1]) - lo;
					for (size_t b = 0; b < blocks; b++) {
						for (size_t q = 0; q < p.PartitionsPerBlock; q++) {
							const auto active = classes[b * p.PartitionsPerBlock + q] <= s;
							if (!active)
								continue;
							for (size_t k = 0; k < p.PartitionSize; k++) {
								const auto at = b * width + q * p.PartitionSize + k;
								const auto residual = working[at] - static_cast<double>(accumulated[at]);
								auto j = static_cast<int64_t>(std::llround((residual - lo) / stepSize));
								j = std::clamp<int64_t>(j, 0, static_cast<int64_t>(values.size()) - 1);
								index[static_cast<size_t>(s)][at] = static_cast<uint16_t>(j);
								accumulated[at] = accumulated[at] + values[static_cast<size_t>(j)];
							}
						}
					}
				}

				// --- train the books on this pass's histograms ---------------------------
				std::vector<uint64_t> classCounts(static_cast<size_t>(p.classes()), 0);
				for (const auto c : classes)
					classCounts[c]++;
				auto classLengths = huffman_lengths(classCounts);
				std::vector<codebook> books;
				{
					codebook classBook;
					classBook.Dim = 1;
					classBook.Entries = static_cast<int>(classLengths.size());
					classBook.Lengths = classLengths;
					classBook.MapType = 0;
					classBook.finish();
					books.push_back(std::move(classBook));
				}
				for (int s = 0; s < p.Ladder.Stages; s++) {
					std::vector<uint64_t> counts(static_cast<size_t>(p.Ladder.Levels), 0);
					for (size_t b = 0; b < blocks; b++)
						for (size_t q = 0; q < p.PartitionsPerBlock; q++)
							if (classes[b * p.PartitionsPerBlock + q] <= s)
								for (size_t k = 0; k < p.PartitionSize; k++)
									counts[index[static_cast<size_t>(s)][b * width + q * p.PartitionSize + k]]++;
					auto lengths = huffman_lengths(counts);
					books.push_back(uniform_scalar_book(p.Ladder.Levels, deltas[static_cast<size_t>(s)],
						-deltas[static_cast<size_t>(s)] * (p.Ladder.Levels / 2), &lengths));
				}

				// --- structures ----------------------------------------------------------
				residue2 residue;
				residue.Begin = 0;
				residue.End = static_cast<int>(width);
				residue.PartitionSize = static_cast<int>(p.PartitionSize);
				residue.Classifications = p.classes();
				residue.ClassBook = 0;
				// Class c uses rungs c..stages-1 in cascade passes 0..stages-c-1; class
				// `stages` is the null class, costing nothing and reconstructing as zero.
				for (int c = 0; c < p.Ladder.Stages; c++) {
					residue.Cascade.push_back((1 << (p.Ladder.Stages - c)) - 1);
					std::array<int, 8> row{};
					row.fill(0);
					for (int s = 0; s < p.Ladder.Stages - c; s++)
						row[static_cast<size_t>(s)] = 1 + c + s;
					residue.Books.push_back(row);
				}
				residue.Cascade.push_back(0);
				residue.Books.push_back({});

				const auto floorYBits = ilog(static_cast<uint32_t>(
					(p.FloorMultiplier == 1 ? 256 : p.FloorMultiplier == 2 ? 128
						: p.FloorMultiplier == 3 ? 86 : 64) - 1));

				// --- pack into an Ogg stream ---------------------------------------------
				ogg_stream_state os{};
				ogg_stream_init(&os, 0x10C51E55);
				std::vector<uint8_t> out;
				const auto drain_pages = [&](bool force) {
					ogg_page og;
					while (force ? ogg_stream_flush(&os, &og) : ogg_stream_pageout(&os, &og)) {
						out.insert(out.end(), og.header, og.header + og.header_len);
						out.insert(out.end(), og.body, og.body + og.body_len);
					}
				};
				const auto put = [&](std::vector<uint8_t> data, int64_t granule, bool bos, bool eos) {
					ogg_packet op{};
					op.packet = data.data();
					op.bytes = static_cast<long>(data.size());
					op.b_o_s = bos;
					op.e_o_s = eos;
					op.granulepos = granule;
					op.packetno = 0;
					ogg_stream_packetin(&os, &op);
				};

				put(id_header(channels, rate, ilog(static_cast<uint32_t>(blockSize)) - 1), 0, true, false);
				drain_pages(true);
				put(comment_header(opts.Comments), 0, false, false);
				put(setup_header(books, p.FloorMultiplier, ilog(static_cast<uint32_t>(half)) - 1,
					residue, channels), 0, false, false);
				drain_pages(true);

				// libvorbis derives the front trim from the first audio page's granulepos and
				// the end trim from the last, and mis-handles both landing on one page, so at
				// least two audio pages are always emitted.
				const auto perPage = (std::max<size_t>)(1, (std::min<size_t>)(16, (blocks + 1) / 2));
				std::vector<std::vector<uint16_t>> blockIndex(static_cast<size_t>(p.Ladder.Stages));
				for (size_t b = 0; b < blocks; b++) {
					for (int s = 0; s < p.Ladder.Stages; s++)
						blockIndex[static_cast<size_t>(s)].assign(
							index[static_cast<size_t>(s)].begin() + static_cast<ptrdiff_t>(b * width),
							index[static_cast<size_t>(s)].begin() + static_cast<ptrdiff_t>((b + 1) * width));
					std::vector<uint8_t> classRow(
						classes.begin() + static_cast<ptrdiff_t>(b * p.PartitionsPerBlock),
						classes.begin() + static_cast<ptrdiff_t>((b + 1) * p.PartitionsPerBlock));
					const auto last = b + 1 == blocks;
					put(encode_packet(p, blockIndex, classRow, books, floorYBits),
						static_cast<int64_t>(last ? frames : b * half), false, last);
					if (last || (b + 1) % perPage == 0)
						drain_pages(true);
					else
						drain_pages(false);
				}
				drain_pages(true);
				ogg_stream_clear(&os);

				// --- measure against the real decoder ------------------------------------
				size_t decodedChannels = 0;
				const auto pcm = decode(out, decodedChannels);
				if (decodedChannels != channels || pcm.size() / channels != frames)
					throw std::runtime_error(std::format(
						"lossless: decoder returned {} frames x {} ch, wanted {} x {}",
						decodedChannels ? pcm.size() / decodedChannels : 0, decodedChannels,
						frames, channels));

				size_t mismatches = 0;
				double worst = 0;
				std::vector<double> error(frames * channels);
				for (size_t i = 0; i < error.size(); i++) {
					const auto got = static_cast<double>(pcm[i]);
					error[i] = target[i] - got;
					worst = (std::max)(worst, std::abs(error[i]));
					const auto scaled = got * FullScale;
					const auto want = static_cast<double>(samples[i]);
					auto ok = true;
					if (opts.Rounding != rounding::Truncate)
						ok = ok && std::clamp(std::nearbyint(scaled), -32768.0, 32767.0) == want;
					if (opts.Rounding != rounding::Round)
						ok = ok && std::clamp(std::trunc(scaled), -32768.0, 32767.0) == want;
					if (!ok)
						mismatches++;
				}

				result candidate;
				candidate.Ogg = out;
				candidate.Exact = mismatches == 0;
				candidate.Mismatches = mismatches;
				candidate.WorstErrorLsb = worst * FullScale;
				candidate.BitsPerSample = static_cast<double>(out.size()) * 8.
					/ static_cast<double>((std::max<size_t>)(1, frames * channels));
				candidate.Iterations = iteration;
				candidate.Attempts = attempt;
				candidate.Levels = static_cast<size_t>(p.Ladder.Levels);
				candidate.Stages = static_cast<size_t>(p.Ladder.Stages);
				if (attemptBest.Ogg.empty() || mismatches < attemptBest.Mismatches)
					attemptBest = candidate;
				if (!mismatches)
					break;

				// Newton step: the residual pushed back through the analysis transform is
				// exactly the coefficient correction that cancels it.
				analyse_into(error, working, true);
			}

			best = attemptBest;
			if (opts.MinMargin <= 0 || best.Exact) {
				if (opts.MinMargin <= 0 || best.WorstErrorLsb * opts.MinMargin <= tolerance)
					return best;
			}
			if (best.WorstErrorLsb <= 0)
				return best;
			// A finer grid that stops reducing mismatches has hit the decoder's own float32
			// floor, not our quantisation; spending more bits on it buys nothing.
			if (havePrevious && best.Mismatches > 0 && best.Mismatches >= previous.Mismatches)
				return previous;
			previous = best;
			havePrevious = true;
			const auto shrink = (std::max)(0.1, (tolerance / opts.MinMargin) / best.WorstErrorLsb);
			if (shrink >= 0.95)
				return best;
			budget *= shrink;
		}
		return best;
	}

}
