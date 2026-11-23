# Micro-benchmark Suite

A suite of microbenchmarks for measuring the empirical peak performance of processors and memory subsystems.

## Overview

Each benchmark demonstrates the empirical peak performance of a specific hardware resource.

## Requirements

- x86-64 CPU
- Linux kernel with `perf_event_open` support
- C++17-capable compiler (GCC or Clang)
- CMake >= 3.10
- OpenMP
- [`perf-counter`](https://github.com/tmtoku/perf-counter) (included as a submodule)
- [`libpfm4`](https://sourceforge.net/p/perfmon2/libpfm4/ci/master/tree/) for named PMU event support

## Build

```sh
git clone --recursive https://github.com/tmtoku/micro-benchmark-suite
cd micro-benchmark-suite
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To target a specific microarchitecture (defaults to `native`):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMICRO_BENCHMARK_SUITE_ARCH=znver2
```

## Setup

Run the setup script before benchmarking (requires `sudo`):

```sh
./scripts/setup.sh
```

The script does the following:

- Reserve 512 huge pages for benchmarks that allocate buffers with `MAP_HUGETLB`
- Prevent 4 KiB pages from being promoted to huge pages automatically
- Fix the CPU frequency at the base clock
- Disable the NMI watchdog, which occupies a hardware performance counter
- Disable ASLR

## Running

### Single-threaded benchmarks

Pin IRQs (interrupt requests) to CPU 0 and run the benchmark on CPU 7:

```sh
./scripts/pin_irqs.sh 0
sudo chrt -f 99 taskset -c 7 ./build/<path/to/benchmark>
```

### Multi-threaded benchmarks

Distribute IRQs across all CPUs (e.g. 0-7) and run the benchmark with each thread bound to its own physical core:

```sh
./scripts/pin_irqs.sh 0-7
sudo chrt -f 99 env OMP_PROC_BIND=true OMP_PLACES=cores ./build/<path/to/benchmark>
```

## Benchmarks

- **memory_latency**
  - **cache_latency** — load latency at each cache level, measured via random pointer chasing to minimize hardware prefetcher effects
  - **tlb_latency** — L1 and L2 TLB miss latency, measured with all data kept in the L1D cache
  - **virtual_address_aliasing_latency** — load latency with different numbers of virtual aliases per physical page
- **memory_throughput** — load throughput at each cache level, measured across multiple thread counts
- **integer_division** — `idivq` latency with different dividend or quotient bit lengths

## License

See [LICENSE](LICENSE).
