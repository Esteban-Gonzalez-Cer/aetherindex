#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace aetherindex {

// Runs body(i) for every i in [0, count) on up to `threads` threads (the caller is one
// of them), handing out indices dynamically. threads <= 1 runs inline, in order. The
// first exception stops further work and is rethrown after every thread has joined.
template <class Body>
void parallel_for(std::size_t count, std::size_t threads, Body&& body) {
    threads = std::min(threads, count);
    if (threads <= 1) {
        for (std::size_t i = 0; i < count; ++i) body(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::mutex error_mutex;
    const auto work = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count || failed.load(std::memory_order_relaxed)) return;
            try {
                body(i);
            } catch (...) {
                const std::lock_guard lock(error_mutex);
                if (!error) error = std::current_exception();
                failed.store(true, std::memory_order_relaxed);
                return;
            }
        }
    };
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads - 1);
        for (std::size_t t = 1; t < threads; ++t) workers.emplace_back(work);
        work();
    }  // jthread joins on destruction.
    if (error) std::rethrow_exception(error);
}

// Hardware threads, or 1 when the runtime cannot tell.
inline std::size_t hardware_threads() noexcept {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

}  // namespace aetherindex
