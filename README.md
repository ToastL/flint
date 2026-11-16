# Flint

C with its historical accidents fixed, keeping its philosophy: what you write is roughly what the machine does. There's no garbage collector, no exceptions, no hidden control flow and a tiny runtime.

The full design (rationale, grammar, type system, safety rules, comparison with Zig, Odin, C3 and Hare) is in **[docs/design.md](docs/design.md)**.

```
import std.heap;
import std.io;
import std.list;

struct Vec2 {
    x: f64,
    y: f64,

    fn len(self: Vec2) -> f64 { return sqrt(self.x * self.x + self.y * self.y); }
    fn scale(self: *Self, k: f64) { self.x *= k; self.y *= k; }
}

fn sum(xs: []const i64) -> i64 {
    var total: i64 = 0;
    for (xs) |x| { total += x; }
    return total;
}

fn main() -> !void {
    var gpa = heap.DebugAllocator.init();  // every allocation goes through an Allocator
    defer gpa.deinit();                    // reports leaks
    var xs = list.ArrayList(i64).init(gpa.allocator());
    defer xs.deinit();
    try xs.appendSlice(&[_]i64 { 1, 2, 3 });

    var v = Vec2 { x: 3, y: 4 };
    v.scale(2);                          // exactly Vec2.scale(&v, 2)
    let flags: u32 = 6;
    if (flags & 1 == 0) {                // bitwise binds tighter than ==, unlike C
        try io.print("even, len {d:.1}, sum {}\n", .{ v.len(), sum(xs.items) }); // checked at compile time
    }
    var small: u8 = 250;
    small +%= 10;                        // wrapping must be asked for
}
```

```sh
./x                                   # builds build/flint (macOS on Apple Silicon); see "Building" below
./build/flint run   examples/tour.fl
./build/flint build examples/tour.fl -o tour
./build/flint build -O2 examples/tour.fl             # optimized; -O0 (the default) isn't
./build/flint build -O2 --overflow=wrap examples/tour.fl  # integer overflow wraps instead of trapping
./build/flint check examples/tour.fl  # compile errors only: nothing is built
./build/flint asm   examples/tour.fl  # the generated ARM64 assembly
./build/flint ir    examples/tour.fl  # the IR (optimized with -O2)
./build/flint test  file.fl           # runs its `test "name" { ... }` blocks
./build/flint help  build             # every command has its own help; `flint help` lists them
./x test
./x install                           # PREFIX=/usr/local by default; also DESTDIR, ./x uninstall
```

Two flags replace build modes. `-O0` (the default) doesn't optimize and keeps the debugging aids: undefined memory is filled with 0xAA and `@assume` is checked. `-O2` optimizes. `--overflow=trap` (the default) makes integer overflow a panic; `--overflow=wrap` makes it wrap. Bounds, null, cast and division checks are always on: only `unsafe` removes them.

A package is a directory with a `flint.toml` (name, path dependencies, C sources to build and libraries to link). `flint init name` starts one. Inside it, `flint build`, `flint run -- args`, `flint test` and `flint check` need no file name, and `flint clean` removes what `flint build` made; see `tests/packages/app`.

A command line that's wrong gets one line saying what's wrong and a hint (`unknown command 'biuld'`, `did you mean 'build'?`). Errors and help are colored on a terminal; `--color=always|never` or `NO_COLOR` decides otherwise.

## Status

Version 0.7.0 implements the design's milestones 1–7: the type system (tagged unions, optionals, error unions, arrays, slices, tuples, packed structs), the safety model (overflow, bounds, null and cast checks), allocators and checked formatting with no hidden allocation, modules and packages, C header import, `flint test`, comptime and generics, SIMD vectors, inline assembly (`asm { }`), and a compiler written in Flint that builds itself. [CHANGELOG.md](CHANGELOG.md) lists everything in detail; [TODO.md](TODO.md) has what's left (register returns, more backends, known gaps).

## How it compiles

The compiler is written in Flint (`compiler/`) and generates ARM64 assembly itself. It doesn't go through C or LLVM.

```
source ─syntax/─▶ syntax tree ─sema/─▶ IR ─opt/─▶ optimized IR ─arm64/─▶ .s ─as─▶ .o ─cc─▶ executable
```

- **syntax/** lexes and parses.
- **modules/** finds the files a program imports and embeds the standard library; **cimport/** reads C headers through the C preprocessor and parses their declarations.
- **sema/** type-checks and lowers to an IR of basic blocks over virtual registers (one file per concern: types, names, operators, calls, generics, comptime, `@format`, statements, ...).
- **interp/** runs that IR at compile time, for `comptime` code and type functions.
- **opt/** improves the IR, one pass per file:
  - promotes variables, and struct fields, from memory to registers
  - folds constants
  - propagates copies
  - computes repeated values once and removes redundant bounds checks
  - removes dead code
- **arm64/** allocates registers with linear scan and emits Mach-O assembly.
- **driver/** is the command line, `flint.toml`, and the calls to `as` and `cc`.
- **std/** is the standard library, written in Flint and compiled into the `flint` binary. Only the parts you use end up in your program. **std/rt.fl** is the runtime: the program entry point, panics and output.

## Building

The compiler builds itself, so it needs a Flint compiler to start from:

```sh
./x                                      # stage 0 builds build/stage1/flint, which builds build/flint
FLINT_BOOTSTRAP=path/to/flint ./x        # start from another flint instead
./x check                                # build/flint rebuilds itself to the same assembly
./x test [filter]                        # the tests (only those whose name contains filter)
./x dist                                 # build/dist/flint-<version>-<arch>-<os>.tar.gz and its sha256
./x ci                                   # build, check and test: what CI runs
./x clean                                # removes build/
```

`-O Debug|ReleaseSafe|ReleaseFast` picks the mode the compiler is built in (ReleaseSafe by default). `./x help` lists everything.

The build is a Flint program too: `dev/`. `./x` is a few lines of shell that find stage 0 and run `dev/main.fl` with it. A build is skipped when `build/flint` was already made from the same sources (their contents are hashed, so timestamps don't matter). Everything it makes is in `build/`:

```
build/flint             the compiler
build/stage0/<version>  the pinned release
build/stage1/flint      the compiler built by stage 0
build/check/            ./x check: stage 3 and both assembly files
build/dist/             ./x dist: the release
```

Stage 0 is the release pinned in `bootstrap/release`: `./x` downloads it once and checks its sha256. The compiler is an ordinary Flint package (`compiler/flint.toml`), so each stage is just `flint build` in `compiler/`. Its own source, and `dev/`, may use what the pinned release supports; to use a new language feature in the compiler itself, publish a release with it and pin that one.

## Layout

- `compiler/` holds the compiler (in Flint), one directory per stage
- `std/` holds the standard library (one module per file), including the runtime `rt.fl`
- `bootstrap/release` pins the release that builds the compiler
- `x` and `dev/` are the build: the launcher, and the build tool it runs (in Flint)
- `docs/design.md` is the language spec
- `examples/` has example programs, next to their expected output
- `tests/` has output tests (run unoptimized, optimized, and optimized with wrapping overflow), compile-error tests, multi-file module tests, `flint test` tests (`testblocks/`), command-line tests (`cli/`) and sample packages; `./x test` runs them all
- `CHANGELOG.md` is what each release added; `TODO.md` is what's left
