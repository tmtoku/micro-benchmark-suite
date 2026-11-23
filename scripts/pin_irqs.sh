#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <cpu-list>" >&2
    exit 1
fi

# Pin IRQs (interrupt requests) to the given CPUs.
for f in /proc/irq/*/smp_affinity_list; do
    echo "$1" | sudo tee "$f" > /dev/null 2>&1 || true
done
