#include "pch.h"
#include "join_detector.h"

#include "win32_process.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <complex>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <set>

namespace {
	using cplx = std::complex<double>;
	constexpr auto Nan = std::numeric_limits<double>::quiet_NaN();
	constexpr auto Sr = static_cast<double>(DetectorRateHz);

	// ---- tunables, as calibrated in scratch/_detector/calibrate.py ----------------------

	// The weight grid: one estimate every 0.25 s, each fitted over the 0.5 s centred on it.
	// A crossfade is rarely shorter than 0.5 s and the game's own are 1-16 s, so a 0.25 s step
	// resolves where one starts; 0.5 s of audio is what a 12-band fit needs to be stable.
	constexpr double Win = 0.25;
	constexpr double FitLen = 0.5;
	// Each join is judged over 20 s either side, and joins closer than 40 s share one region
	// -- ec5's layered re-entries sit 2-13 s after the join they patch, and judged apart each
	// would normalise against the other's lead-in.
	constexpr double RegionPad = 20.;
	constexpr double ClusterGap = 40.;
	// Lags: a 2 s window every second, searched +-50 ms around where the preset puts the copy.
	// Wide enough for the 1-13 ms offsets and 7-13 ppm drifts the join campaign found, narrow
	// enough that a repetitive loop does not offer a second peak a bar away.
	constexpr double LagWin = 2.;
	constexpr double LagStep = 1.;
	constexpr double LagMax = 0.050;
	// A lag window is used when its normalised peak reaches 0.45: over the calibration cases
	// a copy that really plays there peaks 0.6-0.95, and a window where it is faded out or
	// buried under the other copy scatters under 0.35 at lags anywhere in the range.
	constexpr double LagRhoMin = 0.45;
	constexpr double LagOutlierSamples = 22.;   // 0.5 ms from the line: a window that locked elsewhere
	constexpr double LagMinSpan = 8.;           // s of accepted windows before a drift slope is fitted
	constexpr double MaxDrift = 60e-6;          // 60 ppm; the worst real recording ran 13
	// Truncated eigen-solve: eigenvalues of the unit-diagonal Gram under 0.15^2 of the largest
	// are dropped. Two copies of one recording a few seconds apart are nearly collinear over a
	// sustained chord, and an untruncated solve splits their weight +3/-2 where the truth is
	// 0.5/0.5; the minimum-norm split at least lands on the even one.
	constexpr double TsvdRel = 0.15;
	// A per-copy weight is only reported where it is identifiable -- 1/sqrt((G^-1)_kk) at
	// least 0.3, the sine of the copy's angle to the span of the others -- and where the copy
	// at its fitted scale is within 30 dB of the game (0.03 in amplitude).
	constexpr double IdentMin = 0.30;
	constexpr double AudibleMin = 0.03;
	constexpr double Edge = 4.;                 // s of lead-in (else lead-out) that set a recording's scale
	constexpr double DenFloor = 0.01;           // windows 20 dB under the region's median game energy do not count
	constexpr size_t NBands = 12;
	constexpr double BandLo = 80., BandHi = 16000.;
	constexpr double BandFloor = 1e-3;          // bands 30 dB under the window's loudest carry no say

	// Flags. Calibrated on table_final.txt, prototype and this port alike:
	//   shape: every clean build peaks at 0.09 or less (ban09ex4 0.09, raid22 and ec3 0.07,
	//          raid26 and tanoshii1 0.06); the weakest wrong one is wedding's M5 sum at 0.15
	//          and raid26's pre-campaign early fade at 0.19.
	//   level: clean builds stay within 2.7 dB of their own region median (raid26 +2.7,
	//          ban09ex4 +2.6, ec3 -2.6); the smallest wrong step is ec3's 523 s join at
	//          -4.5 dB and the M5 sums at +4.6 to +4.9.
	constexpr double TPeak = 0.14;
	constexpr double TLevel = 3.5;
	// Relative lag between copies, build against game: under 2 ms is inside what a 2 s
	// correlation window resolves on a legato passage (clean builds read 0.0-1.5 ms, with
	// raid24 and ec3 at 2.2-2.3 ms on lag alone); the pre-campaign offsets were 2.9-9.7 ms.
	constexpr double TRelLag = 2.;
	constexpr double TWinErr = 0.10;            // "wrong" seconds: windows past this, reported only

	// Filters that shape a copy's level rather than its waveform: exactly the envelopes being
	// measured, so they are stripped from the render the copy is read from. What is left --
	// allpass, eq, aresample/asetrate, adelay -- changes the waveform and has to stay, or the
	// copy would not correlate with what the build actually played.
	const std::set<std::string> GainOps{"volume", "afade", "aeval", "alimiter", "anull", "apad", "amix",
		"asplit", "dynaudnorm", "loudnorm", "acompressor", "agate", "compand"};

	// ---- FFT --------------------------------------------------------------------------

	// Radix-2 with the twiddles and the bit reversal precomputed per size. fft.h recomputes
	// both on every call, which is fine for its callers' one transform per frame and is most
	// of the cost at the rate this runs them: a region takes tens of thousands.
	class fft_plan {
		size_t m_n;
		std::vector<uint32_t> m_rev;
		std::vector<cplx> m_tw;

	public:
		explicit fft_plan(size_t n) : m_n(n), m_rev(n), m_tw(n / 2) {
			size_t bits = 0;
			while ((size_t{1} << bits) < n)
				++bits;
			for (size_t i = 0; i < n; ++i) {
				size_t r = 0;
				for (size_t b = 0; b < bits; ++b)
					if (i & (size_t{1} << b))
						r |= size_t{1} << (bits - 1 - b);
				m_rev[i] = static_cast<uint32_t>(r);
			}
			for (size_t k = 0; k < n / 2; ++k)
				m_tw[k] = std::polar(1., -2. * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n));
		}

		// Unscaled either way.
		void run(cplx* a, bool inverse) const {
			for (size_t i = 0; i < m_n; ++i)
				if (i < m_rev[i])
					std::swap(a[i], a[m_rev[i]]);
			for (size_t len = 2; len <= m_n; len <<= 1) {
				const auto half = len / 2;
				const auto step = m_n / len;
				for (size_t i = 0; i < m_n; i += len) {
					for (size_t j = 0; j < half; ++j) {
						const auto w = inverse ? std::conj(m_tw[j * step]) : m_tw[j * step];
						const auto u = a[i + j];
						const auto v = a[i + j + half] * w;
						a[i + j] = u + v;
						a[i + j + half] = u - v;
					}
				}
			}
		}
	};

	const fft_plan& plan_for(size_t n) {
		thread_local std::map<size_t, std::unique_ptr<fft_plan>> plans;
		auto& p = plans[n];
		if (!p)
			p = std::make_unique<fft_plan>(n);
		return *p;
	}

	// numpy's rfft/irfft for a power-of-two length, through a complex transform of half the
	// length. irfft drops the imaginary part of the DC and Nyquist bins, as numpy's does --
	// which matters here: the fractional-lag read multiplies the Nyquist bin by a phase.
	class real_fft {
		size_t m_n;
		std::vector<cplx> m_tw;   // e^{-2 pi i k / n}, k < n/2
		mutable std::vector<cplx> m_z;

	public:
		explicit real_fft(size_t n) : m_n(n), m_tw(n / 2), m_z(n / 2) {
			for (size_t k = 0; k < n / 2; ++k)
				m_tw[k] = std::polar(1., -2. * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n));
		}

		[[nodiscard]] size_t size() const { return m_n; }
		[[nodiscard]] size_t bins() const { return m_n / 2 + 1; }

		// x[i * stride] for i < len, zero past it. out: bins() values.
		void forward(const double* x, size_t len, size_t stride, cplx* out) const {
			const auto h = m_n / 2;
			for (size_t m = 0; m < h; ++m) {
				const auto i0 = 2 * m, i1 = 2 * m + 1;
				m_z[m] = cplx(i0 < len ? x[i0 * stride] : 0., i1 < len ? x[i1 * stride] : 0.);
			}
			plan_for(h).run(m_z.data(), false);
			for (size_t k = 0; k <= h; ++k) {
				const auto zk = m_z[k % h];
				const auto zc = std::conj(m_z[(h - k) % h]);
				const auto e = (zk + zc) * 0.5;
				const auto o = (zk - zc) * cplx(0., -0.5);
				out[k] = k == h ? e - o : e + m_tw[k] * o;
			}
		}

		// out: n values, scaled by 1/n.
		void inverse(const cplx* in, double* out) const {
			const auto h = m_n / 2;
			const auto x0 = cplx(in[0].real(), 0.);
			const auto xh = cplx(in[h].real(), 0.);
			for (size_t k = 0; k < h; ++k) {
				const auto xk = k == 0 ? x0 : in[k];
				const auto xc = std::conj(h - k == h ? xh : in[h - k]);
				const auto e = (xk + xc) * 0.5;
				const auto o = (xk - xc) * 0.5 * std::conj(m_tw[k]);
				m_z[k] = e + cplx(0., 1.) * o;
			}
			plan_for(h).run(m_z.data(), true);
			const auto scale = 1. / static_cast<double>(h);
			for (size_t m = 0; m < h; ++m) {
				out[2 * m] = m_z[m].real() * scale;
				out[2 * m + 1] = m_z[m].imag() * scale;
			}
		}
	};

	const real_fft& rfft_for(size_t n) {
		thread_local std::map<size_t, std::unique_ptr<real_fft>> plans;
		auto& p = plans[n];
		if (!p)
			p = std::make_unique<real_fft>(n);
		return *p;
	}

	size_t next_pow2(size_t n) {
		size_t p = 1;
		while (p < n)
			p <<= 1;
		return p;
	}

	// ---- small numerics ---------------------------------------------------------------

	// numpy's median: the mean of the two middle values of an even count.
	double median(std::vector<double> v) {
		if (v.empty())
			return Nan;
		std::ranges::sort(v);
		const auto n = v.size();
		return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
	}

	// Python's round(): half to even, which std::nearbyint gives in the default rounding mode.
	double round_to(double x, double scale) { return std::nearbyint(x * scale) / scale; }

	// Cyclic Jacobi on a small symmetric matrix: eigenvalues ascending, eigenvectors as columns.
	void eigh(std::vector<double> a, size_t n, std::vector<double>& lam, std::vector<double>& v) {
		v.assign(n * n, 0.);
		for (size_t i = 0; i < n; ++i)
			v[i * n + i] = 1.;
		for (int sweep = 0; sweep < 100; ++sweep) {
			auto off = 0., norm = 0.;
			for (size_t i = 0; i < n; ++i)
				for (size_t j = 0; j < n; ++j) {
					norm += a[i * n + j] * a[i * n + j];
					if (i != j)
						off += a[i * n + j] * a[i * n + j];
				}
			if (off <= 1e-30 * norm || off == 0.)
				break;
			for (size_t p = 0; p < n; ++p) {
				for (size_t q = p + 1; q < n; ++q) {
					const auto apq = a[p * n + q];
					if (std::abs(apq) < 1e-300)
						continue;
					const auto theta = (a[q * n + q] - a[p * n + p]) / (2. * apq);
					const auto t = (theta >= 0. ? 1. : -1.) / (std::abs(theta) + std::sqrt(theta * theta + 1.));
					const auto c = 1. / std::sqrt(t * t + 1.);
					const auto s = t * c;
					for (size_t k = 0; k < n; ++k) {
						const auto akp = a[k * n + p], akq = a[k * n + q];
						a[k * n + p] = c * akp - s * akq;
						a[k * n + q] = s * akp + c * akq;
					}
					for (size_t k = 0; k < n; ++k) {
						const auto apk = a[p * n + k], aqk = a[q * n + k];
						a[p * n + k] = c * apk - s * aqk;
						a[q * n + k] = s * apk + c * aqk;
					}
					for (size_t k = 0; k < n; ++k) {
						const auto vkp = v[k * n + p], vkq = v[k * n + q];
						v[k * n + p] = c * vkp - s * vkq;
						v[k * n + q] = s * vkp + c * vkq;
					}
				}
			}
		}
		std::vector<size_t> order(n);
		std::iota(order.begin(), order.end(), size_t{0});
		std::ranges::sort(order, [&](size_t x, size_t y) { return a[x * n + x] < a[y * n + y]; });
		lam.resize(n);
		std::vector<double> sorted(n * n);
		for (size_t c = 0; c < n; ++c) {
			lam[c] = a[order[c] * n + order[c]];
			for (size_t r = 0; r < n; ++r)
				sorted[r * n + c] = v[r * n + order[c]];
		}
		v = std::move(sorted);
	}

	// The truncated solve on a unit-diagonal Gram, and each column's identifiability.
	void tsvd_solve(const std::vector<double>& g, const std::vector<double>& rhs, size_t n,
		std::vector<double>& w, std::vector<double>& ident) {
		std::vector<double> lam, v;
		eigh(g, n, lam, v);
		const auto top = *std::ranges::max_element(lam);
		w.assign(n, 0.);
		for (size_t c = 0; c < n; ++c) {
			if (!(lam[c] >= TsvdRel * TsvdRel * top))
				continue;
			auto proj = 0.;
			for (size_t r = 0; r < n; ++r)
				proj += v[r * n + c] * rhs[r];
			proj /= lam[c];
			for (size_t r = 0; r < n; ++r)
				w[r] += v[r * n + c] * proj;
		}
		// diag((G + 1e-9 I)^-1) from the same decomposition.
		ident.assign(n, 0.);
		for (size_t k = 0; k < n; ++k) {
			auto d = 0.;
			for (size_t c = 0; c < n; ++c)
				d += v[k * n + c] * v[k * n + c] / (std::max)(lam[c] + 1e-9, 1e-300);
			ident[k] = (std::min)(1. / std::sqrt((std::max)(d, 1e-12)), 1.);
		}
	}

	// ---- filter parsing ---------------------------------------------------------------

	std::string trim(std::string_view s) {
		const auto b = s.find_first_not_of(" \t\r\n");
		if (b == std::string_view::npos)
			return {};
		const auto e = s.find_last_not_of(" \t\r\n");
		return std::string(s.substr(b, e - b + 1));
	}

	// Split on `sep` outside quotes and parentheses.
	std::vector<std::string> split_top(std::string_view s, char sep) {
		std::vector<std::string> out;
		std::string cur;
		char quote = 0;
		int depth = 0;
		for (const auto ch : s) {
			if (quote) {
				cur += ch;
				if (ch == quote)
					quote = 0;
				continue;
			}
			if (ch == '\'' || ch == '"')
				quote = ch;
			else if (ch == '(')
				++depth;
			else if (ch == ')')
				--depth;
			if (ch == sep && depth == 0) {
				out.push_back(std::move(cur));
				cur.clear();
			} else {
				cur += ch;
			}
		}
		out.push_back(std::move(cur));
		return out;
	}

	struct filter_op {
		std::string Op, Args, Token;
	};

	struct filter_chain {
		std::vector<std::string> Ins, Outs;
		std::vector<filter_op> Ops;
	};

	// '[a]x=1,y[b]' -> in labels, ops, out labels.
	filter_chain parse_chain(std::string_view text) {
		filter_chain res;
		auto chain = trim(text);
		while (!chain.empty() && chain.front() == '[') {
			const auto j = chain.find(']');
			if (j == std::string::npos)
				throw std::runtime_error("unbalanced label");
			res.Ins.push_back(chain.substr(1, j - 1));
			chain = trim(chain.substr(j + 1));
		}
		auto toks = split_top(chain, ',');
		auto last = trim(toks.back());
		while (!last.empty() && last.back() == ']' && last.find('[') != std::string::npos) {
			const auto j = last.rfind('[');
			res.Outs.insert(res.Outs.begin(), last.substr(j + 1, last.size() - j - 2));
			last = trim(last.substr(0, j));
		}
		toks.back() = last;
		for (const auto& raw : toks) {
			const auto t = trim(raw);
			if (t.empty())
				continue;
			const auto eq = t.find('=');
			res.Ops.push_back({trim(t.substr(0, eq)), eq == std::string::npos ? std::string{} : t.substr(eq + 1), t});
		}
		return res;
	}

	double adelay_seconds(const std::string& args, double nativeRate) {
		auto v = args.substr(0, args.find(':'));
		v = trim(v.substr(0, v.find('|')));
		if (v.starts_with("delays="))
			v = v.substr(7);
		if (v.ends_with("S"))
			return std::stod(v.substr(0, v.size() - 1)) / nativeRate;
		if (v.ends_with("ms"))
			return std::stod(v.substr(0, v.size() - 2)) / 1000.;
		if (v.ends_with("s"))
			return std::stod(v.substr(0, v.size() - 1));
		return std::stod(v) / 1000.;
	}

	// aresample=N,asetrate=M speeds the stream up by M/N: output time = input time * N/M.
	double time_scale(const std::vector<filter_op>& ops) {
		auto sc = 1.;
		std::optional<double> lastRate;
		for (const auto& op : ops) {
			if (op.Op == "aresample") {
				try {
					lastRate = std::stod(op.Args.substr(0, op.Args.find(':')));
				} catch (const std::exception&) {
					lastRate.reset();
				}
			} else if (op.Op == "asetrate" && lastRate && *lastRate != 0.) {
				sc *= *lastRate / std::stod(op.Args.substr(0, op.Args.find(':')));
				lastRate.reset();
			}
		}
		return sc;
	}

	std::string join_tokens(const std::vector<std::string>& tokens) {
		std::string s;
		for (const auto& t : tokens) {
			if (!s.empty())
				s += ',';
			s += t;
		}
		return s;
	}

	struct filter_copies {
		std::string Render;
		std::vector<std::pair<double, std::string>> Delays;   // (seconds in the rendered timeline, branch label)
		std::vector<std::string> Notes;
		std::string Unsupported;
	};

	// The render a source's filter reduces to, and one delay per asplit branch.
	filter_copies copies_from_filter(const std::string& filter, double nativeRate) {
		filter_copies res;
		if (filter.empty()) {
			res.Delays.emplace_back(0., "");
			return res;
		}
		const auto chains = split_top(filter, ';');
		if (chains.size() == 1 && filter.find("asplit") == std::string::npos) {
			const auto parsed = parse_chain(chains.front());
			std::vector<std::string> keep;
			std::set<std::string> dropped;
			for (const auto& op : parsed.Ops)
				(GainOps.contains(op.Op) ? (void)dropped.insert(op.Op) : keep.push_back(op.Token));
			if (!dropped.empty()) {
				std::string list;
				for (const auto& d : dropped)
					list += (list.empty() ? "" : "/") + d;
				res.Notes.push_back("stripped " + list);
			}
			res.Render = join_tokens(keep);
			res.Delays.emplace_back(0., "");
			return res;
		}
		if (filter.find("asplit") == std::string::npos) {
			res.Unsupported = "a multi-chain filter with no asplit";
			return res;
		}

		std::vector<filter_chain> parsed;
		for (const auto& c : chains)
			parsed.push_back(parse_chain(c));
		std::vector<std::string> pre;
		std::vector<filter_op> post;
		std::vector<std::string> splitOuts;
		for (const auto& chain : parsed) {
			for (size_t i = 0; i < chain.Ops.size(); ++i) {
				if (chain.Ops[i].Op == "asplit") {
					pre.clear();
					for (size_t k = 0; k < i; ++k)
						if (!GainOps.contains(chain.Ops[k].Op))
							pre.push_back(chain.Ops[k].Token);
					splitOuts = chain.Outs;
					break;
				}
			}
			for (size_t i = 0; i < chain.Ops.size(); ++i) {
				if (chain.Ops[i].Op == "amix") {
					post.assign(chain.Ops.begin() + static_cast<ptrdiff_t>(i) + 1, chain.Ops.end());
					break;
				}
			}
		}
		// Follow each branch through its chains, adding up its adelays.
		std::vector<std::pair<std::string, double>> delays;
		std::map<std::string, std::string> rootOf;
		for (const auto& b : splitOuts) {
			delays.emplace_back(b, 0.);
			rootOf.emplace(b, b);
		}
		const auto delay_of = [&](const std::string& root) -> double& {
			for (auto& [b, d] : delays)
				if (b == root)
					return d;
			throw std::runtime_error("unknown branch");
		};
		for (auto changed = true; changed;) {
			changed = false;
			for (const auto& chain : parsed) {
				if (chain.Ins.size() != 1 || !rootOf.contains(chain.Ins.front())
					|| chain.Outs.empty() || rootOf.contains(chain.Outs.front()))
					continue;
				const auto root = rootOf.at(chain.Ins.front());
				auto d = delay_of(root);
				for (const auto& op : chain.Ops) {
					if (op.Op == "adelay") {
						d += adelay_seconds(op.Args, nativeRate);
					} else if (!GainOps.contains(op.Op)) {
						// A waveform filter on one branch only makes that branch a different
						// waveform from the render every copy is read from, and the fit would
						// then be judging the filter.
						res.Unsupported = std::format("\"{}\" inside an asplit branch", op.Op);
						return res;
					}
				}
				delay_of(root) = d;
				rootOf.emplace(chain.Outs.front(), root);
				changed = true;
			}
		}
		const auto sc = time_scale(post);
		auto render = pre;
		for (const auto& op : post)
			if (!GainOps.contains(op.Op))
				render.push_back(op.Token);
		res.Render = join_tokens(render);
		std::string list;
		for (const auto& [b, d] : delays) {
			res.Delays.emplace_back(d * sc, b);
			list += std::format("{}{:.4f}", list.empty() ? "" : ", ", d * sc);
		}
		res.Notes.push_back(std::format("asplit x{} delays {}", delays.size(), list));
		return res;
	}

	std::string lower_utf8(const std::filesystem::path& p) {
		auto w = p.wstring();
		std::ranges::transform(w, w.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
		return xivres::util::unicode::convert<std::string>(w);
	}

	// ---- signal side ------------------------------------------------------------------

	// Rows [n0, n0 + n) of an interleaved buffer, zero outside it, as doubles.
	void slice_rows(const detector_audio& a, ptrdiff_t n0, size_t n, std::vector<double>& out) {
		out.assign(n * a.Channels, 0.);
		const auto s = (std::max<ptrdiff_t>)(n0, 0);
		const auto e = (std::min<ptrdiff_t>)(n0 + static_cast<ptrdiff_t>(n), static_cast<ptrdiff_t>(a.Frames));
		for (auto i = s; i < e; ++i)
			for (size_t c = 0; c < a.Channels; ++c)
				out[static_cast<size_t>(i - n0) * a.Channels + c] = a.Data[static_cast<size_t>(i) * a.Channels + c];
	}

	// The game's or the build's audio in the analysis layout: at most two channels, more is
	// folded to mono.
	void fold(const std::vector<double>& x, size_t xc, size_t n, size_t nch, std::vector<double>& out) {
		if (nch == xc && nch <= 2) {
			out = x;
			return;
		}
		out.assign(n * nch, 0.);
		if (nch == 1 || xc > 2) {
			for (size_t i = 0; i < n; ++i) {
				auto s = 0.;
				for (size_t c = 0; c < xc; ++c)
					s += x[i * xc + c];
				out[i * nch] = s / static_cast<double>(xc);   // nch is 1 here
			}
			return;
		}
		for (size_t i = 0; i < n; ++i)
			for (size_t c = 0; c < nch; ++c)
				out[i * nch + c] = xc == 1 ? x[i] : x[i * xc + c];
	}

	void mono_of(const std::vector<double>& x, size_t xc, size_t n, std::vector<double>& out) {
		out.resize(n);
		for (size_t i = 0; i < n; ++i) {
			auto s = 0.;
			for (size_t c = 0; c < xc; ++c)
				s += x[i * xc + c];
			out[i] = s / static_cast<double>(xc);
		}
	}

	// A copy's audio mapped to the analysed signal's channel layout.
	class copy_signal {
	public:
		const detector_copy* Cp;
		detector_audio R;
		size_t OutCh;
		double Dur;
		size_t Sel0 = 0, Sel1 = 1;

		copy_signal(const detector_copy_audio& ca, size_t outCh)
			: Cp(ca.Copy), R(ca.Audio), OutCh(outCh), Dur(static_cast<double>(ca.Audio.Frames) / Sr) {
			const auto rn = R.Channels;
			std::vector<size_t> m = Cp->ChannelMap.size() >= 2 ? Cp->ChannelMap : std::vector<size_t>{0, 1};
			Sel0 = (std::min)(m[0], rn - 1);
			Sel1 = (std::min)(m[1], rn - 1);
		}

		// Fraction of [t0, t1] (game time) where the copy has recording material.
		[[nodiscard]] double available(double t0, double t1) const {
			const auto a = Cp->C + t0, b = Cp->C + t1;
			return (std::max)(0., (std::min)(b, Dur) - (std::max)(a, 0.)) / (std::max)(b - a, 1e-9);
		}

		// Mapped rows [n0, n0 + n), zero outside the recording.
		void mapped_rows(ptrdiff_t n0, size_t n, std::vector<double>& out) const {
			out.assign(n * OutCh, 0.);
			const auto rn = R.Channels;
			const auto s = (std::max<ptrdiff_t>)(n0, 0);
			const auto e = (std::min<ptrdiff_t>)(n0 + static_cast<ptrdiff_t>(n), static_cast<ptrdiff_t>(R.Frames));
			for (auto i = s; i < e; ++i) {
				const auto* row = R.Data + static_cast<size_t>(i) * rn;
				auto* o = out.data() + static_cast<size_t>(i - n0) * OutCh;
				if (OutCh == 2) {
					o[0] = row[Sel0];
					o[1] = row[Sel1];
				} else {
					auto sum = 0.;
					for (size_t c = 0; c < rn; ++c)
						sum += row[c];
					o[0] = sum / static_cast<double>(rn);
				}
			}
		}

		// Mean of every recording channel, as the lag search reads it.
		void mono_rows(ptrdiff_t n0, size_t n, std::vector<double>& out) const {
			out.assign(n, 0.);
			const auto rn = R.Channels;
			const auto s = (std::max<ptrdiff_t>)(n0, 0);
			const auto e = (std::min<ptrdiff_t>)(n0 + static_cast<ptrdiff_t>(n), static_cast<ptrdiff_t>(R.Frames));
			for (auto i = s; i < e; ++i) {
				auto sum = 0.;
				for (size_t c = 0; c < rn; ++c)
					sum += R.Data[static_cast<size_t>(i) * rn + c];
				out[static_cast<size_t>(i - n0)] = sum / static_cast<double>(rn);
			}
		}

		// n samples at the fractional rendered-recording position `pos` (44.1 kHz samples),
		// shifted by an FFT phase ramp over a 256-sample margin either side. The prototype
		// transforms exactly n + 512 samples; this zero-pads them to the next power of two,
		// which moves the periodic extension the phase ramp wraps around but not the interior:
		// the ringing from either edge is down ~60 dB by the time it crosses the margin.
		void read(double pos, size_t n, std::vector<double>& out) const {
			constexpr size_t Margin = 256;
			const auto n0 = static_cast<ptrdiff_t>(std::floor(pos));
			const auto fr = pos - static_cast<double>(n0);
			thread_local std::vector<double> seg, col, back;
			thread_local std::vector<cplx> spec;
			mapped_rows(n0 - static_cast<ptrdiff_t>(Margin), n + 2 * Margin, seg);
			out.assign(n * OutCh, 0.);
			if (std::abs(fr) <= 1e-4) {
				for (size_t i = 0; i < n; ++i)
					for (size_t c = 0; c < OutCh; ++c)
						out[i * OutCh + c] = seg[(i + Margin) * OutCh + c];
				return;
			}
			const auto len = n + 2 * Margin;
			const auto& rf = rfft_for(next_pow2(len));
			const auto L = rf.size();
			spec.resize(rf.bins());
			back.resize(L);
			for (size_t c = 0; c < OutCh; ++c) {
				rf.forward(seg.data() + c, len, OutCh, spec.data());
				for (size_t k = 0; k < spec.size(); ++k)
					spec[k] *= std::polar(1., 2. * std::numbers::pi * static_cast<double>(k) / static_cast<double>(L) * fr);
				rf.inverse(spec.data(), back.data());
				for (size_t i = 0; i < n; ++i)
					out[i * OutCh + c] = back[i + Margin];
			}
		}
	};

	struct lag_fit {
		double A = 0., B = 0., TRef = 0.;
		size_t N = 0;
		double Rho = 0.;
		[[nodiscard]] double at(double t) const { return A + B * (t - TRef); }
	};

	// g (n), r (n + 2M) -> (lag in samples relative to M, normalised peak).
	std::pair<double, double> xcorr_peak(const std::vector<double>& g, const std::vector<double>& r) {
		const auto n = g.size();
		const auto m2 = r.size() - n;
		const auto& rf = rfft_for(next_pow2(r.size() + n));
		thread_local std::vector<cplx> rs, gs;
		thread_local std::vector<double> c;
		rs.resize(rf.bins());
		gs.resize(rf.bins());
		c.resize(rf.size());
		rf.forward(r.data(), r.size(), 1, rs.data());
		rf.forward(g.data(), n, 1, gs.data());
		for (size_t k = 0; k < rs.size(); ++k)
			rs[k] *= std::conj(gs[k]);
		rf.inverse(rs.data(), c.data());

		std::vector<double> cs(r.size() + 1, 0.);
		for (size_t i = 0; i < r.size(); ++i)
			cs[i + 1] = cs[i] + r[i] * r[i];
		auto gn = 0.;
		for (const auto v : g)
			gn += v * v;
		gn = std::sqrt(gn) + 1e-20;
		std::vector<double> ar(m2 + 1);
		for (size_t k = 0; k <= m2; ++k)
			ar[k] = std::abs(c[k] / (std::sqrt((std::max)(cs[n + k] - cs[k], 1e-20)) * gn));
		// The peak of |rho|: a polarity-inverted mix (M14's DT_053) peaks negative, and its
		// largest positive value sits at a wrong lag.
		const auto k = static_cast<size_t>(std::ranges::max_element(ar) - ar.begin());
		auto d = 0.;
		if (k > 0 && k < m2) {
			const auto y0 = ar[k - 1], y1 = ar[k], y2 = ar[k + 1];
			if (const auto den = y0 - 2 * y1 + y2; den < 0)
				d = 0.5 * (y0 - y2) / den;
		}
		return {static_cast<double>(k) + d - static_cast<double>(m2) / 2., ar[k]};
	}

	// The copy's lag in `sig` over [t0, t1] as a line (samples), or nothing.
	std::optional<lag_fit> find_lag(const detector_audio& sig, const copy_signal& cs, double t0, double t1) {
		const auto m = static_cast<ptrdiff_t>(LagMax * Sr);
		const auto n = static_cast<size_t>(LagWin * Sr);
		struct pt { double T, Lag, Rho; };
		std::vector<pt> pts;
		std::vector<double> raw, g, r;
		for (auto t = t0; t + LagWin <= t1 + 1e-9; t += LagStep) {
			if (cs.available(t, t + LagWin) <= 0.95)
				continue;
			slice_rows(sig, static_cast<ptrdiff_t>(std::nearbyint(t * Sr)), n, raw);
			mono_of(raw, sig.Channels, n, g);
			// First-differenced: the low end would otherwise dominate the correlation, and a
			// sustained bass note correlates almost as well a few milliseconds either way.
			for (size_t i = n; i-- > 1;)
				g[i] -= g[i - 1];
			g[0] = 0.;
			auto e = 0.;
			for (const auto v : g)
				e += v * v;
			if (!(std::sqrt(e / static_cast<double>(n)) > 1e-6))
				continue;
			const auto pos = static_cast<ptrdiff_t>(std::nearbyint((cs.Cp->C + t) * Sr));
			cs.mono_rows(pos - m, n + 2 * static_cast<size_t>(m), r);
			for (size_t i = r.size(); i-- > 1;)
				r[i] -= r[i - 1];
			r[0] = 0.;
			const auto [lag, rho] = xcorr_peak(g, r);
			pts.push_back({t + LagWin / 2, lag, rho});
		}
		std::vector<pt> good;
		for (const auto& p : pts)
			if (p.Rho >= LagRhoMin)
				good.push_back(p);
		if (good.empty()) {
			for (const auto& p : pts)
				if (p.Rho >= 0.25 && std::abs(p.Lag) < static_cast<double>(m) - 2)
					good.push_back(p);
			if (good.empty())
				return std::nullopt;
			std::ranges::stable_sort(good, [](const pt& x, const pt& y) { return x.Rho > y.Rho; });
			if (good.size() > 3)
				good.resize(3);
		}
		std::vector<double> ts;
		for (const auto& p : good)
			ts.push_back(p.T);
		const auto tref = median(ts);
		auto a = 0., b = 0.;
		for (int iter = 0; iter < 4; ++iter) {
			auto tmin = good.front().T, tmax = good.front().T;
			for (const auto& p : good) {
				tmin = (std::min)(tmin, p.T);
				tmax = (std::max)(tmax, p.T);
			}
			if (tmax - tmin >= LagMinSpan && good.size() >= 3) {
				// Weighted least squares on [1, t - tref], weights rho^2.
				auto s0 = 0., s1 = 0., s2 = 0., y0 = 0., y1 = 0.;
				for (const auto& p : good) {
					const auto w = p.Rho * p.Rho, x = p.T - tref;
					s0 += w;
					s1 += w * x;
					s2 += w * x * x;
					y0 += w * p.Lag;
					y1 += w * x * p.Lag;
				}
				const auto det = s0 * s2 - s1 * s1;
				a = (s2 * y0 - s1 * y1) / det;
				b = (s0 * y1 - s1 * y0) / det;
				b = std::clamp(b, -MaxDrift * Sr, MaxDrift * Sr);
			} else {
				// The rho^2-weighted median lag, no drift.
				std::vector<size_t> o(good.size());
				std::iota(o.begin(), o.end(), size_t{0});
				std::ranges::stable_sort(o, [&](size_t x, size_t y) { return good[x].Lag < good[y].Lag; });
				std::vector<double> cw(o.size());
				auto acc = 0.;
				for (size_t i = 0; i < o.size(); ++i)
					cw[i] = acc += good[o[i]].Rho * good[o[i]].Rho;
				const auto half = cw.back() / 2;
				const auto at = static_cast<size_t>(std::ranges::lower_bound(cw, half) - cw.begin());
				a = good[o[(std::min)(at, o.size() - 1)]].Lag;
				b = 0.;
			}
			std::vector<pt> kept;
			for (const auto& p : good)
				if (std::abs(p.Lag - (a + b * (p.T - tref))) <= LagOutlierSamples)
					kept.push_back(p);
			if (kept.size() == good.size() || kept.empty())
				break;
			good = std::move(kept);
		}
		std::vector<double> rhos;
		for (const auto& p : good)
			rhos.push_back(p.Rho);
		return lag_fit{a, b, tref, good.size(), median(rhos)};
	}

	std::vector<std::pair<size_t, size_t>> band_edges(size_t nfft) {
		const auto val = 1. / (static_cast<double>(nfft) * (1. / Sr));
		const auto bins = nfft / 2 + 1;
		const auto l0 = std::log10(BandLo), l1 = std::log10(BandHi);
		const auto step = (l1 - l0) / static_cast<double>(NBands);
		std::vector<double> edges(NBands + 1);
		for (size_t i = 0; i <= NBands; ++i)
			edges[i] = std::pow(10., static_cast<double>(i) * step + l0);
		edges.front() = BandLo;
		edges.back() = BandHi;
		const auto search = [&](double f) {
			size_t k = 0;
			while (k < bins && static_cast<double>(k) * val < f)
				++k;
			return k;
		};
		std::vector<std::pair<size_t, size_t>> res;
		for (size_t i = 0; i < NBands; ++i)
			res.emplace_back(search(edges[i]), search(edges[i + 1]));
		return res;
	}

	// Per window: the weights of `sig` on the copies, and -- for the game -- the band Gram
	// the scores are computed from.
	struct fit_result {
		size_t Nw = 0, K = 0;
		std::vector<double> W, Id, Xn;       // nw x K
		std::vector<double> Gn, Mn, R2;      // nw
		struct gram {
			std::vector<size_t> Idx;
			std::vector<double> G, R, T;     // band-whitened Gram and rhs, time-domain Gram
			size_t Bands = 0;
		};
		std::vector<gram> Grams;

		double& w(size_t j, size_t k) { return W[j * K + k]; }
		[[nodiscard]] double w(size_t j, size_t k) const { return W[j * K + k]; }
	};

	fit_result fit_windows(const detector_audio& sig, const std::vector<copy_signal>& css,
		const std::vector<lag_fit>& lags, double t0, size_t nw, size_t nch, bool keepGram) {

		const auto n = static_cast<size_t>(std::nearbyint(FitLen * Sr));
		const auto k = css.size();
		fit_result f;
		f.Nw = nw;
		f.K = k;
		f.W.assign(nw * k, Nan);
		f.Id.assign(nw * k, 0.);
		f.Xn.assign(nw * k, 0.);
		f.Gn.assign(nw, 0.);
		f.Mn.assign(nw, 0.);
		f.R2.assign(nw, 0.);
		if (keepGram)
			f.Grams.resize(nw);
		const auto nfft = next_pow2(n);
		const auto& rf = rfft_for(nfft);
		const auto bands = band_edges(nfft);
		const auto bins = rf.bins();
		const auto sq = std::sqrt(Win / FitLen);

		std::vector<double> raw, g;
		std::vector<cplx> gf(bins * nch);
		std::vector<std::vector<double>> xs;
		std::vector<std::vector<cplx>> xf;
		std::vector<size_t> idx;
		for (size_t j = 0; j < nw; ++j) {
			const auto tc = t0 + (static_cast<double>(j) + 0.5) * Win;
			const auto tw = tc - FitLen / 2;
			slice_rows(sig, static_cast<ptrdiff_t>(std::nearbyint(tw * Sr)), n, raw);
			fold(raw, sig.Channels, n, nch, g);
			auto gg = 0.;
			for (const auto v : g)
				gg += v * v;
			f.Gn[j] = std::sqrt(gg) * sq;

			xs.clear();
			idx.clear();
			for (size_t q = 0; q < k; ++q) {
				const auto& cs = css[q];
				if (cs.available(tc - Win / 2, tc + Win / 2) < 0.5)
					continue;
				std::vector<double> x;
				cs.read((cs.Cp->C + tw) * Sr + lags[q].at(tc), n, x);
				auto xx = 0.;
				for (const auto v : x)
					xx += v * v;
				const auto nx = std::sqrt(xx);
				f.Xn[j * k + q] = nx * sq;
				if (nx < 1e-7 * std::sqrt(static_cast<double>(x.size()))) {
					f.w(j, q) = 0.;
					continue;
				}
				xs.push_back(std::move(x));
				idx.push_back(q);
			}
			if (xs.empty())
				continue;

			for (size_t c = 0; c < nch; ++c)
				rf.forward(g.data() + c, n, nch, gf.data() + c * bins);
			const auto kq = xs.size();
			xf.resize(kq);
			for (size_t q = 0; q < kq; ++q) {
				xf[q].resize(bins * nch);
				for (size_t c = 0; c < nch; ++c)
					rf.forward(xs[q].data() + c, n, nch, xf[q].data() + c * bins);
			}

			// Whitened LS: every band normalised by the signal's own energy in it, so each
			// band has an equal say -- the same measure the shape score uses.
			std::vector<double> eall(bands.size(), 0.);
			for (size_t b = 0; b < bands.size(); ++b)
				for (size_t c = 0; c < nch; ++c)
					for (auto i = bands[b].first; i < bands[b].second; ++i)
						eall[b] += std::norm(gf[c * bins + i]);
			const auto emax = eall.empty() ? 0. : *std::ranges::max_element(eall);
			std::vector<double> gt(kq * kq, 0.), rt(kq, 0.);
			size_t nbu = 0;
			for (size_t b = 0; b < bands.size(); ++b) {
				const auto [lo, hi] = bands[b];
				const auto e = eall[b];
				if (hi - lo < 2 || e <= BandFloor * emax || e <= 0.)
					continue;
				for (size_t p = 0; p < kq; ++p) {
					auto rs = 0.;
					for (size_t c = 0; c < nch; ++c)
						for (auto i = lo; i < hi; ++i)
							rs += (std::conj(xf[p][c * bins + i]) * gf[c * bins + i]).real();
					rt[p] += rs / e;
					for (size_t q = p; q < kq; ++q) {
						auto s = 0.;
						for (size_t c = 0; c < nch; ++c)
							for (auto i = lo; i < hi; ++i)
								s += (std::conj(xf[p][c * bins + i]) * xf[q][c * bins + i]).real();
						gt[p * kq + q] += s / e;
						if (q != p)
							gt[q * kq + p] += s / e;
					}
				}
				++nbu;
			}
			if (!nbu)
				continue;

			std::vector<double> dg(kq), gnrm(kq * kq), rn(kq), wn, ident;
			for (size_t p = 0; p < kq; ++p)
				dg[p] = std::sqrt((std::max)(gt[p * kq + p], 1e-30));
			for (size_t p = 0; p < kq; ++p) {
				rn[p] = rt[p] / dg[p];
				for (size_t q = 0; q < kq; ++q)
					gnrm[p * kq + q] = gt[p * kq + q] / (dg[p] * dg[q]);
			}
			tsvd_solve(gnrm, rn, kq, wn, ident);
			std::vector<double> w(kq);
			for (size_t p = 0; p < kq; ++p)
				w[p] = wn[p] / dg[p];
			// Residual per band over the band's own energy, averaged: nb - 2 w.r + w'Gw, over nb.
			auto wr = 0., wgw = 0.;
			for (size_t p = 0; p < kq; ++p) {
				wr += w[p] * rt[p];
				for (size_t q = 0; q < kq; ++q)
					wgw += w[p] * gt[p * kq + q] * w[q];
			}
			f.R2[j] = (std::max)(0., 1. - (static_cast<double>(nbu) - 2. * wr + wgw) / static_cast<double>(nbu));
			for (size_t p = 0; p < kq; ++p) {
				f.w(j, idx[p]) = w[p];
				f.Id[j * k + idx[p]] = ident[p];
			}
			std::vector<double> tg(kq * kq, 0.);
			for (size_t p = 0; p < kq; ++p)
				for (size_t q = p; q < kq; ++q) {
					auto s = 0.;
					for (size_t i = 0; i < xs[p].size(); ++i)
						s += xs[p][i] * xs[q][i];
					tg[p * kq + q] = tg[q * kq + p] = s;
				}
			auto mm = 0.;
			for (size_t p = 0; p < kq; ++p)
				for (size_t q = 0; q < kq; ++q)
					mm += w[p] * tg[p * kq + q] * w[q];
			f.Mn[j] = std::sqrt((std::max)(mm, 0.)) * sq;
			if (keepGram)
				f.Grams[j] = {idx, std::move(gt), std::move(rt), std::move(tg), nbu};
		}
		return f;
	}

	// Signed level of each recording: median |sum of its copies' weights| over the lead-in or
	// lead-out windows where it carries the model. Per render rather than per copy, because a
	// crossfade between two copies of one recording moves weight between them while their sum
	// holds -- which is what makes it a scale.
	std::vector<double> source_scales(const fit_result& f, const std::vector<copy_signal>& css) {
		const auto nw = f.Nw, k = f.K;
		const auto key = [&](size_t q) { return lower_utf8(css[q].Cp->File) + "|" + css[q].Cp->Render; };
		std::map<std::string, double> out;
		const auto ne = static_cast<size_t>(Edge / Win);
		for (size_t q = 0; q < k; ++q) {
			const auto s = key(q);
			if (out.contains(s))
				continue;
			std::vector<size_t> ks;
			for (size_t p = 0; p < k; ++p)
				if (key(p) == s)
					ks.push_back(p);
			std::vector<double> tot(nw, 0.);
			std::vector<bool> ok(nw, false);
			for (size_t j = 0; j < nw; ++j) {
				auto share = 0.;
				for (const auto p : ks) {
					const auto w = std::isnan(f.w(j, p)) ? 0. : f.w(j, p);
					tot[j] += w;
					share += (w * f.Xn[j * k + p]) * (w * f.Xn[j * k + p]);
				}
				share = std::sqrt(share) / (std::max)(f.Mn[j], 1e-12);
				ok[j] = f.R2[j] > 0.15 && share > 0.7 && std::abs(tot[j]) > 1e-6;
			}
			std::optional<double> val;
			for (int sel = 0; sel < 3 && !val; ++sel) {
				std::vector<double> m;
				for (size_t j = 0; j < nw; ++j) {
					const auto in = sel == 0 ? j < ne : sel == 1 ? j + ne >= nw : true;
					if (ok[j] && in)
						m.push_back(tot[j]);
				}
				if (m.size() >= 3)
					val = median(m);
			}
			out[s] = val && *val != 0. && std::abs(*val) > 1e-6 ? *val : 1.;
		}
		std::vector<double> res(k);
		for (size_t q = 0; q < k; ++q)
			res[q] = out.at(key(q));
		return res;
	}

	// Median of 3 over the non-NaN neighbours: keeps steps, drops single-window outliers.
	std::vector<double> med3(const std::vector<double>& w) {
		auto out = w;
		for (size_t j = 0; j < w.size(); ++j) {
			if (std::isnan(w[j]))
				continue;
			std::vector<double> v;
			for (size_t i = j ? j - 1 : 0; i <= j + 1 && i < w.size(); ++i)
				if (!std::isnan(w[i]))
					v.push_back(w[i]);
			out[j] = median(v);
		}
		return out;
	}

	struct crossing { double Ts, T50, Te; };

	// A normalised weight curve's move from lvl0 to lvl1, by its 10/50/90 % points.
	std::optional<crossing> crossings(const std::vector<double>& t, const std::vector<double>& w, double lvl0, double lvl1) {
		std::vector<double> tt, f;
		for (size_t i = 0; i < w.size(); ++i)
			if (!std::isnan(w[i])) {
				tt.push_back(t[i]);
				f.push_back((w[i] - lvl0) / (lvl1 - lvl0));
			}
		if (tt.size() < 3 || std::abs(lvl1 - lvl0) < 0.3)
			return std::nullopt;
		std::optional<size_t> k;
		size_t above = 0;
		for (size_t i = 0; i < f.size(); ++i) {
			if (f[i] >= 0.5)
				++above;
			else
				k = i;
		}
		if (above == f.size() || !above || !k || *k + 1 >= f.size())
			return std::nullopt;
		// 50 %: the last crossing into the "after" side.
		const auto t50 = tt[*k] + (0.5 - f[*k]) / (std::max)(f[*k + 1] - f[*k], 1e-9) * (tt[*k + 1] - tt[*k]);
		auto ts = tt.front(), te = tt.back();
		for (size_t i = 0; i < f.size(); ++i)
			if (f[i] <= 0.1 && tt[i] < t50)
				ts = tt[i];
		for (size_t i = f.size(); i-- > 0;)
			if (f[i] >= 0.9 && tt[i] > t50)
				te = tt[i];
		return crossing{ts, t50, te};
	}

	std::string shape_guess(const std::vector<double>& t, const std::vector<double>& w, double lvl0, double lvl1, const crossing& cr) {
		if (cr.Te - cr.Ts <= 0.6)
			return "step";
		std::vector<double> x, f;
		for (size_t i = 0; i < w.size(); ++i)
			if (!std::isnan(w[i]) && t[i] >= cr.Ts && t[i] <= cr.Te) {
				x.push_back((t[i] - cr.Ts) / (cr.Te - cr.Ts));
				f.push_back((w[i] - lvl0) / (lvl1 - lvl0));
			}
		if (x.size() < 4)
			return "short";
		// A hold: a long flat stretch at an intermediate level, then a quick move.
		std::vector<double> mid;
		for (size_t i = 0; i < x.size(); ++i)
			if (x[i] > 0.25 && x[i] < 0.75)
				mid.push_back(f[i]);
		if (mid.size() >= 3) {
			const auto mean = std::accumulate(mid.begin(), mid.end(), 0.) / static_cast<double>(mid.size());
			auto var = 0.;
			for (const auto v : mid)
				var += (v - mean) * (v - mean);
			const auto sd = std::sqrt(var / static_cast<double>(mid.size()));
			const auto md = median(mid);
			if (sd < 0.08 && md > 0.2 && md < 0.8)
				return std::format("hold {:.2f}", lvl0 + md * (lvl1 - lvl0));
		}
		const std::pair<const char*, double (*)(double)> shapes[] = {
			{"linear", [](double v) { return v; }},
			{"equal-power", [](double v) { return std::sin(v * std::numbers::pi / 2); }},
			{"hsin", [](double v) { return 0.5 - 0.5 * std::cos(std::numbers::pi * v); }},
			{"squared", [](double v) { return v * v; }},
			{"sqrt", [](double v) { return std::sqrt(v); }},
		};
		const char* best = nullptr;
		auto bestErr = 0.;
		for (const auto& [name, fn] : shapes) {
			auto e = 0.;
			for (size_t i = 0; i < x.size(); ++i)
				e += (fn(x[i]) - f[i]) * (fn(x[i]) - f[i]);
			e /= static_cast<double>(x.size());
			if (!best || e < bestErr) {
				best = name;
				bestErr = e;
			}
		}
		return best;
	}

	detector_curve describe(const std::vector<double>& t, const std::vector<double>& w) {
		detector_curve d;
		std::vector<double> ww;
		for (const auto v : w)
			if (!std::isnan(v))
				ww.push_back(v);
		if (ww.size() < 4)
			return d;
		const auto ne = (std::max)(size_t{3}, static_cast<size_t>(2. / Win));
		const auto head = std::vector<double>(ww.begin(), ww.begin() + static_cast<ptrdiff_t>((std::min)(ne, ww.size())));
		const auto tail = std::vector<double>(ww.end() - static_cast<ptrdiff_t>((std::min)(ne, ww.size())), ww.end());
		d.Start = median(head);
		d.End = median(tail);
		if (const auto cr = crossings(t, w, d.Start, d.End)) {
			d.Kind = d.End < d.Start ? detector_curve::kind::Out : detector_curve::kind::In;
			d.T10 = round_to(cr->Ts, 100.);
			d.T50 = round_to(cr->T50, 100.);
			d.T90 = round_to(cr->Te, 100.);
			d.Shape = shape_guess(t, w, d.Start, d.End, *cr);
		} else {
			d.Kind = detector_curve::kind::Steady;
			d.Mid = median(ww);
		}
		return d;
	}
}

std::string detector_copy::name() const {
	return std::format("{}{}@{:+.3f}", Source, Label.empty() ? "" : "[" + Label + "]", C);
}

std::string detector_copy::key() const {
	std::string map;
	for (const auto c : ChannelMap)
		map += std::format("{},", c);
	return std::format("{}|{}|{}|{}", lower_utf8(File), Render, std::nearbyint(C * 1000.), map);
}

detector_plan plan_copies(const std::vector<apply_segment>& segments, size_t gameChannels,
	const std::function<double(const std::filesystem::path&)>& nativeRate) {

	detector_plan plan;
	// Whether there is anything here a join detector would judge, before asking whether it
	// can: more than one segment, or a filter that sums delayed copies of its source.
	auto candidate = segments.size() > 1;
	for (const auto& seg : segments)
		for (const auto& [name, src] : seg.Sources)
			if (src.Graph || xivres::util::unicode::convert<std::string>(src.Filter).find("asplit") != std::string::npos)
				candidate = true;
	const auto refuse = [&](std::string why) {
		plan.NotAnalysable = std::move(why);
		plan.MultiCopy = candidate;
		plan.Copies.clear();
		return plan;
	};
	if (gameChannels > 2)
		return refuse(std::format("{}-channel stems", gameChannels));

	// Where each segment starts: the walk `apply` makes. A segment that states its own start
	// is placed there and the rest still follow on from it.
	std::vector<double> start(segments.size());
	auto cursorSeconds = 0.;
	for (size_t i = 0; i < segments.size(); ++i) {
		start[i] = segments[i].StartSeconds >= 0. ? segments[i].StartSeconds : cursorSeconds;
		cursorSeconds = start[i] + segments[i].Length;
	}

	std::map<std::string, size_t> byKey;
	for (size_t i = 0; i < segments.size(); ++i) {
		const auto& seg = segments[i];
		const auto st = start[i];
		if (i > 0)
			plan.Events.push_back(st);
		// The sources this segment plays, in the order its channels first name them. A name
		// the preset gives an offset but routes to no channel is not heard.
		std::vector<std::string> names;
		for (const auto& [name, _ch] : seg.Channels)
			if (std::ranges::find(names, name) == names.end())
				names.push_back(name);
		for (const auto& name : names) {
			const auto it = seg.Sources.find(name);
			if (it == seg.Sources.end())
				continue;
			const auto& src = it->second;
			if (src.IsTarget) {
				plan.Notes.push_back(std::format("seg{} plays the game's own audio (not modelled)", i));
				continue;
			}
			if (src.Graph)
				return refuse(std::format("source \"{}\" is built by a filter graph", name));
			// An offset `apply` re-derives by onset alignment is not the one written down, and
			// a copy searched for +-50 ms around the wrong place is a copy not found.
			if (!src.FixesAlignment())
				return refuse(std::format("source \"{}\" is placed by onset alignment at build time", name));
			filter_copies fc;
			try {
				fc = copies_from_filter(xivres::util::unicode::convert<std::string>(src.Filter),
					src.Filter.find(L"S") != std::wstring::npos ? nativeRate(src.Path) : Sr);
			} catch (const std::exception& e) {
				return refuse(std::format("source \"{}\": filter not understood ({})", name, e.what()));
			}
			if (!fc.Unsupported.empty())
				return refuse(std::format("source \"{}\": {}", name, fc.Unsupported));
			for (const auto& n : fc.Notes)
				plan.Notes.push_back(std::format("seg{} {}: {}", i, name, n));
			std::vector<size_t> chmap;
			for (const auto& [n, ch] : seg.Channels)
				if (n == name)
					chmap.push_back(ch);
			for (const auto& [d, label] : fc.Delays) {
				detector_copy cp{
					.Source = name,
					.File = src.Path,
					.Render = fc.Render,
					.C = src.Offset - st - d,
					.ChannelMap = chmap,
					.Label = label,
				};
				if (!byKey.contains(cp.key())) {
					byKey.emplace(cp.key(), plan.Copies.size());
					plan.Copies.push_back(std::move(cp));
				}
				if (d > 0.)
					plan.Events.push_back(-(src.Offset - st - d));   // the delayed copy's first sample of material
			}
		}
	}
	std::ranges::sort(plan.Events);
	plan.MultiCopy = plan.Copies.size() >= 2 && !plan.Events.empty();
	return plan;
}

std::vector<detector_region_span> detector_regions(const std::vector<double>& events, double endSeconds) {
	std::vector<std::vector<double>> clusters;
	for (const auto e : events) {
		if (e <= 0. || e >= endSeconds)
			continue;
		if (!clusters.empty() && e - clusters.back().back() < ClusterGap)
			clusters.back().push_back(e);
		else
			clusters.push_back({e});
	}
	std::vector<detector_region_span> res;
	for (auto& c : clusters)
		res.push_back({(std::max)(0., c.front() - RegionPad), (std::min)(endSeconds, c.back() + RegionPad), std::move(c)});
	return res;
}

std::string detector_region_result::problem() const {
	if (!Error.empty())
		return "not analysable: " + Error;
	std::string res;
	const auto add = [&](const std::string& what) {
		if (!res.empty())
			res += "; ";
		res += what;
	};
	if (ShapeFlag)
		add(std::format("arrangement differs from the game's at {:.1f} s", PeakShapeAt));
	if (LevelFlag)
		add(std::format("level off {:+.1f} dB at {:.1f} s", PeakLevelDb, PeakLevelAt));
	if (LagFlag)
		add(std::format("copies {:.1f} ms apart", RelLagMs));
	return res;
}

detector_region_result analyse_join_region(const detector_audio& game, const detector_audio& build,
	size_t nch, const std::vector<detector_copy_audio>& copies, const detector_region_span& region) {

	detector_region_result res;
	res.Joins = region.Joins;
	const auto t0 = region.From;
	const auto t1 = (std::min)({region.To, static_cast<double>(build.Frames) / Sr - 0.01,
		static_cast<double>(game.Frames) / Sr - 0.01});
	res.From = round_to(t0, 100.);
	res.To = round_to(t1, 100.);

	// Copies present in the region at all.
	std::vector<copy_signal> css;
	for (const auto& ca : copies) {
		copy_signal cs(ca, nch);
		if (cs.available(t0, t1) > 0.02)
			css.push_back(cs);
	}
	if (css.empty()) {
		res.Error = "no copy has material here";
		return res;
	}
	if (!(t1 > t0)) {
		res.Error = "the region lies past the end of the build or the game's file";
		return res;
	}
	const auto nw = static_cast<size_t>((t1 - t0) / Win);
	if (!nw) {
		res.Error = "the region is shorter than one window";
		return res;
	}
	res.To = round_to(t0 + static_cast<double>(nw) * Win, 100.);
	const auto k = css.size();
	for (const auto& cs : css)
		res.Copies.push_back(cs.Cp->name());

	std::vector<std::optional<lag_fit>> lg(k), lb(k);
	for (size_t q = 0; q < k; ++q) {
		lg[q] = find_lag(game, css[q], t0, t1);
		lb[q] = find_lag(build, css[q], t0, t1);
	}
	// A copy one signal never plays borrows the other's lag plus the common build-game offset.
	std::vector<double> diffs;
	for (size_t q = 0; q < k; ++q)
		if (lg[q] && lb[q])
			diffs.push_back(lb[q]->A + lb[q]->B * (lg[q]->TRef - lb[q]->TRef) - lg[q]->A);
	const auto off = diffs.empty() ? 0. : median(diffs);
	std::vector<lag_fit> lgv(k), lbv(k);
	for (size_t q = 0; q < k; ++q) {
		if (!lg[q] && lb[q])
			lg[q] = lag_fit{lb[q]->A - off, lb[q]->B, lb[q]->TRef, 0, 0.};
		if (!lb[q] && lg[q])
			lb[q] = lag_fit{lg[q]->A + off, lg[q]->B, lg[q]->TRef, 0, 0.};
		if (!lg[q])
			lg[q] = lb[q] = lag_fit{0., 0., t0, 0, 0.};
		lgv[q] = *lg[q];
		lbv[q] = *lb[q];
	}

	const auto fg = fit_windows(game, css, lgv, t0, nw, nch, true);
	const auto fb = fit_windows(build, css, lbv, t0, nw, nch, false);
	const auto sg = source_scales(fg, css);
	const auto sb = source_scales(fb, css);

	// Per window (FitLen long, centred), on the game-lag copies X_k:
	//   u = sum_k a^game_k X_k                      the game's own fitted arrangement
	//   v = sum_k a^build_k / s^build_k * s^game_k X_k   the build's arrangement at the game's level
	// In the band-whitened inner product <x,y>_W = sum_b Re<x_b,y_b> / e_b (e_b the game's
	// energy in band b; bands 30 dB under the window's loudest skipped):
	//   shape = c(u)^2 - c(v)^2, c(x) = <g,x>_W / (|g|_W |x|_W)
	//     = the fraction of the game the build's copy *ratio* fails to explain, after its best
	//       rescaling, beyond what the best ratio fails to explain. Scale-free, so a copy the
	//       game plays at the right level but in a different mix (lower coherence) is not
	//       counted -- only which copies sound, and in what proportion.
	//   level = 10 log10(E_build / E_game) per window, minus its region median.
	// Both u and v are sums over the columns the game fit already transformed, so every term
	// is a quadratic form in that window's band Gram and needs no second read of any copy.
	auto blag = 0.;
	{
		std::vector<double> d;
		for (size_t q = 0; q < k; ++q)
			d.push_back(lbv[q].A - lgv[q].A);
		blag = median(d);
	}
	const auto n = static_cast<size_t>(std::nearbyint(FitLen * Sr));
	std::vector<double> err(nw, 0.), den(nw, 0.), shp(nw, 0.), gen(nw, 0.), ben(nw, 0.);
	std::vector<double> raw, folded;
	for (size_t j = 0; j < nw; ++j) {
		const auto tc = t0 + (static_cast<double>(j) + 0.5) * Win;
		const auto tw = tc - FitLen / 2;
		slice_rows(game, static_cast<ptrdiff_t>(std::nearbyint(tw * Sr)), n, raw);
		fold(raw, game.Channels, n, nch, folded);
		for (const auto v : folded)
			gen[j] += v * v;
		slice_rows(build, static_cast<ptrdiff_t>(std::nearbyint(tw * Sr + blag)), n, raw);
		fold(raw, build.Channels, n, nch, folded);
		for (const auto v : folded)
			ben[j] += v * v;
		const auto& gm = fg.Grams[j];
		if (gm.Idx.empty())
			continue;
		const auto kq = gm.Idx.size();
		std::vector<double> a(kq), b(kq);
		for (size_t p = 0; p < kq; ++p) {
			const auto q = gm.Idx[p];
			const auto wg = fg.w(j, q);
			const auto wb = fb.w(j, q) / sb[q];
			a[p] = std::isnan(wg) ? 0. : wg;
			b[p] = (std::isnan(wb) ? 0. : wb) * sg[q];
		}
		auto gu = 0., uu = 0., gv = 0., vv = 0., ee = 0., dd = 0.;
		for (size_t p = 0; p < kq; ++p) {
			gu += a[p] * gm.R[p];
			gv += b[p] * gm.R[p];
			for (size_t q = 0; q < kq; ++q) {
				uu += a[p] * gm.G[p * kq + q] * a[q];
				vv += b[p] * gm.G[p * kq + q] * b[q];
				ee += (a[p] - b[p]) * gm.T[p * kq + q] * (a[q] - b[q]);
				dd += a[p] * gm.T[p * kq + q] * a[q];
			}
		}
		err[j] = (std::max)(ee, 0.);
		den[j] = (std::max)(dd, 0.);
		const auto nb = static_cast<double>(gm.Bands);
		const auto cu2 = (std::max)(gu, 0.) * (std::max)(gu, 0.) / (std::max)(uu * nb, 1e-30);
		const auto cv = gv / std::sqrt((std::max)(vv * nb, 1e-30));
		shp[j] = std::clamp(cu2 - std::copysign(cv * cv, cv), 0., 1.);
	}

	// Quiet windows -- the game 20 dB under the region's median -- do not count.
	std::vector<double> pos;
	for (const auto v : gen)
		if (v > 0.)
			pos.push_back(v);
	const auto genMedian = pos.empty() ? 0. : median(pos);
	std::vector<bool> loud(nw);
	for (size_t j = 0; j < nw; ++j)
		loud[j] = pos.empty() ? gen[j] > 0. : gen[j] >= DenFloor * genMedian;
	std::vector<double> exc(nw), lvl(nw);
	std::vector<double> loudLvl;
	for (size_t j = 0; j < nw; ++j) {
		exc[j] = loud[j] ? shp[j] : 0.;
		lvl[j] = 10. * std::log10((std::max)(ben[j], 1e-12) / (std::max)(gen[j], 1e-12));
		if (loud[j])
			loudLvl.push_back(lvl[j]);
	}
	const auto lvlMedian = loudLvl.empty() ? 0. : median(loudLvl);
	for (size_t j = 0; j < nw; ++j)
		lvl[j] = loud[j] ? lvl[j] - lvlMedian : 0.;
	std::vector<double> denPos;
	for (const auto v : den)
		if (v > 0.)
			denPos.push_back(v);
	const auto floor = denPos.empty() ? 1. : DenFloor * median(denPos);

	// 1 s running means.
	const auto k4 = (std::max)(size_t{1}, static_cast<size_t>(std::nearbyint(1. / Win)));
	const auto running = [&](const std::vector<double>& x) {
		std::vector<double> r;
		if (nw >= k4) {
			for (size_t i = 0; i + k4 <= nw; ++i) {
				auto s = 0.;
				for (size_t m = 0; m < k4; ++m)
					s += x[i + m] * (1. / static_cast<double>(k4));
				r.push_back(s);
			}
		} else {
			r.push_back(std::accumulate(x.begin(), x.end(), 0.) / static_cast<double>(nw));
		}
		return r;
	};
	const auto run1 = running(exc);
	const auto ip = static_cast<size_t>(std::ranges::max_element(run1) - run1.begin());
	res.PeakShape = run1[ip];
	res.PeakShapeAt = t0 + (static_cast<double>(ip) + static_cast<double>(k4) / 2) * Win;
	const auto runl = running(lvl);
	size_t il = 0;
	for (size_t i = 1; i < runl.size(); ++i)
		if (std::abs(runl[i]) > std::abs(runl[il]))
			il = i;
	res.PeakLevelDb = runl[il];
	res.PeakLevelAt = t0 + (static_cast<double>(il) + static_cast<double>(k4) / 2) * Win;
	auto errSum = 0., denSum = 0.;
	for (size_t j = 0; j < nw; ++j) {
		if (exc[j] > TWinErr)
			res.WrongSeconds += Win;
		res.Area += exc[j] * Win;
		errSum += err[j];
		denSum += (std::max)(den[j], floor);
	}
	res.Total = std::sqrt(errSum / (std::max)(denSum, 1e-20));
	res.R2Game = median(fg.R2);
	res.R2Build = median(fb.R2);

	// Per-copy curves, confident windows only; a copy with no material in a window plays
	// nothing there, which is a known zero rather than an unknown.
	std::vector<double> t(nw);
	for (size_t j = 0; j < nw; ++j)
		t[j] = t0 + (static_cast<double>(j) + 0.5) * Win;
	std::vector<double> rl;
	for (size_t q = 0; q < k; ++q) {
		std::vector<double> wg(nw), wb(nw);
		size_t confident = 0;
		for (size_t j = 0; j < nw; ++j) {
			const auto nomat = css[q].available(t0 + static_cast<double>(j) * Win, t0 + static_cast<double>(j + 1) * Win) < 0.5;
			const auto cg = fg.Id[j * k + q] >= IdentMin && fg.Xn[j * k + q] * std::abs(sg[q]) >= AudibleMin * fg.Gn[j];
			const auto cb = fb.Id[j * k + q] >= IdentMin && fb.Xn[j * k + q] * std::abs(sb[q]) >= AudibleMin * fb.Gn[j];
			confident += cg;
			wg[j] = cg ? fg.w(j, q) / sg[q] : nomat ? 0. : Nan;
			wb[j] = cb ? fb.w(j, q) / sb[q] : nomat ? 0. : Nan;
		}
		detector_copy_result cr;
		cr.Name = css[q].Cp->name();
		cr.Game = describe(t, med3(wg));
		cr.Build = describe(t, med3(wb));
		const auto mid = (t0 + t1) / 2;
		cr.LagGameMs = round_to(lgv[q].at(mid) / Sr * 1000., 1000.);
		cr.LagBuildMs = round_to(lbv[q].at(mid) / Sr * 1000., 1000.);
		cr.LagWindowsGame = lgv[q].N;
		cr.LagWindowsBuild = lbv[q].N;
		cr.LagRhoGame = lgv[q].Rho;
		cr.LagRhoBuild = lbv[q].Rho;
		cr.IdentFraction = static_cast<double>(confident) / static_cast<double>(nw);
		using kind = detector_curve::kind;
		const auto moving = [](kind x) { return x == kind::In || x == kind::Out; };
		if (moving(cr.Game.Kind) && cr.Game.Kind == cr.Build.Kind)
			cr.Dt50 = round_to(cr.Build.T50 - cr.Game.T50, 100.);
		else if (cr.Game.Kind != cr.Build.Kind && cr.Game.Kind != kind::Unknown && cr.Build.Kind != kind::Unknown)
			cr.KindMismatch = true;
		if (cr.LagWindowsGame > 0 && cr.LagWindowsBuild > 0)
			rl.push_back(cr.LagBuildMs - cr.LagGameMs);
		res.PerCopy.push_back(std::move(cr));
	}
	// The relative lag between copies both signals play: a constant offset of the whole build
	// is the head/lag probes' business, a copy out of step with its neighbours is this one's.
	res.RelLagMs = rl.size() >= 2 ? *std::ranges::max_element(rl) - *std::ranges::min_element(rl) : 0.;
	res.ShapeFlag = res.PeakShape > TPeak;
	res.LevelFlag = std::abs(res.PeakLevelDb) > TLevel;
	res.LagFlag = res.RelLagMs > TRelLag;
	return res;
}

mapped_audio_file::mapped_audio_file(std::filesystem::path path, size_t channels)
	: m_channels(channels ? channels : 1), m_path(std::move(path)) {
	const auto file = CreateFileW(m_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		throw std::runtime_error("could not open a decoded stream");
	m_file = file;
	LARGE_INTEGER size{};
	GetFileSizeEx(file, &size);
	m_count = static_cast<size_t>(size.QuadPart) / sizeof(float);
	if (m_count) {
		m_mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
		if (!m_mapping)
			throw std::runtime_error("could not map a decoded stream");
		m_data = static_cast<const float*>(MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, 0));
		if (!m_data)
			throw std::runtime_error("could not map a decoded stream");
	}
}

mapped_audio_file::~mapped_audio_file() {
	if (m_data)
		UnmapViewOfFile(m_data);
	if (m_mapping)
		CloseHandle(m_mapping);
	if (m_file)
		CloseHandle(m_file);
	std::error_code ec;
	std::filesystem::remove(m_path, ec);
}

std::shared_ptr<mapped_audio_file> decode_for_detector(const std::filesystem::path& ffmpeg,
	const std::filesystem::path& input, const std::string& filter, size_t channels,
	const std::filesystem::path& rawPath) {

	std::error_code ec;
	std::filesystem::remove(rawPath, ec);
	// soxr at precision 28, the prototype's resampler: the copies are compared with the
	// game's file at the waveform level, and a cheaper resample of one side than the other
	// is a difference the fit would read as a mix.
	auto chain = xivres::util::unicode::convert<std::wstring>(filter);
	if (!chain.empty())
		chain += L",";
	chain += std::format(L"aresample={}:resampler=soxr:precision=28", DetectorRateHz);
	run_process_capture_stdout(ffmpeg, {
		L"-v", L"error", L"-nostdin", L"-y",
		L"-i", input.wstring(),
		L"-map", L"0:a:0",
		L"-af", chain,
		L"-f", L"f32le", rawPath.wstring(),
	});
	return std::make_shared<mapped_audio_file>(rawPath, channels);
}
