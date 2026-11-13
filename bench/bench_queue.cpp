// SPDX-License-Identifier: MIT
//
// Throughput and round-trip latency benchmark comparing
// lockfree_spsc::spsc_queue with a std::mutex + std::queue baseline.
//
// Both queues are bounded to the same capacity and both sides busy-poll
// with the same non-blocking try_push / try_pop interface, so the only
// difference measured is the synchronisation strategy.
//
// Usage: bench_queue [throughput_items] [latency_round_trips]

#include <lockfree_spsc/spsc_queue.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

constexpr std::size_t queue_capacity = 1024;
constexpr int repetitions = 5;
constexpr std::size_t latency_batch = 100;

// ---------------------------------------------------------------------------
// Baseline: bounded queue guarded by a mutex
// ---------------------------------------------------------------------------
template <typename T>
class mutex_queue {
public:
    explicit mutex_queue(std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] bool try_push(const T& value) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= capacity_) {
            return false;
        }
        queue_.push(value);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = queue_.front();
        queue_.pop();
        return true;
    }

private:
    std::mutex mutex_;
    std::queue<T> queue_;
    std::size_t capacity_;
};

using lockfree_queue = lockfree_spsc::spsc_queue<std::uint64_t>;
using locked_queue = mutex_queue<std::uint64_t>;

// ---------------------------------------------------------------------------
// Workloads
// ---------------------------------------------------------------------------

template <typename Queue>
void push_blocking(Queue& queue, std::uint64_t value) {
    while (!queue.try_push(value)) {
    }
}

template <typename Queue>
std::uint64_t pop_blocking(Queue& queue) {
    std::uint64_t value = 0;
    while (!queue.try_pop(value)) {
    }
    return value;
}

/// One producer streams `items` integers to one consumer.
/// @return Throughput in millions of items per second.
template <typename Queue>
double run_throughput(std::uint64_t items) {
    Queue queue(queue_capacity);
    std::uint64_t checksum = 0;

    std::thread consumer([&] {
        std::uint64_t sum = 0;
        for (std::uint64_t i = 0; i < items; ++i) {
            sum += pop_blocking(queue);
        }
        checksum = sum;
    });

    const auto start = clock_type::now();
    for (std::uint64_t i = 0; i < items; ++i) {
        push_blocking(queue, i);
    }
    consumer.join();
    const auto stop = clock_type::now();

    if (checksum != items * (items - 1) / 2) {
        std::fprintf(stderr, "throughput checksum mismatch\n");
        std::exit(EXIT_FAILURE);
    }
    const double seconds = std::chrono::duration<double>(stop - start).count();
    return static_cast<double>(items) / seconds / 1e6;
}

struct latency_result {
    double mean_ns;
    double p50_ns;
    double p99_ns;
};

/// Ping-pong between two threads over two queues. Each sample is the mean
/// round-trip time of `latency_batch` consecutive round trips, which keeps
/// the measurement well above the steady_clock resolution.
template <typename Queue>
latency_result run_latency(std::uint64_t round_trips) {
    Queue ping(queue_capacity);
    Queue pong(queue_capacity);

    std::thread echo([&] {
        for (std::uint64_t i = 0; i < round_trips; ++i) {
            push_blocking(pong, pop_blocking(ping));
        }
    });

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(round_trips / latency_batch) + 1);
    const auto start = clock_type::now();
    std::uint64_t done = 0;
    while (done < round_trips) {
        const std::uint64_t batch = std::min<std::uint64_t>(latency_batch, round_trips - done);
        const auto batch_start = clock_type::now();
        for (std::uint64_t i = 0; i < batch; ++i) {
            push_blocking(ping, done + i);
            if (pop_blocking(pong) != done + i) {
                std::fprintf(stderr, "latency echo mismatch\n");
                std::exit(EXIT_FAILURE);
            }
        }
        const auto batch_stop = clock_type::now();
        samples.push_back(std::chrono::duration<double, std::nano>(batch_stop - batch_start).count() /
                          static_cast<double>(batch));
        done += batch;
    }
    const auto stop = clock_type::now();
    echo.join();

    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double p) {
        const auto index = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
        return samples[index];
    };
    const double total_ns = std::chrono::duration<double, std::nano>(stop - start).count();
    return {total_ns / static_cast<double>(round_trips), percentile(0.50), percentile(0.99)};
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

std::string compiler_name() {
#if defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown compiler";
#endif
}

#ifndef SPSC_BENCH_BUILD_TYPE
#define SPSC_BENCH_BUILD_TYPE "unknown"
#endif

std::uint64_t parse_count(int argc, char** argv, int index, std::uint64_t fallback) {
    if (argc > index) {
        const unsigned long long value = std::strtoull(argv[index], nullptr, 10);
        if (value > 0) {
            return value;
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t throughput_items = parse_count(argc, argv, 1, 10'000'000);
    const std::uint64_t latency_round_trips = parse_count(argc, argv, 2, 1'000'000);

    std::printf("Environment\n");
    std::printf("  compiler         : %s\n", compiler_name().c_str());
    std::printf("  build type       : %s\n", SPSC_BENCH_BUILD_TYPE);
    std::printf("  hardware threads : %u\n", std::thread::hardware_concurrency());
    std::printf("Workload\n");
    std::printf("  queue capacity   : %zu x uint64_t (both queues)\n", queue_capacity);
    std::printf("  throughput       : %llu items, median of %d runs\n",
                static_cast<unsigned long long>(throughput_items), repetitions);
    std::printf("  latency          : %llu ping-pong round trips, median of %d runs\n",
                static_cast<unsigned long long>(latency_round_trips), repetitions);
    std::printf("  waiting strategy : busy polling on try_push / try_pop\n\n");

    std::vector<double> lf_tp;
    std::vector<double> mx_tp;
    for (int r = 0; r < repetitions; ++r) {
        lf_tp.push_back(run_throughput<lockfree_queue>(throughput_items));
        mx_tp.push_back(run_throughput<locked_queue>(throughput_items));
    }

    std::vector<double> lf_mean, lf_p50, lf_p99, mx_mean, mx_p50, mx_p99;
    for (int r = 0; r < repetitions; ++r) {
        const latency_result lf = run_latency<lockfree_queue>(latency_round_trips);
        const latency_result mx = run_latency<locked_queue>(latency_round_trips);
        lf_mean.push_back(lf.mean_ns);
        lf_p50.push_back(lf.p50_ns);
        lf_p99.push_back(lf.p99_ns);
        mx_mean.push_back(mx.mean_ns);
        mx_p50.push_back(mx.p50_ns);
        mx_p99.push_back(mx.p99_ns);
    }

    const double lf_tp_m = median(lf_tp);
    const double mx_tp_m = median(mx_tp);
    std::printf("| Queue                       | Throughput (M items/s) | RTT mean (ns) | RTT p50 (ns) | RTT p99 (ns) |\n");
    std::printf("|-----------------------------|-----------------------:|--------------:|-------------:|-------------:|\n");
    std::printf("| lockfree_spsc::spsc_queue   | %22.1f | %13.1f | %12.1f | %12.1f |\n", lf_tp_m,
                median(lf_mean), median(lf_p50), median(lf_p99));
    std::printf("| std::mutex + std::queue     | %22.1f | %13.1f | %12.1f | %12.1f |\n", mx_tp_m,
                median(mx_mean), median(mx_p50), median(mx_p99));
    std::printf("\nThroughput speedup: %.1fx, mean RTT speedup: %.1fx\n", lf_tp_m / mx_tp_m,
                median(mx_mean) / median(lf_mean));
    return EXIT_SUCCESS;
}
