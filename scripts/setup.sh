#!/usr/bin/env bash
set -euo pipefail

# Reserve 512 huge pages for benchmarks that allocate buffers with MAP_HUGETLB.
echo 512 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null

# Prevent 4 KiB pages from being promoted to huge pages automatically.
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled > /dev/null

# Fix the CPU frequency at the base clock.
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost > /dev/null
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null

# Disable the NMI watchdog, which occupies a hardware performance counter.
echo 0 | sudo tee /proc/sys/kernel/nmi_watchdog > /dev/null

# Disable ASLR.
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space > /dev/null
