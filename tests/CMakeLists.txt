find_package(Threads REQUIRED)

function(spsc_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE lockfree_spsc::spsc_queue spsc_project_warnings ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

spsc_add_test(test_compile)
spsc_add_test(test_layout)
spsc_add_test(test_push_pop Threads::Threads)
spsc_add_test(test_emplace)
spsc_add_test(test_stress Threads::Threads)
set_tests_properties(test_stress PROPERTIES TIMEOUT 300)

# The README quick-start example must always compile and run.
add_executable(example_quick_start ${PROJECT_SOURCE_DIR}/examples/quick_start.cpp)
target_link_libraries(example_quick_start PRIVATE
    lockfree_spsc::spsc_queue spsc_project_warnings Threads::Threads)
add_test(NAME example_quick_start COMMAND example_quick_start)
