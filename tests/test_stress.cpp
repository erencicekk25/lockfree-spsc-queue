// SPDX-License-Identifier: MIT
//
// Multithreaded stress tests: one producer and one consumer exchange
// millions of items; every item must arrive exactly once and in order.

#include <lockfree_spsc/spsc_queue.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "test_common.hpp"

namespace {

using lockfree_spsc::spsc_queue;

// A payload larger than a cache line whose fields must stay consistent,
// so torn or prematurely published writes are detected.
struct message {
    std::uint64_t sequence;
    std::uint64_t words[9];
    std::uint64_t checksum;
};

message make_message(std::uint64_t sequence) {
    message m{};
    m.sequence = sequence;
    std::uint64_t sum = sequence;
    for (std::size_t i = 0; i < 9; ++i) {
        m.words[i] = sequence * 0x9E3779B97F4A7C15ULL + i;
        sum ^= m.words[i];
    }
    m.checksum = sum;
    return m;
}

bool is_valid(const message& m) {
    std::uint64_t sum = m.sequence;
    for (std::size_t i = 0; i < 9; ++i) {
        if (m.words[i] != m.sequence * 0x9E3779B97F4A7C15ULL + i) {
            return false;
        }
        sum ^= m.words[i];
    }
    return sum == m.checksum;
}

// Spins briefly, then yields, so the test also behaves on machines with
// fewer cores than threads.
class backoff {
public:
    void pause() noexcept {
        if (++spins_ > 64) {
            std::this_thread::yield();
            spins_ = 0;
        }
    }

private:
    int spins_ = 0;
};

void stress_sequence(std::size_t capacity, std::uint64_t count) {
    spsc_queue<std::uint64_t> queue(capacity);
    std::atomic<bool> ok{true};

    std::thread consumer([&] {
        backoff wait;
        for (std::uint64_t expected = 0; expected < count; ++expected) {
            std::uint64_t value = 0;
            while (!queue.try_pop(value)) {
                wait.pause();
            }
            if (value != expected) {
                ok.store(false, std::memory_order_relaxed);
                return;
            }
        }
    });

    backoff wait;
    for (std::uint64_t i = 0; i < count; ++i) {
        while (!queue.try_push(i)) {
            wait.pause();
        }
    }
    consumer.join();

    if (!ok.load()) {
        std::fprintf(stderr, "sequence stress failed for capacity %zu\n", capacity);
    }
    SPSC_CHECK(ok.load());
    SPSC_CHECK(queue.empty());
}

void stress_large_payload(std::uint64_t count) {
    spsc_queue<message> queue(128);
    std::atomic<bool> ok{true};

    std::thread consumer([&] {
        backoff wait;
        for (std::uint64_t expected = 0; expected < count; ++expected) {
            const message* m = nullptr;
            while ((m = queue.front()) == nullptr) {
                wait.pause();
            }
            if (m->sequence != expected || !is_valid(*m)) {
                ok.store(false, std::memory_order_relaxed);
                return;
            }
            queue.pop();
        }
    });

    backoff wait;
    for (std::uint64_t i = 0; i < count; ++i) {
        while (!queue.try_push(make_message(i))) {
            wait.pause();
        }
    }
    consumer.join();
    SPSC_CHECK(ok.load());
}

void stress_owning_payload(std::uint64_t count) {
    spsc_queue<std::unique_ptr<std::string>> queue(256);
    std::atomic<bool> ok{true};

    std::thread consumer([&] {
        backoff wait;
        for (std::uint64_t expected = 0; expected < count; ++expected) {
            std::unique_ptr<std::string> value;
            while (!queue.try_pop(value)) {
                wait.pause();
            }
            if (!value || *value != std::to_string(expected)) {
                ok.store(false, std::memory_order_relaxed);
                return;
            }
        }
    });

    backoff wait;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto value = std::make_unique<std::string>(std::to_string(i));
        while (!queue.try_push(std::move(value))) {
            wait.pause();
        }
    }
    consumer.join();
    SPSC_CHECK(ok.load());
}

}  // namespace

int main() {
    // Tiny capacities maximise contention on the full/empty boundaries;
    // larger ones exercise sustained wraparound.
    stress_sequence(1, 200000);
    stress_sequence(2, 500000);
    stress_sequence(3, 500000);
    stress_sequence(64, 2000000);
    stress_sequence(1024, 5000000);
    stress_large_payload(1000000);
    stress_owning_payload(500000);
    return spsc_test::finish("stress");
}
