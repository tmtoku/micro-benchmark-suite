add_executable(memory_latency
    src/main.cpp
    src/utils.hpp
)

target_link_libraries(memory_latency PRIVATE
    micro_benchmark_common
    perf_counter::perf_counter
)
