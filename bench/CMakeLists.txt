find_package(Threads REQUIRED)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(WARNING "Benchmarks are meaningful only in Release builds "
                    "(current CMAKE_BUILD_TYPE: '${CMAKE_BUILD_TYPE}')")
endif()

add_executable(bench_queue bench_queue.cpp)
target_link_libraries(bench_queue PRIVATE
    lockfree_spsc::spsc_queue spsc_project_warnings Threads::Threads)
target_compile_definitions(bench_queue PRIVATE
    SPSC_BENCH_BUILD_TYPE="$<CONFIG>")
