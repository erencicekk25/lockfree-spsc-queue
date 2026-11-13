// SPDX-License-Identifier: MIT
//
// Tests for in-place construction, move-only payloads, zero-copy access,
// exception safety and object lifetime management.

#include <lockfree_spsc/spsc_queue.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "test_common.hpp"

namespace {

using lockfree_spsc::spsc_queue;

// Counts live instances so that leaks and double destruction are detected.
struct tracked {
    static int live;

    explicit tracked(int v) : value(v) { ++live; }
    tracked(const tracked& other) : value(other.value) { ++live; }
    tracked(tracked&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
    }
    tracked& operator=(const tracked&) = default;
    tracked& operator=(tracked&& other) noexcept {
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~tracked() { --live; }

    int value;
};
int tracked::live = 0;

// Throws from its constructor on demand.
struct throwing {
    throwing(int v, bool fail) : value(v) {
        if (fail) {
            throw std::runtime_error("construction failed");
        }
    }
    int value;
};

struct point {
    point(int x_, int y_, std::string label_) : x(x_), y(y_), label(std::move(label_)) {}
    int x;
    int y;
    std::string label;
};

void test_emplace_multiple_arguments() {
    spsc_queue<point> queue(4);
    SPSC_CHECK(queue.try_emplace(1, 2, "a"));
    SPSC_CHECK(queue.try_emplace(3, 4, std::string("b")));

    const std::optional<point> first = queue.try_pop();
    SPSC_CHECK(first.has_value() && first->x == 1 && first->y == 2 && first->label == "a");
    const std::optional<point> second = queue.try_pop();
    SPSC_CHECK(second.has_value() && second->x == 3 && second->label == "b");
}

void test_emplace_full() {
    spsc_queue<point> queue(1);
    SPSC_CHECK(queue.try_emplace(1, 1, "x"));
    SPSC_CHECK(!queue.try_emplace(2, 2, "y"));
    SPSC_CHECK(queue.size() == 1);
}

void test_move_only_payload() {
    spsc_queue<std::unique_ptr<int>> queue(4);
    SPSC_CHECK(queue.try_push(std::make_unique<int>(7)));
    SPSC_CHECK(queue.try_emplace(new int(8)));

    std::unique_ptr<int> out;
    SPSC_CHECK(queue.try_pop(out));
    SPSC_CHECK(out && *out == 7);
    const std::optional<std::unique_ptr<int>> second = queue.try_pop();
    SPSC_CHECK(second.has_value() && *second && **second == 8);
}

void test_front_and_pop() {
    spsc_queue<std::string> queue(2);
    SPSC_CHECK(queue.front() == nullptr);
    SPSC_CHECK(queue.try_emplace("zero-copy"));

    std::string* head = queue.front();
    SPSC_CHECK(head != nullptr && *head == "zero-copy");
    head->append("!");
    SPSC_CHECK(*queue.front() == "zero-copy!");
    queue.pop();
    SPSC_CHECK(queue.front() == nullptr);
    SPSC_CHECK(queue.empty());
}

void test_throwing_constructor_leaves_queue_unchanged() {
    spsc_queue<throwing> queue(2);
    SPSC_CHECK(queue.try_emplace(1, false));
    SPSC_CHECK_THROWS(queue.try_emplace(2, true), std::runtime_error);
    SPSC_CHECK(queue.size() == 1);
    SPSC_CHECK(queue.try_emplace(3, false));

    const std::optional<throwing> first = queue.try_pop();
    const std::optional<throwing> second = queue.try_pop();
    SPSC_CHECK(first.has_value() && first->value == 1);
    SPSC_CHECK(second.has_value() && second->value == 3);
    SPSC_CHECK(queue.empty());
}

void test_lifetimes_balanced() {
    tracked::live = 0;
    {
        spsc_queue<tracked> queue(8);
        for (int round = 0; round < 50; ++round) {
            for (int i = 0; i < 5; ++i) {
                SPSC_CHECK(queue.try_emplace(i));
            }
            for (int i = 0; i < 5; ++i) {
                const std::optional<tracked> out = queue.try_pop();
                SPSC_CHECK(out.has_value() && out->value == i);
            }
        }
        SPSC_CHECK(tracked::live == 0);
    }
    SPSC_CHECK(tracked::live == 0);
}

void test_destructor_destroys_remaining_elements() {
    tracked::live = 0;
    {
        spsc_queue<tracked> queue(4);
        // Advance the indices first so the remaining elements wrap around.
        for (int i = 0; i < 3; ++i) {
            SPSC_CHECK(queue.try_emplace(i));
            queue.pop();
        }
        for (int i = 0; i < 4; ++i) {
            SPSC_CHECK(queue.try_emplace(i));
        }
        SPSC_CHECK(tracked::live == 4);
    }
    SPSC_CHECK(tracked::live == 0);
}

void test_try_pop_into_existing_object() {
    tracked::live = 0;
    {
        spsc_queue<tracked> queue(2);
        tracked out(0);
        SPSC_CHECK(queue.try_emplace(42));
        SPSC_CHECK(queue.try_pop(out));
        SPSC_CHECK(out.value == 42);
        SPSC_CHECK(tracked::live == 1);
    }
    SPSC_CHECK(tracked::live == 0);
}

}  // namespace

int main() {
    test_emplace_multiple_arguments();
    test_emplace_full();
    test_move_only_payload();
    test_front_and_pop();
    test_throwing_constructor_leaves_queue_unchanged();
    test_lifetimes_balanced();
    test_destructor_destroys_remaining_elements();
    test_try_pop_into_existing_object();
    return spsc_test::finish("emplace");
}
