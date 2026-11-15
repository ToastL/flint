# Flint TODO

What's left to build, in roadmap order, plus the known gaps in what already exists.
The target design is [docs/design.md](docs/design.md); section numbers below refer to it.

## Milestone 1: tagged unions ✅

- [x] Enums: plain and tagged (tuple and struct payloads), methods, `@tagName`
- [x] Exhaustive `switch`: statement and expression, captures, ranges, multiple patterns, `else`, `fallthrough`
- [x] Optionals `?T`, non-null `*T`, `if (x) |v|`, `while (x) |v|`, `orelse`, `.?`
- [x] `noreturn` expressions (`return`, `break`, `continue`, `unreachable`, `panic`)
- [x] Error unions `E!T` / `!T`, error sets, `try`, `catch`, `errdefer`, `else |e|`, `main() -> !void`

## Milestone 2: finish the type system (§3) ✅

- [x] Fixed arrays `[N]T`
  - [x] value semantics: copied on assignment and when passed
  - [x] `.len`, and slicing `arr[..]`
  - [x] `*[N]T` → `[]T` coercion
- [x] Array literals `[N]T { ... }` / `[_]T { ... }` and fill values (`@splat`), allocated on the stack
- [x] `[*]T` many-item pointers. Indexing and arithmetic moved off `*T` onto `[*]T` (and need `unsafe` since milestone 3).
- [x] `cstr` for C interop
  - [x] string literals coerce to it
  - [x] `mem.span(c)` converts it to a string. `mem` is a prelude struct used as a namespace until modules exist (milestone 5), and it returns `str` until `str` becomes `[]const u8` (milestone 4).
- [x] Tuples `(A, B)`, multiple return values, `let (q, r) = divmod(a, b);`
- [x] Function pointer types `fn(i32) -> i32` and `?fn(...)`; passing function names as values
- [x] Anonymous literals `.{ x: 1 }` / `.{ 1, 2 }` that take their type from context
- [x] `usize`/`isize` as distinct types, and `.len` as `usize`. `for (0..n)` with literal bounds counts in `usize`.
- [x] Converting integers to enums (checked)
- [x] `if (x) |*v|`: capture an optional by pointer to change it in place
- [x] `packed struct` (bit fields, up to 64 bits), and `extern union` for C interop
- [x] `export` for variables; `extern` globals

## Milestone 3: the safety model (§6) ✅

- [x] Overflow traps on `+ - *` and negation in Debug and ReleaseSafe; wrapping in ReleaseFast and inside `unsafe`. Negating an unsigned value is a compile error.
- [x] Saturating operators `+| -| *|`, plus `@addWithOverflow` / `@subWithOverflow` / `@mulWithOverflow`
- [x] Checked `as`: integer narrowing and sign changes, float to int (NaN, out of range); constants that don't fit are compile errors. A runtime value that doesn't fit a packed bit field traps too.
- [x] Shift amounts must be unsigned; ≥ bit width traps in safe modes, taken modulo the width in ReleaseFast
- [x] Integer division by zero traps in every mode; `MIN / -1` traps in safe modes
- [x] `unsafe { }` blocks: unchecked indexing, `[*]T` indexing/slicing/arithmetic, pointer casts. Defers keep the safety of the scope that declared them.
- [x] Removed the global `--unchecked` flag (and `-O0`)
- [x] `unsafe fn` (its body is an unsafe context), `@ptrCast`, `@intToPtr`, `@ptrToInt`, `@assume` (checked in Debug)
- [x] Reading an `extern union` field requires `unsafe`
- [x] `*void` → `*T` is no longer implicit: `@ptrCast` in `unsafe`. Pointers no longer go through `as`.
- [x] Build modes `-O Debug | ReleaseSafe | ReleaseFast` (Debug is the default and skips the optimizer)
- [x] Debug builds fill `undefined` memory with `0xAA`; release modes keep it stable (scalars and small aggregates are zeroed)
- [x] Reject `return &local`, `&local.f`, `&local[i]` and `local[..]` at compile time
- [ ] Inline assembly `asm { }` (§6.1 item 7)

## Milestone 4: memory and strings, with no hidden allocations (§4.13–4.14) ✅

- [x] `Allocator` interface: `alloc`, `create`, `free`, `destroy` (plus `realloc`, `dupe`), with out-of-memory as `error.OutOfMemory`
- [x] Allocators in `std.heap` (a prelude struct until modules exist):
  - [x] `c_allocator`, `page_allocator`
  - [x] `Arena`
  - [x] `FixedBuffer`
  - [x] `DebugAllocator`, which reports leaks (numbered by allocation order) and panics on double frees, wrong-length frees, frees of foreign memory and uses after free (freed pages are poisoned, then made inaccessible; the runtime's fault handler names the freed block)
- [x] `str` becomes `[]const u8`; slices are `{ ptr, len }` (16 bytes, no `cap`)
- [x] Remove the hidden allocations from the earlier design:
  - [x] template literals (now a compile error pointing at `fmt`)
  - [x] `str + str` and `+=` on strings
  - [x] array literals `[1, 2, 3]` (use `[_]T { ... }`, `&[_]T { ... }` or an `ArrayList`)
  - [x] `.push()`/`.pop()` on slices, and the `alloc`/`free` builtins
  - [x] prelude methods that allocated: `toUpperCase`, `repeat`, `replace`, `join` now take an allocator; `split` returns an iterator; `toString` is gone (use `fmt`)
  - [x] `args()` returns the runtime's array instead of allocating
- [x] Formatting:
  - [x] `io.print("x = {}\n", .{x})` with format strings checked at compile time (`@format` in the compiler expands them into calls to the prelude's `fmt.format*` helpers)
  - [x] `fmt.allocPrint`, `fmt.bufPrint`, `fmt.count`, `Writer` (`io.stdout`, `io.stderr`, `ArrayList(u8).writer()`)
  - [x] these replace the `print(...)` builtin
- [x] Containers that take an allocator: `ArrayList(T)`, `HashMap(K, V)` (keys hashed with `@hash`, compared with `@eql`)
- [x] What this needed from elsewhere:
  - [x] the generics part of milestone 6 that doesn't need an interpreter: comptime type and value parameters, `anytype`, `@TypeOf`, `-> type` functions whose body is `return struct { ... };`, and `List(i32) { ... }` literals
  - [x] `let`/`var`, structs and enums declared inside a struct (`heap.Arena`, `io.stdout`); the prelude's are only emitted when used
  - [x] global initializers made of constant structs, function names and `&global`
  - [x] more than 8 integer / 8 float arguments between Flint functions (passed on the stack)
  - [x] `try` inside a `defer` is rejected (it used to recurse forever in the compiler)
  - [x] errors inside a generic instance also show the call that made it

## Milestone 5: modules and tooling (§4.10, §4.15, §4.16) ✅

- [x] `import std.io;` / `import net.http as web;`: one module per file, `pub` visibility (top-level declarations, methods, a type's `let`s; fields and variants are always visible), any declaration order across files, import cycles allowed
  - [x] no implicit prelude: only compiler builtins are visible without an import (`mem.Allocator`, `list.ArrayList`)
  - [x] globals are made when first used, so their order doesn't matter; one that depends on itself is an error
  - [x] symbols carry the module path, so private names can repeat across modules
  - [x] "did you forget the import" hints for `io.print` and `mem.Allocator`
- [x] The prelude is split into `std` modules: `mem`, `heap`, `fmt`, `io`, `list`, `map`, `ascii`, `hash`, `testing`, embedded in the `flint` binary and compiled only where used
  - [x] methods on built-in types are gone from the language (`fn str.trim` is an error); `str.trim()` → `mem.trim(s)`, `.split` → `mem.split`, `.indexOf` → `mem.find` (now `?usize`), `.toInt()` → `fmt.parseInt`, `u8.isDigit()` → `ascii.isDigit`, `toUpperCase` → `ascii.upperAlloc`, `join`/`repeat`/`replace` → `mem.*`
- [x] `import c "stdio.h" as c;`: the system preprocessor plus a small declaration parser; functions (variadic too), structs, unions, enums, typedefs, extern variables, and macros that are constants; translated when used, with an error that says why when something can't be
- [x] `test "name" { ... }` blocks and `flint test` (each test in its own process; `std.testing`)
- [x] A `flint.toml` package manifest: path dependencies, a `[c]` table (sources, include, flags, link); `flint build` / `run -- args` / `test` without a file

## Milestone 6: comptime and generics (§4.11, §4.12) ✅

- [x] An IR interpreter for compile-time evaluation (src/interp.c): it runs the unoptimized IR with the backend's semantics, deterministic, no I/O or C calls, a 10 million step limit, checked memory (regions for frames, strings, globals and functions); a failure is a compile error with the chain of calls
- [x] `comptime` parameters; monomorphized generics like `fn max(comptime T: type, ...)`
- [x] Types as values: `let T = List(i32);` (in functions and at the top level), `T == u8`, `@typeName(T)`; a type can only be held in a `let`
- [x] Type functions with logic in them (`if (bits <= 8) { return u8; }`), run by the interpreter; `let`s made of compile-time-known names are constants there, so types can use them
- [x] Generic structs can declare `let`s and nested types (`Tree(i32).Node`), and see the type function's constants
- [x] Constraints: `where <comptime bool>` after the return type, checked per new instance and reported at the call; `@isInt`, `@isFloat`, `@isSigned`, `@hasField`, `@hasMethod`, `@compileError`. `fmt.parseInt`, `fmt.parseFloat` and `mem.order` use it
- [x] `comptime expr` (the whole expression after it), `comptime { }` in functions and at the top level, and precomputed tables: an aggregate result becomes read-only data with relocations for its pointers (comptime memory it points into becomes data too); functions only comptime code used aren't emitted
- [x] `if (comptime target.os == .linux)` with `std.target`: the untaken branch isn't type-checked, but its names must exist
- [x] SIMD vectors `@Vector(N, T)` (8 or 16 bytes): `+ - * / & | ^`, unary `-`, indexing, `.len`, `for`, `@splat`, `@reduceAdd`, each one NEON instruction; integer lanes wrap in every mode (§4.12)

## Milestone 7: self-hosting ✅

- [x] The runtime is Flint: `std/rt.fl` (program entry, output, panics, the test runner, the DebugAllocator's fault handler) replaces `rt/rt.c` and is compiled into every program, so `flint` is a single binary
- [x] The compiler is Flint: `compiler/`, one directory per stage (syntax, modules, cimport, sema, ir, interp, opt, arm64, driver), no file over 1000 lines
  - [x] it produces byte-identical IR, assembly and error messages to the C compiler for every test program (`make diff-test`), and passes the whole test suite (`make test`)
  - [x] it builds itself: stage 2 and stage 3 produce the same assembly (`make selfhost-check`); compiling itself takes about as long as the C compiler does (3.2s vs 3.0s)
- [x] Bootstrapping: `make` starts from `FLINT_BOOTSTRAP` (a release binary, see `scripts/bootstrap.sh`) or from the frozen C compiler in `src/`; `make release` packages the binary
- [x] What the port needed from the language and std:
  - [x] `std.fs` (files, buffered writer, temp dirs) and `std.process` (run, getEnv, exePath)
  - [x] `@embedFile("path")`
  - [x] `@bitcast` between floats and integers of the same size
  - [x] `x orelse { ... }` (a block that leaves)
  - [x] optional pointers compare with each other (`?*T == ?*T`, `?*T == *T`)
  - [x] if-expressions unify `[]T`/`[]const T`, a pointer and `null` (`?*T`), `T` and `?T`
  - [x] calling a `noreturn` function ends the block; constant array literals in global initializers
- [ ] Publish the first release and pin it in `bootstrap/VERSION`; then the C compiler (`src/`) can be retired

## Backend and performance

- [ ] The self-hosted compiler never frees memory (one process-wide heap); fine for one compile, but it could use arenas per function
- [ ] Zero-size global data: zeroed globals go to `__bss` with `.zerofill` (done); the linker's UUID still makes two builds' binaries differ in a few header bytes

- [ ] Return `!T`, `?T` and small structs in registers (x0–x3 / d0–d3) instead of through a hidden pointer
- [x] Pass more than 8 integer / 8 float arguments on the stack (between Flint functions; calls to C are still limited to registers)
- [ ] Jump tables for dense `switch`es. They're compare chains today.
- [ ] Loop-invariant code motion
- [ ] Remove bounds checks in more loop shapes. n-body now costs nothing extra (0.90s checked vs 0.92s unchecked), but indexing with a loop-invariant offset still checks every time.
- [ ] Register allocation:
  - [ ] remove redundant `mov`s where control flow joins
  - [ ] split live intervals instead of spilling whole ones. This is most of ReleaseSafe's cost today: in n-body the `i + 1` overflow check lengthens the outer counter's interval and it gets spilled, so ReleaseSafe runs 1.87s vs 1.57s for ReleaseFast. The inner loop has no extra checks.
  - [ ] use x0–x7 for values that don't cross calls
- [ ] Leaf functions without a frame
- [ ] Keep `@Vector` values in SIMD registers between operations (today each operation loads, computes and stores)
- [ ] Ryu-style shortest float formatting: string building with floats is ~2× slower than Node
- [ ] An x86-64 / Linux (ELF) backend
- [ ] DWARF line info, so debuggers show Flint source lines
- [ ] Error return traces in Debug builds (§4.7)

## Known gaps in what exists today

- [ ] Overflow on two typed constants (`let a: u8 = 200; a + a`) traps at run time; it could be a compile error
- [ ] Union reads are tracked through field access and indexing, but not through every path (e.g. a method call on a union field's value)

- [ ] The compiler stops at the first error; it should report several
- [ ] Payloads captured in a `switch` (`.Rect(r)`) alias the original value instead of copying it
- [ ] With `anyerror`, any `error.Name` is accepted, so a typo silently creates a new error
- [ ] `return` inside a `defer` isn't rejected in every form (`try` is)
- [ ] Optionals can only be compared with `null`, not with each other
- [ ] `!T` always means `anyerror!T`; error sets aren't inferred from function bodies
- [ ] Errors carry no payload (open question §8.1)
- [ ] `{}` on a `cstr` prints its address; `{s}` is needed for the text
- [ ] `[][]u8` doesn't coerce to `[]const []const u8` (only the outer `const` can be added for free)
- [ ] The prelude's `io.stdout`/`heap.c_allocator` have a zero `ctx`, though `*void` promises non-null (it's never dereferenced)
- [ ] Allocator vtable calls aren't inlined, so `ArrayList.append` costs an indirect call when it grows
- [ ] Calls to C can't pass or return structs by value (C returns small structs in registers); such imported functions are rejected, and so is an `extern fn` that returns a struct
- [ ] `flint.toml` dependencies are local paths only; fetching by URL plus hash isn't there yet
- [ ] C import doesn't translate function-like macros, `static inline` functions, bit fields (the struct becomes opaque), anonymous struct members, or Objective-C blocks
- [ ] Assigning to a typed global from arithmetic on typed constants (`let b: P = .{ x: N * 2 }`) isn't folded, so it's rejected as non-constant (`comptime` works around it)
- [ ] A literal of a type held in a local `let` needs `.{ }`: `let h: T = .{ ... };` works, `T { ... }` doesn't parse (the parser can't know `T` is a type)
- [ ] The comptime step limit (10 million IR instructions) can't be raised yet; unoptimized IR makes deep recursion like `fib(30)` hit it
- [ ] Comptime code can't get memory from the system (`heap.page_allocator` calls `mmap`); a `heap.FixedBuffer` over a local array works
- [ ] A type function's `struct` that names its own instance (instead of `Self`) is an error when the body has logic in it; the plain `return struct { ... };` form allows it
- [ ] Inside a struct, its `let`s and functions are reached as `Self.name`, not by their bare name

## Tooling

- [ ] `flint fmt`
- [ ] `flint init` to start a package
- [ ] Editor support: syntax highlighting, then a language server
- [ ] CI that runs `make test` and the benchmarks

## Open design questions

See §8 of the design. Still undecided:
- error payloads
- bounds-check cost in release builds
- whether `let n = 42;` should make a compile-time constant
- the automatic `&` in method calls
- semicolons
- declarative vs scripted build files
