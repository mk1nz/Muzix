#!/bin/sh
# Final verification script for the Muzix Z80 OS build.
#
# Reconciled 2026-10-14 against the current Makefile and tree:
#   - kernel/scheduler.c does not exist; the scheduler is
#     muzix_proc_table_pick_next() in kernel/proc_table.c.
#   - There is no run-tests target, and the Makefile builds no test_*.ihx
#     binaries. The host-runnable tests are tools/host_tests.rb, run by
#     `make test-host`; every source gate is `make check`; the full build and
#     image validation is `make rom`.
#   - File counts below exclude the .kilo/ worktrees, and the .ihx count is
#     labelled for what it is (linked kernel/userspace images), not "tests".
#   - A missing component is fatal, and the final status line is derived from
#     the actual results rather than printed unconditionally.
#
# NOTE: this script builds. `make rom` links the kernel and produces the ROM.
# Run it from anywhere; it cds to its own directory (the Muzix/ project root).

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
cd "$ROOT" || exit 1

FAILED=0

fail() {
    echo "  FAIL: $1"
    FAILED=$((FAILED + 1))
}

pass() {
    echo "  PASS: $1"
}

echo "======================================================"
echo "  Muzix Z80 OS - Build Verification"
echo "======================================================"
echo ""

# ---------------------------------------------------------------- toolchain --

echo "Toolchain:"
echo "=========="

check_tool() {
    if command -v "$1" > /dev/null 2>&1; then
        printf "  ok   %-8s %s\n" "$1" "$(command -v "$1")"
    else
        printf "  FAIL %-8s not found on PATH\n" "$1"
        fail "tool $1 not found on PATH"
    fi
}

check_tool "${SDCC:-sdcc}"
check_tool "${SDASZ80:-sdasz80}"
check_tool "${SDLDZ80:-sdldz80}"
check_tool "${HOST_CC:-cc}"
check_tool ruby
check_tool make

# The Makefile's check-toolchain target enforces SDCC 4.5.0; mirror it here so
# the version is reported before anything is built.
SDCC_VERSION=$("${SDCC:-sdcc}" --version 2>&1 | head -n 1)
case "$SDCC_VERSION" in
    *" 4.5.0 "*) pass "SDCC version: $SDCC_VERSION" ;;
    *) fail "expected SDCC 4.5.0, found: $SDCC_VERSION" ;;
esac

echo ""

# ------------------------------------------------------------------- counts --

# Counts exclude the .kilo/ worktrees (agent scratch checkouts) so the numbers
# describe this tree.
count() {
    find . -not -path './.kilo/*' "$@" | wc -l
}

echo "File Statistics:"
echo "================"
printf "  Core C sources (non-test):   %s\n" "$(count -name '*.c' -not -name 'test_*')"
printf "  Test C sources:              %s\n" "$(count -name 'test_*.c')"
printf "  Compiled objects (.rel):     %s\n" "$(count -name '*.rel')"
printf "  Linked images (.ihx):        %s\n" "$(count -name '*.ihx')"
echo "  (.ihx files are linked kernel/userspace images; the Makefile builds"
echo "   no standalone test binaries - tests run on the host via make test-host)"
echo ""

# --------------------------------------------------------------- components --

echo "Component Verification:"
echo "======================="

# Files that must exist in the current tree. kernel/scheduler.c was removed
# long ago; kernel/proc_table.c owns the scheduler.
COMPONENTS="kernel/kernel_main.c
kernel/proc_table.c
kernel/exec_loader.c
kernel/syscalls_ipc.c
kernel/ipc.c
mm/mproc_model.c
mm/mm_copy.c
fs/fs_service.c
servers/fs/fs_server.c
servers/mm/mm_server.c
servers/pm/pm_server.c
shell/shell.c
platform/zeta-v2/context_switch.c"

for component in $COMPONENTS; do
    if [ -f "$component" ]; then
        printf "  ok   %s\n" "$component"
    else
        printf "  FAIL %s (NOT FOUND)\n" "$component"
        fail "component $component not found"
    fi
done

echo "  note: kernel/scheduler.c does not exist by design - the scheduler is"
echo "        muzix_proc_table_pick_next() in kernel/proc_table.c"

echo ""
echo "Build System Files:"
echo "==================="

for file in Makefile tools/host_tests.rb; do
    if [ -f "$file" ]; then
        printf "  ok   %s\n" "$file"
    else
        printf "  FAIL %s (NOT FOUND)\n" "$file"
        fail "build file $file not found"
    fi
done

echo ""
echo "Documentation:"
echo "=============="

for file in AGENTS.md BUILD_REPORT.md PROJECT_STATUS.md; do
    if [ -f "$file" ]; then
        printf "  ok   %s\n" "$file"
    else
        printf "  FAIL %s (NOT FOUND)\n" "$file"
        fail "documentation $file not found"
    fi
done

echo ""

# ------------------------------------------------------------------ targets --

echo "Makefile Targets:"
echo "================="

# Verify the targets the script is about to run actually exist. `make help`
# lists only a few, so check the Makefile text itself.
for target in check test-host test-kernel-layout rom clean; do
    if grep -q "^${target}:" Makefile; then
        printf "  ok   make %s\n" "$target"
    else
        printf "  FAIL make %s is not a target in this Makefile\n" "$target"
        fail "target $target missing"
    fi
done

echo ""

# --------------------------------------------------------------------- run --

echo "Build Verification:"
echo "==================="

# `make check` runs every source gate (test-kernel-layout, test-window0,
# test-syscall-context, test-user-pointers, test-link-scripts,
# test-emulator-sources, test-userspace-layout, test-userspace-runtime,
# test-memory-manager, test-park-site, test-proc-identity) plus `make
# test-host`. `make rom` is the full build and runs the ROM validators.
run_make() {
    echo "--- make $1 ---"
    if make "$1"; then
        pass "make $1"
    else
        fail "make $1"
    fi
}

run_make check
run_make test-kernel-layout
run_make test-host
run_make rom

echo ""

# ------------------------------------------------------- kernel size report --

echo "Kernel Layout (from build/kernel.map):"
echo "======================================"

if [ -f build/kernel.map ]; then
    grep -E "^_CODE[[:space:]]+[0-9A-F]" build/kernel.map | head -n 1
    grep -E "^_DATA[[:space:]]+[0-9A-F]" build/kernel.map | head -n 1
    echo "  (source of truth for extents and headroom: make test-kernel-layout)"
else
    fail "build/kernel.map missing after make rom"
fi

echo ""
echo "======================================================"
if [ "$FAILED" -eq 0 ]; then
    echo "  Build Verification: ✅ ALL CHECKS PASSED"
else
    echo "  Build Verification: ❌ FAILED — $FAILED check(s) failed; see above"
fi
echo "======================================================"
echo ""
echo "To rebuild:"
echo "  make clean && make rom      (there is no run-tests target)"
echo ""
echo "To re-run the host tests alone:"
echo "  make test-host"
echo ""
echo "Next steps (not covered by this script):"
echo "  1. Emulator smoke test: cd emulator && make && \\"
echo "       printf 'uptime\\n' | ./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -i -c 100000000"
echo "     expect the userspace shell banner, an uptime result, and no MNT FAIL"
echo "  2. Boot on real Zeta V2 hardware"
echo ""

[ "$FAILED" -eq 0 ]
