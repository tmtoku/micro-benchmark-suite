#include "common.hpp"
#include "perf_counter.h"
#include "perf_events.hpp"
#include "utils.hpp"

#include <sys/mman.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>

namespace memory_latency
{
    namespace
    {
        struct BenchmarkResult
        {
            std::uint64_t cycle_count = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t l1_tlb_miss_count = 0;
            std::uint64_t l2_tlb_miss_count = 0;
            std::uint64_t l2_read_request_count = 0;
        };

        void print_csv_header()
        {
            std::cout << "NumVirtualPages,PageSize,NumLogicalLoads,Cycles,L1TLBMisses,L2TLBMisses,L2ReadRequests\n";
        }

        void print_csv_row(const std::size_t num_virtual_pages, const std::size_t page_size,
                           const std::int32_t num_logical_loads, const BenchmarkResult& result)
        {
            std::cout << num_virtual_pages << "," << page_size << "," << num_logical_loads << "," << result.cycle_count
                      << "," << result.l1_tlb_miss_count << "," << result.l2_tlb_miss_count << ","
                      << result.l2_read_request_count << "\n";
        }

        [[nodiscard]] std::unique_ptr<MemoryAddress, common::MunmapDeleter> allocate_virtual_pages(
            const std::size_t num_virtual_pages)
        {
            const auto total_bytes = num_virtual_pages * common::get_page_size();
            auto* const virtual_pages = mmap(nullptr, total_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (virtual_pages == MAP_FAILED)
            {
                throw std::bad_alloc();
            }

            return {reinterpret_cast<MemoryAddress*>(virtual_pages), common::MunmapDeleter{total_bytes}};
        }

        [[nodiscard]] MemoryAddress* align_up(MemoryAddress* const addr, const std::size_t alignment_bytes)
        {
            if (alignment_bytes == 0)
            {
                throw std::invalid_argument("`alignment_bytes` must be positive.");
            }

            const auto misaligned_bytes = reinterpret_cast<std::uintptr_t>(addr) % alignment_bytes;
            if (misaligned_bytes == 0)
            {
                return addr;
            }

            const auto padding_bytes = alignment_bytes - misaligned_bytes;
            return reinterpret_cast<MemoryAddress*>(reinterpret_cast<unsigned char*>(addr) + padding_bytes);
        }

        void map_virtual_pages(MemoryAddress* const virtual_pages, const std::size_t num_strided_pages,
                               const std::size_t page_stride)
        {
            const auto page_size = common::get_page_size();
            const auto page_stride_bytes = page_stride * page_size;

            for (std::size_t page_index = 0; page_index < num_strided_pages; ++page_index)
            {
                const auto virtual_page_offset = page_stride_bytes * page_index;
                auto* const virtual_page_addr = reinterpret_cast<unsigned char*>(virtual_pages) + virtual_page_offset;

                const auto* const mapped_page = mmap(virtual_page_addr, page_size, PROT_READ | PROT_WRITE,
                                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                if (mapped_page == MAP_FAILED)
                {
                    throw std::bad_alloc();
                }
            }
        }

        [[nodiscard]] MemoryAddress* generate_random_pointer_chasing(MemoryAddress* const virtual_pages,
                                                                     const std::size_t num_strided_pages,
                                                                     const std::size_t page_stride,
                                                                     const std::size_t num_cache_sets,
                                                                     const std::uint64_t seed)
        {
            const auto get_element_location = [virtual_pages, page_stride_bytes = page_stride * common::get_page_size(),
                                               cache_line_bytes = common::get_cache_line_bytes(),
                                               num_cache_sets](const std::size_t virtual_page_index) {
                const auto virtual_page_offset = page_stride_bytes * virtual_page_index;
                auto* const virtual_page_addr = reinterpret_cast<unsigned char*>(virtual_pages) + virtual_page_offset;

                // Use only the odd cache line sets for the pointer chain
                const auto set_number = virtual_page_index % (num_cache_sets / 2);
                const auto odd_set_index = (2 * set_number) + 1;
                const auto cache_line_offset = odd_set_index * cache_line_bytes;

                return reinterpret_cast<MemoryAddress*>(virtual_page_addr + cache_line_offset);
            };

            return detail::generate_random_pointer_chasing(num_strided_pages, get_element_location, seed);
        }

        template <std::int32_t NUM_LOGICAL_LOADS>
        std::optional<BenchmarkResult> measure_pointer_chasing(MemoryAddress* const start_ptr,
                                                               perf_counter& cycle_counter)
        {
            constexpr auto NUM_TRIALS = std::int32_t{10};
            constexpr auto NUM_WARMUPS = std::int32_t{3};

            const auto open_counter = [](const char* name, const std::int32_t group_fd) {
                const auto counter = perf_counter_open_by_name(name, group_fd);
                if (!perf_counter_is_valid(&counter))
                {
                    std::cerr << "Error: Failed to open performance counter for event '" << name << "'.\n";
                }
                return counter;
            };

            const auto close_counter = [](perf_counter* const counter) {
                if (perf_counter_is_valid(counter))
                {
                    perf_counter_close(counter);
                }
            };

            const auto group_fd = cycle_counter.fd;

            BenchmarkResult result;

            auto l1_tlb_miss_counter = open_counter(perf_events::L1_TLB_MISS, group_fd);
            auto l2_tlb_miss_counter = open_counter(perf_events::L2_TLB_MISS, group_fd);
            auto l2_read_request_counter = open_counter(perf_events::L2_READ_REQUEST, group_fd);

            if (!perf_counter_is_valid(&l1_tlb_miss_counter) || !perf_counter_is_valid(&l2_tlb_miss_counter) ||
                !perf_counter_is_valid(&l2_read_request_counter))
            {
                close_counter(&l1_tlb_miss_counter);
                close_counter(&l2_tlb_miss_counter);
                close_counter(&l2_read_request_counter);
                return std::nullopt;
            }

            perf_counter_enable(&cycle_counter);

            for (std::int32_t i = 0; i < NUM_WARMUPS + NUM_TRIALS; ++i)
            {
                const auto start_l1_tlb = perf_counter_read(&l1_tlb_miss_counter);
                const auto start_l2_tlb = perf_counter_read(&l2_tlb_miss_counter);
                const auto start_l2_read_requests = perf_counter_read(&l2_read_request_counter);
                const auto start_cycles = perf_counter_read(&cycle_counter);

                const auto* const last_ptr = walk_pointer_chain<NUM_LOGICAL_LOADS>(start_ptr);
                __asm__ volatile("" ::"r"(last_ptr));

                const auto end_cycles = perf_counter_read(&cycle_counter);
                const auto end_l2_read_requests = perf_counter_read(&l2_read_request_counter);
                const auto end_l2_tlb = perf_counter_read(&l2_tlb_miss_counter);
                const auto end_l1_tlb = perf_counter_read(&l1_tlb_miss_counter);

                const auto cycles = end_cycles - start_cycles;
                if (i >= NUM_WARMUPS && cycles < result.cycle_count)
                {
                    result.cycle_count = cycles;
                    result.l1_tlb_miss_count = end_l1_tlb - start_l1_tlb;
                    result.l2_tlb_miss_count = end_l2_tlb - start_l2_tlb;
                    result.l2_read_request_count = end_l2_read_requests - start_l2_read_requests;
                }
            }

            perf_counter_disable(&cycle_counter);
            close_counter(&l2_read_request_counter);
            close_counter(&l2_tlb_miss_counter);
            close_counter(&l1_tlb_miss_counter);

            return result;
        }

        void run_benchmark(const std::size_t num_virtual_pages)
        {
            constexpr auto NUM_LOGICAL_LOADS = std::int32_t{1'000'000};
            constexpr auto NUM_SEEDS = std::size_t{5};
            constexpr auto PTE_BYTES = std::size_t{8};

            const auto page_size = common::get_page_size();
            const auto cache_line_bytes = common::get_cache_line_bytes();
            const auto num_l1d_ways = common::get_l1d_cache_ways();

            const auto num_cache_sets = common::get_cache_size(0) / (num_l1d_ways * cache_line_bytes);
            const auto num_selectable_sets = std::min(num_cache_sets, page_size / cache_line_bytes);
            if (num_selectable_sets < 2 || (num_selectable_sets & (num_selectable_sets - 1)) != 0)
            {
                throw std::runtime_error(
                    "The number of selectable L1D cache sets must be a power of 2 and at least 2.");
            }

            const auto num_pointer_chain_lines = (num_selectable_sets / 2) * num_l1d_ways;
            if (num_virtual_pages == 0 || num_virtual_pages > num_pointer_chain_lines)
            {
                throw std::invalid_argument(
                    "`num_virtual_pages` must be positive and not exceed the L1D cache lines reserved for the pointer "
                    "chain.");
            }

            // Place the PTEs two cache lines apart in the page table
            const auto page_stride = (2 * cache_line_bytes) / PTE_BYTES;
            const auto page_stride_bytes = page_stride * page_size;

            // Reserve an extra page stride for aligning the start address
            const auto virtual_pages = allocate_virtual_pages((num_virtual_pages * page_stride) + page_stride);
            auto* const aligned_virtual_pages = align_up(virtual_pages.get(), page_stride_bytes);

            // Map one page per page stride, leaving the rest unmapped
            map_virtual_pages(aligned_virtual_pages, num_virtual_pages, page_stride);

            auto cycle_counter = perf_counter_open_by_name(perf_events::CYCLES, -1);
            if (!perf_counter_is_valid(&cycle_counter))
            {
                std::cerr << "Error: Failed to open performance counter for event '" << perf_events::CYCLES << "'.\n";
                return;
            }

            std::array<BenchmarkResult, NUM_SEEDS> seed_results;
            for (std::size_t i = 0; i < NUM_SEEDS; ++i)
            {
                // Place a pointer in each mapped page, using only the odd L1D cache sets
                auto* const start_ptr = generate_random_pointer_chasing(aligned_virtual_pages, num_virtual_pages,
                                                                        page_stride, num_selectable_sets, i);
                const auto result = measure_pointer_chasing<NUM_LOGICAL_LOADS>(start_ptr, cycle_counter);
                if (!result)
                {
                    perf_counter_close(&cycle_counter);
                    return;
                }
                seed_results[i] = result.value();
            }

            perf_counter_close(&cycle_counter);

            std::nth_element(seed_results.begin(), seed_results.begin() + (NUM_SEEDS / 2), seed_results.end(),
                             [](const auto& a, const auto& b) { return a.cycle_count < b.cycle_count; });
            const auto& median_result = seed_results[NUM_SEEDS / 2];
            print_csv_row(num_virtual_pages, page_size, NUM_LOGICAL_LOADS, median_result);
        }
    }  // namespace
}  // namespace memory_latency

int main()
{
    using namespace memory_latency;

    try
    {
        constexpr auto MIN_VIRTUAL_PAGES = std::size_t{8};
        constexpr auto MAX_VIRTUAL_PAGES = std::size_t{256};
        constexpr auto NUM_BINS = std::size_t{8};

        static_assert(MIN_VIRTUAL_PAGES % NUM_BINS == 0, "`MIN_VIRTUAL_PAGES` must be a multiple of `NUM_BINS`");

        print_csv_header();

        for (auto start_virtual_pages = MIN_VIRTUAL_PAGES, step = MIN_VIRTUAL_PAGES / NUM_BINS;
             start_virtual_pages <= MAX_VIRTUAL_PAGES; start_virtual_pages *= 2, step *= 2)
        {
            const auto end_virtual_pages = 2 * start_virtual_pages;
            for (auto num_virtual_pages = start_virtual_pages;
                 num_virtual_pages <= MAX_VIRTUAL_PAGES && num_virtual_pages < end_virtual_pages;
                 num_virtual_pages += step)
            {
                run_benchmark(num_virtual_pages);
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
