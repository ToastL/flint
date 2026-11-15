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
make                                  # builds build/flint (macOS on Apple Silicon); see "Building" below
./build/flint run   examples/tour.fl
./build/flint build examples/tour.fl -o tour
./build/flint build -O ReleaseFast examples/tour.fl  # -O Debug (default) | ReleaseSafe | ReleaseFast
./build/flint asm   examples/tour.fl  # the generated ARM64 assembly
./build/flint ir    examples/tour.fl  # the optimized IR
./build/flint test  file.fl           # runs its `test "name" { ... }` blocks
make test
make install                          # PREFIX=/usr/local by default; also DESTDIR, make uninstall
```

A package is a directory with a `flint.toml` (name, path dependencies, C sources to build and libraries to link). Inside it, `flint build`, `flint run -- args` and `flint test` need no file name; see `tests/packages/app`.

## Status against the design

**Implemented:**
- `let` (immutable) and `var`, names first and types after
- mandatory initialization and `= undefined`
- required `;`, parenthesized conditions and mandatory braces
- fixed-width integers only
- the safety model (design §6):
  - `+ - *` and negation trap on overflow in Debug and ReleaseSafe and wrap in ReleaseFast; `/` and `%` trap on zero in every mode; shift amounts must be unsigned and smaller than the bit width
  - wrapping `+% -% *%`, saturating `+| -| *|`, and `@addWithOverflow` / `@subWithOverflow` / `@mulWithOverflow`
  - `x as T` checks that the value fits, in every mode (integers, and float to int)
  - `unsafe { }` and `unsafe fn`: the only places for `[*]T` indexing and arithmetic, `@ptrCast`, `@intToPtr`, reading an `extern union` field, `@assume`, and unchecked array/slice indexing
  - build modes `-O Debug | ReleaseSafe | ReleaseFast`; Debug fills `undefined` memory with `0xAA`
  - returning `&local` is a compile error
- the precedence fix and non-chainable comparisons
- `for (xs) |x, i|`, `for (xs) |*x|`, `for (a..b) |i|`, and labelled `break`/`continue`
- `if (c) a else b`
- postfix `p.*`, `*const T` and `[]const T`
- `usize`/`isize` as their own types: lengths and indexes are `usize`
- fixed arrays `[N]T`, which are values (copied on assignment and when passed), with `.len`, slicing `a[i..j]` / `a[..]`, and `*[N]T` → `[]T`
- stack array literals `[3]i32 { 1, 2, 3 }`, `[_]u8 { ... }` and `@splat(v)`
- `*T` points at one item; `[*]T` points at many and is the only pointer you can index or offset (in `unsafe` code)
- `cstr` for C strings: string literals convert to it, and `mem.span(c)` converts back
- tuples `(A, B)` with `.0`/`.1`, multiple return values, and `let (q, r) = divmod(a, b);`
- anonymous literals `.{ x: 1 }` / `.{ 1, 2 }` that take their type from context
- function pointer types `fn(i32) -> i32` and `?fn(...)`; function names are values
- structs with methods, field defaults and `Self`
- enums: plain (`enum Http: u16 { Ok = 200 }`) and tagged (`Circle(f64)`, `Rect { w: f64, h: f64 }`), with methods and `@tagName`
- exhaustive `switch`, as a statement or an expression: payload captures, ranges `1..=9`, several patterns per case, `else`, and explicit `fallthrough`
- optionals `?T`: `if (x) |v|`, `if (x) |*v|` (change it in place), `while (x) |v|`, `x orelse y`, `x.?` (traps on null); plain `*T` is never null, and `?*T` is the same size as `*T`
- `return`, `break`, `continue`, `unreachable` and `panic(...)` as expressions of type `noreturn` (e.g. `x orelse return 0`)
- error unions `E!T` / `!T`:
  - error sets `error IoError { NotFound, Denied }`
  - `try` (passes the error up), `catch` (a value, `|e|` with a switch, or a block), `if (x) |v| {} else |e| {}`
  - `errdefer |e|`, which runs only on error exits
  - `switch` on errors, `@errorName`
  - `main() -> !void` prints the error and exits with status 1
  - ignoring an error union is a compile error
- slices with bounds checks
- `defer`
- `_ = f();` (unused results are an error)
- `x as Color`: integer to enum, checked
- `@sizeOf`, `@alignOf`, `@minInt`, `@maxInt`, `@zeroes`, `@truncate`, `@bitcast`, `@splat`, `@ptrToInt`, `@TypeOf`, `@hash`, `@eql`, `@typeName`, `@reduceAdd`
- C interop: `import c "stdio.h" as c;` (functions, structs, unions, enums, globals and constant macros, translated from the header when they're used), `extern fn`, `export fn`, `extern let/var` globals, `export let/var`, `extern union`, and `packed struct` with bit fields (`mode: u3`)
- modules (design §4.10): one file is one module, `import std.io;` / `import net.http as web;`, everything private unless `pub`, any declaration order, import cycles allowed. There is no implicit prelude: only compiler builtins (`panic`, `assert`, `args`, `sqrt`, ...) are visible without an import
- the standard library as modules: `std.mem` (Allocator, slices and byte strings), `std.heap`, `std.fmt`, `std.io`, `std.list` (ArrayList), `std.map` (HashMap), `std.ascii`, `std.hash`, `std.testing`, `std.target`
- `test "name" { ... }` blocks and `flint test`: each test runs in its own process, so a panic fails only that test
- packages: `flint.toml` with path dependencies and a `[c]` table (sources, include directories, flags, libraries); `flint build` / `run` / `test` without a file
- memory and strings with no hidden allocations (design §4.13–4.14):
  - `mem.Allocator` (`alloc`, `create`, `free`, `destroy`, `realloc`, `dupe`); running out is `error.OutOfMemory`
  - `heap.c_allocator`, `heap.page_allocator`, `heap.Arena`, `heap.FixedBuffer`, and `heap.DebugAllocator`, which reports leaks and stops double frees, wrong-length frees and uses after free (freed pages are poisoned and made inaccessible)
  - `str` is just `[]const u8`; slices are `{ ptr, len }`, 16 bytes
  - `io.print`, `io.eprint`, `fmt.bufPrint`, `fmt.allocPrint`, `fmt.count` and `Writer.print`, with format strings checked against their arguments at compile time: `{}`, `{s}`, `{d}`, `{x}`, `{c}`, `{e}`, `{any}`, `{!}`, width, fill, alignment and precision
  - `list.ArrayList(T)` and `map.HashMap(K, V)`; string functions that allocate take an allocator; `mem.eql`, `mem.find`, `mem.trim`, `mem.split`, `fmt.parseInt`, ...
- comptime and generics (design §4.11), run by an interpreter inside the compiler (src/interp.c) that executes the compiler's own IR. It's deterministic, has no I/O and stops after 10 million steps. A panic, an overflow or a bad index at compile time is a compile error that shows the chain of calls.
  - `comptime expr`, `comptime { ... }` (in functions and at the top level), and precomputed tables: `let TABLE = comptime crcTable();` becomes data (pointers in the result become relocations)
  - `if (comptime target.os == .linux)` with `import std.target;`: only the taken branch is type-checked, and the other must still name things that exist
  - `comptime` type and value parameters, `anytype`, `@TypeOf`, monomorphized
  - types as values: `let T = list.ArrayList(i32);`, `Int(16) == u16`, `@typeName`
  - type functions with logic in them (`if (bits <= 8) { return u8; }`), with `let`s computed from the parameters; generic structs can declare `let`s and types (`Tree(i32).Node`)
  - constraints: `fn sum(comptime T: type, xs: []const T) -> T where isNumber(T)`, checked at the call, with `@isInt`, `@isFloat`, `@isSigned`, `@hasField`, `@hasMethod` and `@compileError`
- SIMD vectors (design §4.12): `@Vector(4, f32)` is one 8- or 16-byte NEON register; `+ - * / & | ^`, unary `-`, indexing, `@splat` and `@reduceAdd` are each one instruction
- `let`/`var` and types declared inside a struct (`Config.default_port`, `Outer.Inner`); global data made of constant structs, function names and `&global`
- more than 8 integer/float arguments between Flint functions (the rest go on the stack)

**Not yet:**
- returning `!T` in registers (it goes through memory today); error return traces in debug builds
- jump tables for dense switches (it's a compare chain for now)
- inline assembly `asm { }`
- dependencies fetched by URL and hash (path dependencies work)
- passing or returning structs by value to and from C functions
- keeping vectors in SIMD registers between operations (they go through memory: `ldr q`, op, `str q`)

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
make                                     # stage 0 builds stage 1, which builds build/flint
make FLINT_BOOTSTRAP=path/to/flint       # start from a prebuilt flint (e.g. a release)
make FLINT_BOOTSTRAP=$(scripts/bootstrap.sh)   # ... the release pinned in bootstrap/VERSION
make selfhost-check                      # build/flint rebuilds itself to the same assembly
make diff-test                           # the C and Flint compilers agree on every test program
make release                             # build/release/flint-<version>-aarch64-macos.tar.gz
```

Without `FLINT_BOOTSTRAP`, stage 0 is the original compiler written in C (`src/`), which only needs a C compiler. It is frozen: new language features go into `compiler/` only.

## Layout

- `compiler/` holds the compiler (in Flint), one directory per stage
- `src/` holds the original compiler in C, used to bootstrap
- `std/` holds the standard library (one module per file), including the runtime `rt.fl`
- `docs/design.md` is the language spec
- `examples/` has example programs
- `tests/` has output tests (run in every build mode), panic tests, compile-error tests, multi-file module tests, `flint test` tests and a sample package; `tests/diff.sh` compares two builds of the compiler
- `bench/` has the benchmarks
