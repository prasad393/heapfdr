# heapfdr

![ci](https://github.com/prasad393/heapfdr/actions/workflows/ci.yml/badge.svg)

A heap flight recorder for Linux C and C++ programs.

Preload it into a program and it records every heap block, the call stack that allocated it, and the last few heap operations. At exit it lists what was never freed. If the program crashes, it writes a report with the fault, a backtrace and the heap history just before the crash, so a use-after-free looks different from a NULL dereference. The program under test doesn't need to be recompiled.

I wrote it to understand what leak checkers and crash reporters do underneath (allocator interposition, stack unwinding, signal-safe output) and to have something lighter than Valgrind for quick checks.

## What it catches

| Problem | What you get |
|---|---|
| Memory leaks | Report at exit, grouped by allocation stack, largest first |
| Double free | Reported on the spot with where the block was allocated and first freed; the second free is skipped |
| Freeing a pointer into the middle of a block | Reported with the block it points into |
| Use-after-free that crashes | The crash report ties the fault address to the freed block and shows both stacks |
| NULL dereference, stack overflow, `abort()`, divide by zero | Crash report with a hint, registers, backtrace and recent heap events |

## Build

Needs Linux with glibc, GCC or Clang, and CMake 3.16+. Python 3 and binutils are only needed for file:line output.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

On Ubuntu or WSL: `sudo apt install build-essential cmake binutils python3`

## Usage

```sh
./build/heapfdr ./your-program args...

# or by hand
LD_PRELOAD=$PWD/build/libheapfdr.so ./your-program args...
```

Build your program with `-g`, then run the report through the symbolizer to get file and line numbers:

```sh
./build/heapfdr ./your-program 2>&1 | ./build/heapfdr-symbolize
./build/heapfdr-symbolize heapfdr-crash-1234.log
```

### Demo

`telemetry-demo` is a small telemetry pipeline with one planted bug per scenario: `clean`, `leak`, `double-free`, `bad-free`, `null`, `use-after-free`, `stack-overflow` and `abort`.

```sh
./build/telemetry-demo                         # list the scenarios
./build/heapfdr ./build/telemetry-demo leak
./build/heapfdr ./build/telemetry-demo use-after-free
```

## Example output

Leak report, run through `heapfdr-symbolize`:

```
[heapfdr] ======== LEAK REPORT (pid 3193) ========
[heapfdr] 4 blocks (85 B) never freed, from 2 call sites
[heapfdr]
[heapfdr] leak 1 of 2: 63 B in 2 blocks (this site allocated 4 times)
[heapfdr]     #0   0x7fa7c2cb445f  __strdup+0x1e  (libc.so.6:0xb445e)  at ./string/./string/strdup.c:42
[heapfdr]     #1   0x5607dbe58725  describe+0x6c  (telemetry-demo:0x1724)  at examples/telemetry_demo.c:118
[heapfdr]     #2   0x5607dbe5875a  log_packet+0x1e  (telemetry-demo:0x1759)  at examples/telemetry_demo.c:123
[heapfdr]     #3   0x5607dbe58cb9  run_leak+0x15d  (telemetry-demo:0x1cb8)  at examples/telemetry_demo.c:242
[heapfdr]     #4   0x5607dbe591f7  main+0xe2  (telemetry-demo:0x21f6)  at examples/telemetry_demo.c:349
[heapfdr]
[heapfdr] leak 2 of 2: 22 B in 2 blocks (this site allocated 6 times)
[heapfdr]     #0   0x5607dbe58527  decode+0xdd  (telemetry-demo:0x1526)  at examples/telemetry_demo.c:72
[heapfdr]     #1   0x5607dbe58c56  run_leak+0xfa  (telemetry-demo:0x1c55)  at examples/telemetry_demo.c:235
[heapfdr]     #2   0x5607dbe591f7  main+0xe2  (telemetry-demo:0x21f6)  at examples/telemetry_demo.c:349
[heapfdr]
[heapfdr] not counted: 7 blocks (8.1 KiB) held by the C runtime, startup code or suppressed sites
[heapfdr] totals: 24 allocations, 13 frees, peak 8.3 KiB in use, 0 errors
```

Use-after-free crash (event list shortened):

```
[heapfdr] ======== CRASH ========
[heapfdr] signal   SIGSEGV (address not mapped)
[heapfdr] address  0x7f01d57ff010
[heapfdr] hint     address is 0 bytes into a 1.0 MiB block freed 0.014 ms earlier: use-after-free
[heapfdr]   that block was allocated at:
[heapfdr]     #0   0x55b5f0c3185d  assemble+0x1e  (telemetry-demo:0x185c)
[heapfdr]     #1   0x55b5f0c31f3b  run_use_after_free+0x36  (telemetry-demo:0x1f3a)
[heapfdr]     #2   0x55b5f0c321f7  main+0xe2  (telemetry-demo:0x21f6)
[heapfdr]   and freed at:
[heapfdr]     #0   0x55b5f0c3189a  flush_frame+0x1b  (telemetry-demo:0x1899)
[heapfdr]     #1   0x55b5f0c31f4b  run_use_after_free+0x46  (telemetry-demo:0x1f4a)
[heapfdr]     #2   0x55b5f0c321f7  main+0xe2  (telemetry-demo:0x21f6)
[heapfdr] process  pid 3166, tid 3166, 0.311 ms after start
[heapfdr] regs     pc 0x7f01d6ba16fa  sp 0x7ffe6ffccb88  fp 0x7ffe6ffccba0
[heapfdr] backtrace:
[heapfdr]     #0   0x7f01d6ba16fa  ??  (libc.so.6:0x1a16fa)
[heapfdr]     #1   0x55b5f0c318e2  annotate+0x44  (telemetry-demo:0x18e1)
[heapfdr]     #2   0x55b5f0c31f61  run_use_after_free+0x5c  (telemetry-demo:0x1f60)
[heapfdr]     #3   0x55b5f0c321f7  main+0xe2  (telemetry-demo:0x21f6)
[heapfdr] last 11 heap events, oldest first:
[heapfdr]     ...
[heapfdr]     t+0.256 ms     malloc    0x55b5fa38d3b0  4.0 KiB     tid 3166
[heapfdr]     t+0.274 ms     calloc    0x7f01d57ff010  1.0 MiB     tid 3166
[heapfdr]     t+0.279 ms     free      0x7f01d57ff010  1.0 MiB     tid 3166
[heapfdr] heap     7 live blocks, 8.1 KiB in use, peak 1.0 MiB, 9 allocations
```

## Configuration

Everything is set through environment variables.

| Variable | Default | Meaning |
|---|---|---|
| `HEAPFDR_DEPTH` | `12` | Frames recorded per allocation (1 to 24) |
| `HEAPFDR_TOP` | `10` | Leak sites printed at exit |
| `HEAPFDR_SUPPRESS` | | Comma-separated substrings. Leaks whose stack has a function or library name containing one of them are not reported |
| `HEAPFDR_EXITCODE` | `0` | Exit with this code when leaks are found, handy in CI |
| `HEAPFDR_ABORT` | `0` | `1` aborts on a double or invalid free instead of skipping the call |
| `HEAPFDR_STARTUP` | `0` | `1` also reports memory allocated before `main()` |
| `HEAPFDR_LOG_DIR` | `.` | Where crash logs are written, `none` to turn them off |
| `HEAPFDR_CHILDREN` | `0` | `1` keeps tracing child processes |
| `HEAPFDR_QUIET` | `0` | `1` drops the start-up line |

## How it works

- **Interposition.** The library defines `malloc`, `free`, `realloc`, `calloc` and the aligned variants. With `LD_PRELOAD` the dynamic linker binds the program to these first; each one calls the real glibc function, found with `dlsym(RTLD_NEXT, ...)`, and records the result.
- **Bootstrapping.** `dlsym()` may itself call `calloc()` before the real allocator is known, so the first few requests are served from a small static buffer.
- **Bookkeeping without malloc.** Live blocks sit in an open-addressing hash table and call stacks are interned into "sites". All of it lives in `mmap()` memory, so the tool never calls back into the allocator it is watching.
- **Lock order.** Stacks are captured before the bookkeeping lock is taken, because the unwinder takes the dynamic loader's lock and `dlopen()` calls `malloc()` while holding it. Reports print without the lock for the same reason.
- **Crash path.** Handlers for SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT run on an alternate signal stack and format output with a small writer that only calls `write(2)`. Afterwards the previous handler is restored, so the process still dies with the right status and can leave a core dump.
- **Flight recorder.** A ring of the last 64 heap events and the last 4096 freed blocks. That is what turns "SIGSEGV at 0x7f..." into "0 bytes into a block freed 0.014 ms earlier".
- **Startup noise.** Wrapping `__libc_start_main` tells the tool when `main()` starts, so memory that libc and the C++ runtime set up before it, plus stdio buffers, stays out of the leak report.
- **Names.** `dladdr()` only knows exported symbols, so static functions are looked up in the binary's ELF `.symtab` directly. C++ names are demangled, except in the crash path.

## Limitations

- Linux and glibc only. Developed and tested on x86_64 (Ubuntu 24.04, GCC 13, Clang 18); the aarch64 register code is untested.
- No reachability scan: a block still referenced from a global at exit counts as a leak (Valgrind would call it "still reachable").
- Use-after-free is only caught when it crashes, which in practice means large blocks that glibc returned to the OS. Small freed blocks stay mapped, so touching them doesn't fault.
- One global lock, so heavily multi-threaded programs slow down.
- The alternate signal stack is only set up for the main thread; a stack overflow in another thread kills the process without a report.
- `backtrace()` and `dladdr()` are not strictly async-signal-safe. A crash inside the dynamic loader could hang the report.
- Statically linked programs can't be preloaded.

## Layout

```
src/
  hooks.c      allocator entry points, start-up and shutdown
  ledger.c     live blocks, call sites, freed blocks, event ring
  stack.c      stack capture, symbol lookup, frame printing
  elfsym.c     .symtab lookup for static functions
  crash.c      fatal signal handler
  report.c     leak and misuse reports
  out.c        output formatting that never allocates
  config.c     HEAPFDR_* settings
examples/      telemetry-demo
tests/         scenario tests, stress test, allocator API test
tools/         heapfdr launcher, heapfdr-symbolize
```

## License

MIT, see [LICENSE](LICENSE).
