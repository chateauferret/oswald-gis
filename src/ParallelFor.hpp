#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <thread>
#include <vector>

template <typename Function>
void parallelFor(std::size_t count, std::size_t minimumChunkSize, const Function& function) {
    const auto available = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t workers = std::min<std::size_t>(
        available, count / minimumChunkSize);
    if (workers < 2) {
        for (std::size_t i = 0; i < count; ++i) {
            function(i);
        }
        return;
    }

    std::vector<std::exception_ptr> errors(workers);
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
        const auto begin = (count / workers) * worker + std::min(worker, count % workers);
        const auto end = begin + count / workers + (worker < count % workers ? 1 : 0);
        threads.emplace_back([&, worker, begin, end] {
            try {
                for (std::size_t i = begin; i < end; ++i) {
                    function(i);
                }
            } catch (...) {
                errors[worker] = std::current_exception();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
}
