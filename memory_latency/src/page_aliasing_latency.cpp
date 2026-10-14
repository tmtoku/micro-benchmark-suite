#include "common.hpp"
#include "perf_counter.h"
#include "perf_events.hpp"
#include "utils.hpp"

#include <linux/memfd.h>
#include <sys/mman.h>
#include <unistd.h>
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
            std::uint64_t l1d_miss_count = 0;
            std::uint64_t l2_read_request_count = 0;
            std::uint64_t l2_read_request_hit_count = 0;
        };

        void print_csv_header()
        {
            std::cout << "BufferSize,PageSize,NumAliases,NumLogicalLoads,Cycles,L1DMisses,L2ReadRequests,L2ReadRequestHits\n";
        }

        void print_csv_row(const std::size_t buffer_size, const std::size_t page_size, const std::size_t num_aliases,
                           const std::int32_t num_logical_loads, const BenchmarkResult& result)
        {
            std::cout << buffer_size << "," << page_size << "," << num_aliases << "," << num_logical_loads << ","
                      << result.cycle_count << "," << result.l1d_miss_count << "," << result.l2_read_request_count
                      << "," << result.l2_read_request_hit_count << "\n";
        }

        [[nodiscard]] std::int32_t allocate_backing_pages(const std::size_t num_backing_pages)
        {
            const auto total_bytes = num_backing_pages * common::get_page_size();

            const auto fd = static_cast<std::int32_t>(memfd_create("backing_pages", MFD_CLOEXEC));
            if (fd == -1)
            {
                throw std::runtime_error("Failed to create the backing pages.");
            }

            if (ftruncate(fd, static_cast<off_t>(total_bytes)) == -1)
            {
                close(fd);
                throw std::runtime_error("Failed to extend the backing pages.");
            }

            return fd;
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

        void map_virtual_pages(const std::int32_t backing_fd, MemoryAddress* const virtual_pages,
                               const std::size_t num_virtual_pages, const std::size_t num_aliases)
        {
            const auto page_size = common::get_page_size();

            for (std::size_t virtual_page_index = 0; virtual_page_index < num_virtual_pages; ++virtual_page_index)
            {
                const auto virtual_page_offset = virtual_page_index * page_size;
                auto* const virtual_page_addr = reinterpret_cast<unsigned char*>(virtual_pages) + virtual_page_offset;

                const auto backing_page_index = virtual_page_index / num_aliases;
                const auto backing_page_offset = static_cast<off_t>(backing_page_index * page_size);

                auto* const mapped_page = mmap(virtual_page_addr, page_size, PROT_READ | PROT_WRITE,
                                               MAP_SHARED | MAP_FIXED, backing_fd, backing_page_offset);
                if (mapped_page == MAP_FAILED)
                {
                    throw std::bad_alloc();
                }
            }
        }

        [[nodiscard]] MemoryAddress* generate_random_pointer_chasing(MemoryAddress* const virtual_pages,
                                                                     const std::size_t num_elements,
                                                                     const std::size_t num_aliases,
                                                                     const std::uint64_t seed)
        {
            const auto get_element_location = [virtual_pages, num_aliases](const std::size_t index) {
                const auto page_size = common::get_page_size();
                const auto num_slots_per_page = page_size / sizeof(MemoryAddress);

                const auto backing_page_index = index / num_slots_per_page;
                const auto backing_slot_index = index % num_slots_per_page;
                const auto alias_index = backing_slot_index % num_aliases;

                const auto virtual_page_index = (backing_page_index * num_aliases) + alias_index;
                const auto virtual_page_offset = virtual_page_index * page_size;
                auto* const virtual_page_addr = reinterpret_cast<unsigned char*>(virtual_pages) + virtual_page_offset;

                const auto backing_slot_offset = backing_slot_index * sizeof(MemoryAddress);

                return reinterpret_cast<MemoryAddress*>(virtual_page_addr + backing_slot_offset);
            };

            return detail::generate_random_pointer_chasing(num_elements, get_element_location, seed);
        }

        template <std::int32_t NUM_LOGICAL_LOADS>
        std::optional<BenchmarkResult> measure_pointer_chasing(MemoryAddress* const start_ptr,
                                                               perf_counter& cycle_counter)
        {
            constexpr auto NUM_TRIALS = std::int32_t{10};
            constexpr auto NUM_WARMUPS = std::int32_t{3};

            const auto open_counter = [](const char* const name, const std::int32_t group_fd) {
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

            auto l1d_miss_counter = open_counter(perf_events::L1D_MISS, group_fd);
            auto l2_read_request_counter = open_counter(perf_events::L2_READ_REQUEST, group_fd);
            auto l2_read_request_hit_counter = open_counter(perf_events::L2_READ_REQUEST_HIT, group_fd);

            if (!perf_counter_is_valid(&l1d_miss_counter) || !perf_counter_is_valid(&l2_read_request_counter) ||
                !perf_counter_is_valid(&l2_read_request_hit_counter))
            {
                close_counter(&l1d_miss_counter);
                close_counter(&l2_read_request_counter);
                close_counter(&l2_read_request_hit_counter);
                return std::nullopt;
            }

            perf_counter_enable(&cycle_counter);

            for (std::int32_t i = 0; i < NUM_WARMUPS + NUM_TRIALS; ++i)
            {
                const auto start_l1d = perf_counter_read(&l1d_miss_counter);
                const auto start_l2_read_requests = perf_counter_read(&l2_read_request_counter);
                const auto start_l2_read_request_hits = perf_counter_read(&l2_read_request_hit_counter);
                const auto start_cycles = perf_counter_read(&cycle_counter);

                const auto* const last_ptr = walk_pointer_chain<NUM_LOGICAL_LOADS>(start_ptr);
                __asm__ volatile("" ::"r"(last_ptr));

                const auto end_cycles = perf_counter_read(&cycle_counter);
                const auto end_l2_read_request_hits = perf_counter_read(&l2_read_request_hit_counter);
                const auto end_l2_read_requests = perf_counter_read(&l2_read_request_counter);
                const auto end_l1d = perf_counter_read(&l1d_miss_counter);

                const auto cycles = end_cycles - start_cycles;
                if (i >= NUM_WARMUPS && cycles < result.cycle_count)
                {
                    result.cycle_count = cycles;
                    result.l1d_miss_count = end_l1d - start_l1d;
                    result.l2_read_request_count = end_l2_read_requests - start_l2_read_requests;
                    result.l2_read_request_hit_count = end_l2_read_request_hits - start_l2_read_request_hits;
                }
            }

            perf_counter_disable(&cycle_counter);
            close_counter(&l2_read_request_hit_counter);
            close_counter(&l2_read_request_counter);
            close_counter(&l1d_miss_counter);

            return result;
        }

        void run_benchmark(const std::size_t buffer_size_in_bytes, const std::size_t num_aliases)
        {
            constexpr auto NUM_LOGICAL_LOADS = std::int32_t{1'000'000};
            constexpr auto NUM_SEEDS = std::size_t{5};

            const auto page_size = common::get_page_size();
            if (buffer_size_in_bytes == 0 || buffer_size_in_bytes % page_size != 0)
            {
                throw std::invalid_argument("`buffer_size_in_bytes` must be a positive multiple of the page size.");
            }

            const auto num_elements = buffer_size_in_bytes / sizeof(MemoryAddress);
            const auto num_backing_pages = buffer_size_in_bytes / page_size;

            const auto virtual_pages = [num_backing_pages, num_aliases] {
                const auto num_virtual_pages = num_backing_pages * num_aliases;
                const auto backing_fd = allocate_backing_pages(num_backing_pages);

                try
                {
                    auto pages = allocate_virtual_pages(num_virtual_pages);
                    map_virtual_pages(backing_fd, pages.get(), num_virtual_pages, num_aliases);
                    close(backing_fd);
                    return pages;
                }
                catch (...)
                {
                    close(backing_fd);
                    throw;
                }
            }();

            auto cycle_counter = perf_counter_open_by_name(perf_events::CYCLES, -1);
            if (!perf_counter_is_valid(&cycle_counter))
            {
                std::cerr << "Error: Failed to open performance counter for event '" << perf_events::CYCLES << "'.\n";
                return;
            }

            std::array<BenchmarkResult, NUM_SEEDS> seed_results;
            try
            {
                for (std::size_t i = 0; i < NUM_SEEDS; ++i)
                {
                    auto* const start_ptr =
                        generate_random_pointer_chasing(virtual_pages.get(), num_elements, num_aliases, i);
                    const auto result = measure_pointer_chasing<NUM_LOGICAL_LOADS>(start_ptr, cycle_counter);
                    if (!result)
                    {
                        perf_counter_close(&cycle_counter);
                        return;
                    }
                    seed_results[i] = result.value();
                }
            }
            catch (...)
            {
                perf_counter_close(&cycle_counter);
                throw;
            }

            perf_counter_close(&cycle_counter);

            std::nth_element(seed_results.begin(), seed_results.begin() + (NUM_SEEDS / 2), seed_results.end(),
                             [](const auto& a, const auto& b) { return a.cycle_count < b.cycle_count; });
            const auto& median_result = seed_results[NUM_SEEDS / 2];
            print_csv_row(buffer_size_in_bytes, page_size, num_aliases, NUM_LOGICAL_LOADS, median_result);
        }
    }  // namespace
}  // namespace memory_latency

int main()
{
    using namespace memory_latency;

    try
    {
        constexpr auto BUFFER_SIZE = 4 * common::KiB;
        constexpr auto MIN_ALIASES = std::size_t{1};
        constexpr auto MAX_ALIASES = std::size_t{32};

        print_csv_header();

        for (auto num_aliases = MIN_ALIASES; num_aliases <= MAX_ALIASES; num_aliases *= 2)
        {
            run_benchmark(BUFFER_SIZE, num_aliases);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
