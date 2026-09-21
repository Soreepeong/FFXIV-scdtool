#pragma once

#include <algorithm>
#include <vector>

// Median, matching numpy's: the mean of the two middle values on an even count.
//
// Shared because both comparisons subtract one before measuring a hole, and for the same
// reason: the question is "is the build quieter *here* than it is generally", so the general
// level difference has to come out first or a quieter master reads as a hole everywhere.
// Reorders its argument.
inline double median_of(std::vector<double>& v) {
	if (v.empty())
		return 0.;
	const auto mid = v.size() / 2;
	std::ranges::nth_element(v, v.begin() + mid);
	const auto hi = v[mid];
	if (v.size() % 2)
		return hi;
	std::ranges::nth_element(v, v.begin() + (mid - 1));
	return (hi + v[mid - 1]) / 2.;
}
