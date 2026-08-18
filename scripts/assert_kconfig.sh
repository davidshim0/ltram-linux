#!/bin/bash
# assert_kconfig.sh — fail the build if the LtRAM kernel loses a config option the
# design depends on.
#
# WHY THIS EXISTS
#   M1 (see design doc D3) makes the linear map read-only for resident LtRAM pages so
#   that kernel writes fault instead of vanishing. On arm64 that requires the linear map
#   to be PAGE-granular rather than block-mapped, which is what
#   CONFIG_RODATA_FULL_DEFAULT_ENABLED buys. If that option is lost in a `make oldconfig`,
#   M1 does not fail loudly -- set_memory_ro()/set_direct_map_*() can return SUCCESS while
#   doing nothing, and the protection silently disappears.
#
#   That is the same failure shape as the NOR bus eye constraint, which sat in the FPGA
#   build for weeks reporting clean because its pin selector matched zero objects and
#   Vivado does not error on an empty match. Guard the precondition, loudly.
#
# ARCH MATTERS
#   configs/linux-config in this repo is CONFIG_X86_64 -- the QEMU dev target. On x86 the
#   direct map splits large pages on demand, so set_memory_ro() "just works" and QEMU will
#   give a FALSE PASS for M1. M1 can only be validated on arm64. This script therefore
#   applies the arm64 requirements only to an arm64 config, and says so otherwise.
#
# Usage:
#   ./scripts/assert_kconfig.sh                     # checks the RUNNING kernel
#   ./scripts/assert_kconfig.sh path/to/.config     # checks a config file
set -u
CFG=${1:-/boot/config-$(uname -r)}
[ -r "$CFG" ] || { echo "assert_kconfig: cannot read $CFG"; exit 2; }
echo "=== asserting kernel config: $CFG ==="

fail=0
want_y() {  # want_y SYMBOL "why it matters"
    if grep -q "^CONFIG_$1=y" "$CFG"; then
        printf "  %-42s y    OK\n" "CONFIG_$1"
    else
        state=$(grep -E "^(CONFIG_$1=|# CONFIG_$1 )" "$CFG" || echo "(absent)")
        printf "  %-42s FAIL  %s\n" "CONFIG_$1" "$state"
        printf "      ^ %s\n" "$2"
        fail=1
    fi
}
want_y_warn() {
    if grep -q "^CONFIG_$1=y" "$CFG"; then
        printf "  %-42s y    OK\n" "CONFIG_$1"
    else
        printf "  %-42s warn  %s\n" "CONFIG_$1" "$2"
    fi
}

if grep -q "^CONFIG_ARM64=y" "$CFG"; then
    echo "  [arm64 target — full LtRAM requirements apply]"
    want_y RODATA_FULL_DEFAULT_ENABLED \
        "M1 needs a PAGE-granular linear map. Without this it is block-mapped (2MB/1GB) and set_memory_ro() on a linear-map page cannot work -- possibly WITHOUT reporting an error."
    want_y ARCH_HAS_SET_DIRECT_MAP \
        "provides set_direct_map_*(), the linear-map permission API M1 is built on."
    want_y_warn PTDUMP_DEBUGFS \
        "not required, but without it /sys/kernel/debug/kernel_page_tables does not exist and linear-map granularity cannot be OBSERVED, only inferred from config."
elif grep -q "^CONFIG_X86_64=y" "$CFG"; then
    echo "  [x86_64 target — QEMU dev environment]"
    echo "  !! M1 CANNOT BE VALIDATED HERE. x86 splits large direct-map pages on demand,"
    echo "     so set_memory_ro() succeeds regardless. A pass here says nothing about arm64."
    want_y ARCH_HAS_SET_DIRECT_MAP "API presence only; behaviour differs from arm64."
else
    echo "  !! unrecognised architecture in $CFG — cannot assert"
    fail=1
fi

echo "  [architecture-independent]"
want_y_warn DEBUG_INFO_DWARF5 "makes the D1 refusal stack traces resolvable to source lines."

echo
if [ $fail -ne 0 ]; then
    echo "ASSERT FAILED — do not build the LtRAM kernel with this config."
    echo "  A missing precondition here disables a protection SILENTLY. That is worse than"
    echo "  no protection, because it also removes the suspicion that would make you check."
    exit 1
fi
echo "ASSERT PASSED"
exit 0
