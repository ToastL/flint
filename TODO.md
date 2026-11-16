# Flint TODO

What's left to build, plus the known gaps in what exists. What's already done is in
[CHANGELOG.md](CHANGELOG.md). The design is [docs/design.md](docs/design.md); section numbers refer to it.

## Backend and performance

- [ ] The self-hosted compiler never frees memory (one process-wide heap); fine for one compile, but it could use arenas per function
- [ ] The linker's UUID makes two builds' binaries differ in a few header bytes (the assembly is identical)

- [ ] Return `!T`, `?T` and small structs in registers (x0–x3 / d0–d3) instead of through a hidden pointer
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
- [ ] Non-pointer optionals can only be compared with `null`, not with each other (`?*T` can)
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

## Open design questions

See §8 of the design. Still undecided:
- error payloads
- bounds-check cost in release builds
- whether `let n = 42;` should make a compile-time constant
- the automatic `&` in method calls
- semicolons
- declarative vs scripted build files
