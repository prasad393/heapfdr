#!/usr/bin/env bash
# Runs the demo scenarios, the stress test and the allocator API test
# under heapfdr and checks that each report says what it should.
#
#   tests/run_tests.sh [build-dir]
set -u

build=$(cd "${1:-build}" && pwd)
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
lib="$build/libheapfdr.so"
demo="$build/telemetry-demo"
stress="$build/heap-stress"
api="$build/heap-api"
symbolize="$root/tools/heapfdr-symbolize"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

passed=0
failed=0
label=""
status=0

# run <label> [VAR=value ...] -- <command...>
run() {
    label=$1
    shift
    local -a vars=()
    while [[ $1 != -- ]]; do
        vars+=("$1")
        shift
    done
    shift
    (cd "$work" && env HEAPFDR_LOG_DIR="$work" "${vars[@]}" LD_PRELOAD="$lib" "$@") \
        >"$work/out" 2>&1
    status=$?
}

ok()  { printf '  ok    %-16s %s\n' "$label" "$1"; passed=$((passed + 1)); }
bad() {
    printf '  FAIL  %-16s %s\n' "$label" "$1"
    failed=$((failed + 1))
    sed 's/^/        | /' "$work/out"
}

expect()     { if grep -Eq -- "$2" "$work/out"; then ok "$1"; else bad "$1"; fi; }
expect_not() { if grep -Eq -- "$2" "$work/out"; then bad "$1"; else ok "$1"; fi; }
expect_status() {
    if [[ $status -eq $1 ]]; then ok "exit status $1"; else bad "exit status $status, wanted $1"; fi
}

echo "heapfdr tests ($build)"

run clean -- "$demo" clean
expect_status 0
expect "no leaks"                 "no leaks: everything allocated by the program was freed"
expect "no errors"                "totals: .* 0 errors"

run leak -- "$demo" leak
expect_status 0
expect "leak summary"             "4 blocks \(85 B\) never freed, from 2 call sites"
expect "decode() error path"      "#0 .* decode\+"
expect "describe() via strdup"    "#0 .*strdup\+"
expect "early return in logger"   "log_packet\+"

run double-free -- "$demo" double-free
expect_status 0
expect "detected"                 "DOUBLE FREE"
expect "second free site"         "cancel\+"
expect "first free site"          "transmit\+"
expect "program kept running"     "uplink job finished"

run bad-free -- "$demo" bad-free
expect_status 0
expect "detected"                 "INVALID FREE"
expect "offset into block"        "6 bytes inside a live 17 B block"
expect "block shows up as leak"   "1 block \(17 B\) never freed"

run null -- "$demo" null
expect_status 139
expect "signal"                   "SIGSEGV \(address not mapped\)"
expect "hint"                     "NULL pointer dereference"
expect "faulting function"        "#0 .* route\+"
expect "crash log written"        "also written to .*heapfdr-crash-[0-9]+\.log"

run use-after-free -- "$demo" use-after-free
expect_status 139
expect "hint"                     "0 bytes into a 1.0 MiB block freed .* use-after-free"
expect "where it was freed"       "flush_frame\+"
expect "who wrote to it"          "annotate\+"
expect "recent events"            "free +0x[0-9a-f]+  1.0 MiB"

run stack-overflow -- "$demo" stack-overflow
expect_status 139
expect "hint"                     "most likely a stack overflow"
expect "recursing function"       "nesting\+"

run abort -- "$demo" abort
expect_status 134
expect "signal"                   "SIGABRT \(abort\(\) was called\)"
expect "caller"                   "check_sequence\+"

run exit-code HEAPFDR_EXITCODE=23 -- "$demo" leak
expect_status 23

run suppress HEAPFDR_SUPPRESS=describe -- "$demo" leak
expect "one site left"            "2 blocks \(22 B\) never freed, from 1 call site"
expect "hidden"                   "not counted: .*suppressed"

run abort-on-error HEAPFDR_ABORT=1 -- "$demo" double-free
expect_status 134
expect "stopped"                  "stopping here because HEAPFDR_ABORT=1"

run stress -- "$stress"
expect_status 0
expect "no leaks"                 "no leaks"
expect "every free matched"       "totals: .* 0 errors"
expect_not "no unknown pointers"  "never saw"

run stress-leak -- "$stress" 25
expect "exact count"              "25 blocks \(2.4 KiB\) never freed, from 1 call site"
expect "leaking function"         "keep_forever\+"

run api -- "$api"
expect_status 0
expect "no leaks"                 "no leaks"
expect_not "no false alarms"      "DOUBLE FREE|INVALID FREE|UNKNOWN POINTER"

run api-leak -- "$api" leak
expect "one block per allocator"  "5 blocks \(258 B\) never freed, from 5 call sites"
expect "posix_memalign block"     "leak 1 of 5: 128 B in 1 block"
expect "aligned_alloc block"      "leak 2 of 5: 64 B in 1 block"
expect "caller, not the hook"     "#0 .* leave_one_of_each\+"

run quiet HEAPFDR_QUIET=1 -- "$demo" clean
expect_not "no banner"            "recording heap activity"

if command -v addr2line >/dev/null && command -v python3 >/dev/null; then
    run symbolize -- "$demo" leak
    python3 "$symbolize" "$work/out" >"$work/sym" 2>&1
    cp "$work/sym" "$work/out"
    expect "file and line"        "decode\+.* at .*telemetry_demo\.c:[0-9]+"
else
    echo "  skip  symbolize        (needs addr2line and python3)"
fi

echo "$passed passed, $failed failed"
[[ $failed -eq 0 ]]
