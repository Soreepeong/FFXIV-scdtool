#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

// Runs fn(i) for every i in [0, count) across a pool of threads, and blocks until all of
// them are done. If an invocation throws, the remaining items are still picked up by the
// other workers (so one failure does not strand the rest), and the first exception is
// rethrown on the calling thread afterwards. `fn` must be safe to call concurrently.
//
// `maxThreads` caps the size of the pool. 0 -- the default -- means "one per hardware
// thread", which is what a CPU-bound item wants; pass a smaller number when each item also
// needs a lot of memory, so that few enough of them are in flight at once.
template<typename Fn>
void parallel_for(size_t count, Fn fn, size_t maxThreads = 0) {
	if (!count)
		return;
	const auto threadCount = maxThreads
		? (std::min)(count, maxThreads)
		: (std::min)(count, static_cast<size_t>((std::max)(1u, std::thread::hardware_concurrency())));
	std::atomic<size_t> next{0};
	std::mutex errorMutex;
	std::exception_ptr firstError;
	std::vector<std::thread> threads;
	threads.reserve(threadCount);
	for (size_t t = 0; t < threadCount; ++t) {
		threads.emplace_back([&fn, &next, count, &errorMutex, &firstError] {
			while (true) {
				const auto i = next.fetch_add(1);
				if (i >= count)
					break;
				try {
					fn(i);
				} catch (...) {
					std::scoped_lock lock(errorMutex);
					if (!firstError)
						firstError = std::current_exception();
				}
			}
		});
	}
	for (auto& th : threads)
		th.join();
	if (firstError)
		std::rethrow_exception(firstError);
}
