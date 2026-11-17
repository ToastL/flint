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

There are two programs. **`flintc`** is the compiler: it compiles the file it's given, and the modules that file imports. **`flint`** is the project tool: it creates projects, downloads what they depend on, and builds them by calling `flintc`.

```sh
./x                                    # builds build/flintc and build/flint (macOS on Apple Silicon); see "Building" below
./x install                            # both, into PREFIX=/usr/local by default; also DESTDIR, ./x uninstall

flintc run   examples/tour.fl          # compile and run one file
flintc build examples/tour.fl -o tour
flintc build -O2 examples/tour.fl                  # optimized; -O0 (the default) isn't
flintc build -O2 --overflow=wrap examples/tour.fl  # integer overflow wraps instead of trapping
flintc check examples/tour.fl          # compile errors only: nothing is built
flintc asm   examples/tour.fl          # the generated ARM64 assembly
flintc ir    examples/tour.fl          # the IR (optimized with -O2)
flintc test  file.fl                   # runs its `test "name" { ... }` blocks

flint init hello                       # a new project: hello/flint.toml and hello/src/main.fl
flint run -- args                      # in a project: build it and run it
flint build --release                  # out/release/<name>
flint test                             # every `test` block in the project
flint check
flint fetch                            # download its dependencies ahead of time
flint clean                            # remove out/
```

Both have `help` and `help <command>`.

## flintc: one file

Two flags replace build modes. `-O0` (the default) doesn't optimize and keeps the debugging aids: undefined memory is filled with 0xAA and `@assume` is checked. `-O2` optimizes. `--overflow=trap` (the default) makes integer overflow a panic; `--overflow=wrap` makes it wrap. Bounds, null, cast and division checks are always on: only `unsafe` removes them.

`flintc` knows nothing of `flint.toml`, dependencies or profiles. What a project needs is given to it as options, which is what `flint` does (`flint build --verbose` shows the command), and they work by hand too:

```sh
flintc build src/main.fl -o app \
    --pkg geometry=../geometry/src/geometry.fl   # a package, and its root file
    --dep geometry=geometry                      # the program imports it as `geometry`
    --dep geometry:units=units                   # ... and package geometry imports package units
    --include c                                  # where the C headers it imports are
    --object fastmath.o --lib m                  # linked with the program
    --cache out/cache                            # keep the work there; redo only what changed
```

## flint: a project

A project is a directory with a `flint.toml`; see `tests/packages/app`.

```toml
[package]
name = "app"
version = "0.1.0"
root = "src/main.fl"                 # the default

[dependencies]
geometry = { path = "../geometry" }
json = { url = "https://example.com/json-1.2.0.tar.gz", sha256 = "9f2c..." }

[c]                                  # C code in the package, built with the C compiler
sources = ["c/fastmath.c"]
include = ["c"]
flags = ["-Wall"]
link = ["m"]

[profile.release]                    # build settings: flint build --release
overflow = "wrap"
c-flags = ["-march=native"]
```

- **Profiles** are named build settings: `optimize` (`-O2`), `overflow` (`"trap"` or `"wrap"`) and `c-flags`. `debug` (the default) and `release` (optimized) exist without being written; `[profile.<name>]` changes one or makes a new one, and `--release` or `--profile=<name>` picks it. Only the profiles of the project being built count, not its dependencies'.
- **Dependencies** are a `path`, or a `url` with the `sha256` of the archive there (anything `tar` unpacks). The archive is downloaded once into `$FLINT_CACHE` (`~/.cache/flint` by default), checked against the hash and found there from then on; `flint fetch` does that ahead of time. The hash is what pins the dependency, so there's no lock file. Leave it out and flint tells you the hash of what the url has now. There's no `flint add` yet: a dependency is added by writing it in `flint.toml`.
- **`out/`** is where a project is built: the executable is `out/<profile>/<name>`, and what it was made from stays in `out/<profile>/cache/`. Each step (each C source, assembling, linking) is skipped when its inputs, including the headers a C source includes, are the ones it last ran with. `flint clean` removes `out/`.
- flint finds the compiler as `$FLINTC`, or the `flintc` next to itself, or the one in `PATH`.

A command line that's wrong gets one line saying what's wrong and a hint (`unknown command 'biuld'`, `did you mean 'build'?`). Errors and help are colored on a terminal; `--color=always|never` or `NO_COLOR` decides otherwise.

## Status

Version 0.7.0 implements the design's milestones 1–7: the type system (tagged unions, optionals, error unions, arrays, slices, tuples, packed structs), the safety model (overflow, bounds, null and cast checks), allocators and checked formatting with no hidden allocation, modules and packages, C header import, `test` blocks, comptime and generics, SIMD vectors, inline assembly (`asm { }`), and a compiler written in Flint that builds itself. [CHANGELOG.md](CHANGELOG.md) lists everything in detail; [TODO.md](TODO.md) has what's left (register returns, more backends, known gaps).

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
- **driver/** is `flintc`'s command line and the last steps: the calls to `as` and `cc`, and the cache (`--cache`) that skips them.
- **std/** is the standard library, written in Flint and compiled into the `flintc` binary. Only the parts you use end up in your program. **std/rt.fl** is the runtime: the program entry point, panics and output.

## Building

The compiler builds itself, so it needs a Flint compiler to start from:

```sh
./x                                      # stage 0 builds build/stage1/flintc, which builds build/flintc, which builds build/flint
FLINT_BOOTSTRAP=path/to/flintc ./x       # start from another compiler instead
./x check                                # build/flintc rebuilds itself to the same assembly
./x test [filter]                        # the tests (only those whose name contains filter)
./x dist                                 # build/dist/flint-<version>-<arch>-<os>.tar.gz and its sha256
./x ci                                   # build, check and test: what CI runs
./x clean                                # removes build/
```

`-O Debug|ReleaseSafe|ReleaseFast` picks the mode the compiler is built in (ReleaseSafe by default). `./x help` lists everything.

The build is a Flint program too: `dev/`. `./x` is a few lines of shell that find stage 0 and run `dev/main.fl` with it. A build is skipped when `build/flintc` and `build/flint` were already made from the same sources (their contents are hashed, so timestamps don't matter). Everything it makes is in `build/`:

```
build/flintc            the compiler
build/flint             the project tool, built by build/flintc
build/stage0/<version>  the pinned release
build/stage1/flintc     the compiler built by stage 0
build/check/            ./x check: stage 3 and both assembly files
build/dist/             ./x dist: the release
```

Stage 0 is the release pinned in `bootstrap/release`: `./x` downloads it once and checks its sha256. Each stage is `flintc build main.fl` in `compiler/`. The release pinned today (0.7.0) is from before `flint` and `flintc` were two programs: it is one `flint` that builds `compiler/` as a package, which is why `compiler/flint.toml` has to stay something that release can read (no `[profile.<name>]`). The compiler's source, and `dev/`, may use what the pinned release supports, including its standard library; to use a new language feature in the compiler itself, publish a release with it and pin that one.

The project tool is its own repository, [flint-tool](https://github.com/fl1nt-lang/flint-tool), included here as a git submodule at `tool/` (the way Rust includes Cargo). `./x` fetches it if it's missing (or `git clone --recurse-submodules`), builds it last with the new compiler, ships it in the release next to `flintc`, and runs its tests with the others. It uses `flintc` only through its command line, so it shares no code with `compiler/`. To change both in one go, change `tool/` (commit and push there), then commit the new `tool/` here so the two stay in step.

## Layout

- `compiler/` holds the compiler, `flintc` (in Flint), one directory per stage
- `tool/` is the project tool, `flint`: the flint-tool repository, as a git submodule
- `std/` holds the standard library (one module per file), including the runtime `rt.fl`
- `bootstrap/release` pins the release that builds the compiler
- `x` and `dev/` are the build: the launcher, and the build tool it runs (in Flint)
- `docs/design.md` is the language spec
- `examples/` has example programs, next to their expected output
- `tests/` has output tests (run unoptimized, optimized, and optimized with wrapping overflow), compile-error tests, multi-file module tests, `test`-block tests (`testblocks/`), command-line tests (`cli/` for `flintc`, `tool/` for `flint`) and sample projects (`packages/`); `./x test` runs them all
- `CHANGELOG.md` is what each release added; `TODO.md` is what's left
