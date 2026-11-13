// SPDX-License-Identifier: MIT
//
// Tests for the core layout, capacity contract and RAII behaviour.

#include <lockfree_spsc/spsc_queue.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "test_common.hpp"

namespace {

using lockfree_spsc::cache_line_size;
using lockfree_spsc::spsc_queue;

// A type that cannot be default constructed: the queue must never create
// elements it was not asked to create.
struct no_default {
    explicit no_default(int v) : value(v) {}
    int value;
};

// An over-aligned type: the slot storage must honour its alignment.
struct alignas(128) over_aligned {
    unsigned char bytes[128];
};

// Compile-time properties of the queue type.
static_assert(alignof(spsc_queue<int>) >= cache_line_size,
              "queue must be cache-line aligned");
static_assert(sizeof(spsc_queue<int>) == 3 * cache_line_size,
              "producer, consumer and config state must each use one cache line "
              "(the README layout diagram documents 192 bytes)");
static_assert(!std::is_copy_constructible_v<spsc_queue<int>>);
static_assert(!std::is_copy_assignable_v<spsc_queue<int>>);
static_assert(!std::is_move_constructible_v<spsc_queue<int>>);
static_assert(!std::is_move_assignable_v<spsc_queue<int>>);
static_assert(!std::is_convertible_v<std::size_t, spsc_queue<int>>,
              "capacity constructor must be explicit");

void test_capacity_contract() {
    const spsc_queue<int> one(1);
    SPSC_CHECK(one.capacity() == 1);

    const spsc_queue<int> many(1000);
    SPSC_CHECK(many.capacity() == 1000);
    SPSC_CHECK(many.size() == 0);
    SPSC_CHECK(many.empty());
}

void test_invalid_capacity() {
    SPSC_CHECK_THROWS(spsc_queue<int>(0), std::invalid_argument);
    SPSC_CHECK_THROWS(spsc_queue<int>(std::numeric_limits<std::size_t>::max()),
                      std::length_error);
}

void test_non_default_constructible() {
    const spsc_queue<no_default> queue(16);
    SPSC_CHECK(queue.capacity() == 16);
    SPSC_CHECK(queue.empty());
}

void test_non_trivial_type() {
    const spsc_queue<std::string> queue(4);
    SPSC_CHECK(queue.capacity() == 4);
    SPSC_CHECK(queue.empty());
}

void test_over_aligned_type() {
    const spsc_queue<over_aligned> queue(4);
    SPSC_CHECK(queue.capacity() == 4);
    SPSC_CHECK(reinterpret_cast<std::uintptr_t>(&queue) % cache_line_size == 0);
}

void test_heap_alignment() {
    auto* queue = new spsc_queue<int>(32);
    SPSC_CHECK(reinterpret_cast<std::uintptr_t>(queue) % cache_line_size == 0);
    delete queue;
}

}  // namespace

int main() {
    test_capacity_contract();
    test_invalid_capacity();
    test_non_default_constructible();
    test_non_trivial_type();
    test_over_aligned_type();
    test_heap_alignment();
    return spsc_test::finish("layout");
}
