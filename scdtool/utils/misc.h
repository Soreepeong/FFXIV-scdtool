#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

// A temp directory of this process's own, under %TEMP%. Every command numbers its temp files
// from 0, so two runs sharing %TEMP% itself wrote each other's files -- concurrent verify runs
// returned each other's numbers, and one's cleanup deleted the other's inputs. Removed with
// everything in it on destruction: a file still locked when its own cleanup ran (about one
// apply job in 175) used to stay in %TEMP% for good, and 5.1 GB of raw decodes had piled up.
class process_temp_directory {
	std::filesystem::path m_path;

public:
	explicit process_temp_directory(std::wstring_view command)
		: m_path(std::filesystem::temp_directory_path() / std::format(L"scdtool_{}_{}", command, GetCurrentProcessId())) {
		std::filesystem::create_directories(m_path);
	}
	process_temp_directory(const process_temp_directory&) = delete;
	process_temp_directory& operator=(const process_temp_directory&) = delete;
	~process_temp_directory() {
		std::error_code ec;
		std::filesystem::remove_all(m_path, ec);
	}

	[[nodiscard]] const std::filesystem::path& path() const { return m_path; }
};

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
