// SPDX-License-Identifier: MIT
//
// Quick-start example used by the README. It is built and run as part of
// the test suite so the documented code always compiles.

#include <lockfree_spsc/spsc_queue.hpp>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>

int main() {
    // A queue that can hold up to 1024 integers.
    lockfree_spsc::spsc_queue<int> queue(1024);

    std::thread producer([&queue] {
        for (int i = 0; i < 100; ++i) {
            while (!queue.try_push(i)) {
                // Queue is full: spin, yield or do other work.
            }
        }
    });

    long long sum = 0;
    for (int received = 0; received < 100;) {
        int value = 0;
        if (queue.try_pop(value)) {
            sum += value;
            ++received;
        }
    }
    producer.join();
    std::printf("sum = %lld\n", sum);  // 4950

    // In-place construction and move-only payloads.
    lockfree_spsc::spsc_queue<std::unique_ptr<std::string>> messages(16);
    if (!messages.try_emplace(std::make_unique<std::string>("hello"))) {
        return 1;
    }
    std::optional<std::unique_ptr<std::string>> message = messages.try_pop();
    if (message) {
        std::printf("%s\n", (*message)->c_str());
    }

    // Zero-copy consumption: inspect in place, then pop.
    lockfree_spsc::spsc_queue<std::string> lines(4);
    if (!lines.try_emplace(std::size_t{5}, 'x')) {
        return 1;
    }
    if (const std::string* line = lines.front()) {
        std::printf("%s\n", line->c_str());  // xxxxx
        lines.pop();
    }

    return sum == 4950 ? 0 : 1;
}
