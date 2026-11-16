# Changelog

## Unreleased

- `flint version` prints the compiler's version
- The C compiler is retired: the compiler bootstraps from the pinned 0.7.0 release (`bootstrap/`)
- The compiler builds as an ordinary package (`compiler/flint.toml`)
- CI runs the build, the self-hosting check and the tests on every push (`.github/workflows/test.yml`)
- Removed: `flint tokens` / `flint ast` (they existed to compare against the C compiler)

## 0.7.0 (2026-11-15): first release

The first released version: the language as designed in milestones 1–7, with a compiler written
in Flint that builds itself. Binary for macOS on Apple Silicon.

### Milestone 1: tagged unions

- Enums: plain and tagged (tuple and struct payloads), methods, `@tagName`
- Exhaustive `switch`: statement and expression, captures, ranges, multiple patterns, `else`, `fallthrough`
- Optionals `?T`, non-null `*T`, `if (x) |v|`, `while (x) |v|`, `orelse`, `.?`
- `noreturn` expressions (`return`, `break`, `continue`, `unreachable`, `panic`)
- Error unions `E!T` / `!T`, error sets, `try`, `catch`, `errdefer`, `else |e|`, `main() -> !void`

### Milestone 2: finish the type system (§3)

- Fixed arrays `[N]T`
  - value semantics: copied on assignment and when passed
  - `.len`, and slicing `arr[..]`
  - `*[N]T` → `[]T` coercion
- Array literals `[N]T { ... }` / `[_]T { ... }` and fill values (`@splat`), allocated on the stack
- `[*]T` many-item pointers. Indexing and arithmetic moved off `*T` onto `[*]T` (and need `unsafe` since milestone 3).
- `cstr` for C interop
  - string literals coerce to it
  - `mem.span(c)` converts it to a string. `mem` is a prelude struct used as a namespace until modules exist (milestone 5), and it returns `str` until `str` becomes `[]const u8` (milestone 4).
- Tuples `(A, B)`, multiple return values, `let (q, r) = divmod(a, b);`
- Function pointer types `fn(i32) -> i32` and `?fn(...)`; passing function names as values
- Anonymous literals `.{ x: 1 }` / `.{ 1, 2 }` that take their type from context
- `usize`/`isize` as distinct types, and `.len` as `usize`. `for (0..n)` with literal bounds counts in `usize`.
- Converting integers to enums (checked)
- `if (x) |*v|`: capture an optional by pointer to change it in place
- `packed struct` (bit fields, up to 64 bits), and `extern union` for C interop
- `export` for variables; `extern` globals

### Milestone 3: the safety model (§6)

- Overflow traps on `+ - *` and negation in Debug and ReleaseSafe; wrapping in ReleaseFast and inside `unsafe`. Negating an unsigned value is a compile error.
- Saturating operators `+| -| *|`, plus `@addWithOverflow` / `@subWithOverflow` / `@mulWithOverflow`
- Checked `as`: integer narrowing and sign changes, float to int (NaN, out of range); constants that don't fit are compile errors. A runtime value that doesn't fit a packed bit field traps too.
- Shift amounts must be unsigned; ≥ bit width traps in safe modes, taken modulo the width in ReleaseFast
- Integer division by zero traps in every mode; `MIN / -1` traps in safe modes
- `unsafe { }` blocks: unchecked indexing, `[*]T` indexing/slicing/arithmetic, pointer casts. Defers keep the safety of the scope that declared them.
- Removed the global `--unchecked` flag (and `-O0`)
- `unsafe fn` (its body is an unsafe context), `@ptrCast`, `@intToPtr`, `@ptrToInt`, `@assume` (checked in Debug)
- Reading an `extern union` field requires `unsafe`
- `*void` → `*T` is no longer implicit: `@ptrCast` in `unsafe`. Pointers no longer go through `as`.
- Build modes `-O Debug | ReleaseSafe | ReleaseFast` (Debug is the default and skips the optimizer)
- Debug builds fill `undefined` memory with `0xAA`; release modes keep it stable (scalars and small aggregates are zeroed)
- Reject `return &local`, `&local.f`, `&local[i]` and `local[..]` at compile time

### Milestone 4: memory and strings, with no hidden allocations (§4.13–4.14)

- `Allocator` interface: `alloc`, `create`, `free`, `destroy` (plus `realloc`, `dupe`), with out-of-memory as `error.OutOfMemory`
- Allocators in `std.heap` (a prelude struct until modules exist):
  - `c_allocator`, `page_allocator`
  - `Arena`
  - `FixedBuffer`
  - `DebugAllocator`, which reports leaks (numbered by allocation order) and panics on double frees, wrong-length frees, frees of foreign memory and uses after free (freed pages are poisoned, then made inaccessible; the runtime's fault handler names the freed block)
- `str` becomes `[]const u8`; slices are `{ ptr, len }` (16 bytes, no `cap`)
- Remove the hidden allocations from the earlier design:
  - template literals (now a compile error pointing at `fmt`)
  - `str + str` and `+=` on strings
  - array literals `[1, 2, 3]` (use `[_]T { ... }`, `&[_]T { ... }` or an `ArrayList`)
  - `.push()`/`.pop()` on slices, and the `alloc`/`free` builtins
  - prelude methods that allocated: `toUpperCase`, `repeat`, `replace`, `join` now take an allocator; `split` returns an iterator; `toString` is gone (use `fmt`)
  - `args()` returns the runtime's array instead of allocating
- Formatting:
  - `io.print("x = {}\n", .{x})` with format strings checked at compile time (`@format` in the compiler expands them into calls to the prelude's `fmt.format*` helpers)
  - `fmt.allocPrint`, `fmt.bufPrint`, `fmt.count`, `Writer` (`io.stdout`, `io.stderr`, `ArrayList(u8).writer()`)
  - these replace the `print(...)` builtin
- Containers that take an allocator: `ArrayList(T)`, `HashMap(K, V)` (keys hashed with `@hash`, compared with `@eql`)
- What this needed from elsewhere:
  - the generics part of milestone 6 that doesn't need an interpreter: comptime type and value parameters, `anytype`, `@TypeOf`, `-> type` functions whose body is `return struct { ... };`, and `List(i32) { ... }` literals
  - `let`/`var`, structs and enums declared inside a struct (`heap.Arena`, `io.stdout`); the prelude's are only emitted when used
  - global initializers made of constant structs, function names and `&global`
  - more than 8 integer / 8 float arguments between Flint functions (passed on the stack)
  - `try` inside a `defer` is rejected (it used to recurse forever in the compiler)
  - errors inside a generic instance also show the call that made it

### Milestone 5: modules and tooling (§4.10, §4.15, §4.16)

- `import std.io;` / `import net.http as web;`: one module per file, `pub` visibility (top-level declarations, methods, a type's `let`s; fields and variants are always visible), any declaration order across files, import cycles allowed
  - no implicit prelude: only compiler builtins are visible without an import (`mem.Allocator`, `list.ArrayList`)
  - globals are made when first used, so their order doesn't matter; one that depends on itself is an error
  - symbols carry the module path, so private names can repeat across modules
  - "did you forget the import" hints for `io.print` and `mem.Allocator`
- The prelude is split into `std` modules: `mem`, `heap`, `fmt`, `io`, `list`, `map`, `ascii`, `hash`, `testing`, embedded in the `flint` binary and compiled only where used
  - methods on built-in types are gone from the language (`fn str.trim` is an error); `str.trim()` → `mem.trim(s)`, `.split` → `mem.split`, `.indexOf` → `mem.find` (now `?usize`), `.toInt()` → `fmt.parseInt`, `u8.isDigit()` → `ascii.isDigit`, `toUpperCase` → `ascii.upperAlloc`, `join`/`repeat`/`replace` → `mem.*`
- `import c "stdio.h" as c;`: the system preprocessor plus a small declaration parser; functions (variadic too), structs, unions, enums, typedefs, extern variables, and macros that are constants; translated when used, with an error that says why when something can't be
- `test "name" { ... }` blocks and `flint test` (each test in its own process; `std.testing`)
- A `flint.toml` package manifest: path dependencies, a `[c]` table (sources, include, flags, link); `flint build` / `run -- args` / `test` without a file

### Milestone 6: comptime and generics (§4.11, §4.12)

- An IR interpreter for compile-time evaluation (now compiler/interp/): it runs the unoptimized IR with the backend's semantics, deterministic, no I/O or C calls, a 10 million step limit, checked memory (regions for frames, strings, globals and functions); a failure is a compile error with the chain of calls
- `comptime` parameters; monomorphized generics like `fn max(comptime T: type, ...)`
- Types as values: `let T = List(i32);` (in functions and at the top level), `T == u8`, `@typeName(T)`; a type can only be held in a `let`
- Type functions with logic in them (`if (bits <= 8) { return u8; }`), run by the interpreter; `let`s made of compile-time-known names are constants there, so types can use them
- Generic structs can declare `let`s and nested types (`Tree(i32).Node`), and see the type function's constants
- Constraints: `where <comptime bool>` after the return type, checked per new instance and reported at the call; `@isInt`, `@isFloat`, `@isSigned`, `@hasField`, `@hasMethod`, `@compileError`. `fmt.parseInt`, `fmt.parseFloat` and `mem.order` use it
- `comptime expr` (the whole expression after it), `comptime { }` in functions and at the top level, and precomputed tables: an aggregate result becomes read-only data with relocations for its pointers (comptime memory it points into becomes data too); functions only comptime code used aren't emitted
- `if (comptime target.os == .linux)` with `std.target`: the untaken branch isn't type-checked, but its names must exist
- SIMD vectors `@Vector(N, T)` (8 or 16 bytes): `+ - * / & | ^`, unary `-`, indexing, `.len`, `for`, `@splat`, `@reduceAdd`, each one NEON instruction; integer lanes wrap in every mode (§4.12)

### Milestone 7: self-hosting

- The runtime is Flint: `std/rt.fl` (program entry, output, panics, the test runner, the DebugAllocator's fault handler) replaces `rt/rt.c` and is compiled into every program, so `flint` is a single binary
- The compiler is Flint: `compiler/`, one directory per stage (syntax, modules, cimport, sema, ir, interp, opt, arm64, driver), no file over 1000 lines
  - it produces byte-identical IR, assembly and error messages to the original C compiler for every test program, and passes the whole test suite (`make test`)
  - it builds itself: stage 2 and stage 3 produce the same assembly (`make selfhost-check`); compiling itself takes about as long as the C compiler does (3.2s vs 3.0s)
- Bootstrapping: `make` starts from the release pinned in `bootstrap/release` (0.7.0, fetched by `bootstrap/fetch.sh`) or from `FLINT_BOOTSTRAP`; `make release` packages the binary
- What the port needed from the language and std:
  - `std.fs` (files, buffered writer, temp dirs) and `std.process` (run, getEnv, exePath)
  - `@embedFile("path")`
  - `@bitcast` between floats and integers of the same size
  - `x orelse { ... }` (a block that leaves)
  - optional pointers compare with each other (`?*T == ?*T`, `?*T == *T`)
  - if-expressions unify `[]T`/`[]const T`, a pointer and `null` (`?*T`), `T` and `?T`
  - calling a `noreturn` function ends the block; constant array literals in global initializers
- Published 0.7.0 and pinned it in `bootstrap/release`; the C compiler is retired (it's in git history before that)
