find_package(Threads REQUIRED)

function(spsc_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE lockfree_spsc::spsc_queue spsc_project_warnings ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

spsc_add_test(test_compile)
spsc_add_test(test_layout)
spsc_add_test(test_push_pop Threads::Threads)
