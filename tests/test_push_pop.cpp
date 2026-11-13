// SPDX-License-Identifier: MIT
//
// Tests for try_push / try_pop: full/empty reporting, wraparound,
// capacity boundaries and a two-thread smoke test.

#include <lockfree_spsc/spsc_queue.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "test_common.hpp"

namespace {

using lockfree_spsc::spsc_queue;

void test_empty_queue() {
    spsc_queue<int> queue(4);
    int out = -1;
    SPSC_CHECK(!queue.try_pop(out));
    SPSC_CHECK(out == -1);
    SPSC_CHECK(!queue.try_pop().has_value());
    SPSC_CHECK(queue.empty());
}

void test_fifo_order() {
    spsc_queue<int> queue(8);
    for (int i = 0; i < 5; ++i) {
        SPSC_CHECK(queue.try_push(i));
    }
    SPSC_CHECK(queue.size() == 5);
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        SPSC_CHECK(queue.try_pop(out));
        SPSC_CHECK(out == i);
    }
    SPSC_CHECK(queue.empty());
}

void test_full_queue() {
    spsc_queue<int> queue(3);
    SPSC_CHECK(queue.try_push(1));
    SPSC_CHECK(queue.try_push(2));
    SPSC_CHECK(queue.try_push(3));
    SPSC_CHECK(queue.size() == queue.capacity());
    SPSC_CHECK(!queue.try_push(4));

    int out = 0;
    SPSC_CHECK(queue.try_pop(out));
    SPSC_CHECK(out == 1);
    SPSC_CHECK(queue.try_push(4));
    SPSC_CHECK(!queue.try_push(5));
}

void test_capacity_one() {
    spsc_queue<int> queue(1);
    for (int i = 0; i < 10; ++i) {
        SPSC_CHECK(queue.try_push(i));
        SPSC_CHECK(!queue.try_push(i + 100));
        const std::optional<int> out = queue.try_pop();
        SPSC_CHECK(out.has_value() && *out == i);
        SPSC_CHECK(!queue.try_pop().has_value());
    }
}

void test_wraparound() {
    constexpr std::size_t capacity = 7;
    spsc_queue<std::size_t> queue(capacity);
    std::size_t next_push = 0;
    std::size_t next_pop = 0;
    // Interleave pushes and pops with varying batch sizes so the indices
    // cross the end of the ring at many different offsets.
    for (std::size_t round = 0; round < 1000; ++round) {
        const std::size_t batch = round % capacity + 1;
        for (std::size_t i = 0; i < batch; ++i) {
            SPSC_CHECK(queue.try_push(next_push));
            ++next_push;
        }
        SPSC_CHECK(queue.size() == batch);
        for (std::size_t i = 0; i < batch; ++i) {
            std::size_t out = 0;
            SPSC_CHECK(queue.try_pop(out));
            SPSC_CHECK(out == next_pop);
            ++next_pop;
        }
        SPSC_CHECK(queue.empty());
    }
}

void test_copy_and_move_push() {
    spsc_queue<std::string> queue(4);
    const std::string copied = "copied";
    std::string moved = "moved-string-that-is-long-enough-to-heap-allocate";
    SPSC_CHECK(queue.try_push(copied));
    SPSC_CHECK(queue.try_push(std::move(moved)));
    SPSC_CHECK(copied == "copied");

    std::string out;
    SPSC_CHECK(queue.try_pop(out));
    SPSC_CHECK(out == "copied");
    const std::optional<std::string> second = queue.try_pop();
    SPSC_CHECK(second.has_value() &&
               *second == "moved-string-that-is-long-enough-to-heap-allocate");
}

void test_failed_push_keeps_value() {
    spsc_queue<std::string> queue(1);
    SPSC_CHECK(queue.try_push(std::string("first")));
    std::string value = "second";
    SPSC_CHECK(!queue.try_push(std::move(value)));
    SPSC_CHECK(value == "second");
}

void test_two_thread_smoke() {
    constexpr int count = 200000;
    spsc_queue<int> queue(64);
    std::vector<int> received;
    received.reserve(count);

    std::thread consumer([&] {
        while (received.size() < static_cast<std::size_t>(count)) {
            int value = 0;
            if (queue.try_pop(value)) {
                received.push_back(value);
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (int i = 0; i < count; ++i) {
        while (!queue.try_push(i)) {
            std::this_thread::yield();
        }
    }
    consumer.join();

    bool in_order = received.size() == static_cast<std::size_t>(count);
    for (std::size_t i = 0; in_order && i < received.size(); ++i) {
        in_order = received[i] == static_cast<int>(i);
    }
    SPSC_CHECK(in_order);
    SPSC_CHECK(queue.empty());
}

}  // namespace

int main() {
    test_empty_queue();
    test_fifo_order();
    test_full_queue();
    test_capacity_one();
    test_wraparound();
    test_copy_and_move_push();
    test_failed_push_keeps_value();
    test_two_thread_smoke();
    return spsc_test::finish("push_pop");
}
