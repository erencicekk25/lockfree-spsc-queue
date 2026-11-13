// SPDX-License-Identifier: MIT
//
// Minimal consumer translation unit: the public header must be
// self-contained and compile on its own as C++17.

#include <lockfree_spsc/spsc_queue.hpp>

int main() {
    lockfree_spsc::spsc_queue<int> queue(8);
    return queue.capacity() == 8 ? 0 : 1;
}
