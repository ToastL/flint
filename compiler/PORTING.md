# Port status (milestone 7) — temporary, delete when self-hosting works

Plan: ~/.claude/plans/okay-now-we-can-fizzy-oasis.md

Build the Flint compiler with the C one: `./build/flint build compiler/main.fl -o build/flint1`

## Done (checked against the C compiler)
- base/ (alloc: new, make, dupe, Vec; text: f, eq, z, g, Buf; diag: at, notes)
- syntax/ (token, lex, ast, parser + parse_type/expr/stmt/decl): `flint tokens` and `flint ast`
  output identical to build/flint for all 283 .fl files in the repo (error messages too)
- modules/ (unit, load), driver/manifest.fl, cimport/ (ctypes, clex, cparse, header): compile;
  `flint1 units file.fl` loads programs incl. C headers
- ir/, sema/ (24 files), interp/ (memory, calls, run): `tests/diff.sh` shows `flint ir -O Debug`
  output (and every compile error) identical to the C compiler for all 246 test programs.
  (Module names can't be keywords: sema/cases.fl is switch, sema/ctexec.fl is comptime.)

- opt/, arm64/, driver/: `flint ir` and `flint asm` identical in all modes for all programs; the
  test suite passes with the Flint compiler; it rebuilds itself to the same assembly.

## Next
- Publish the first release (needs a GitHub remote) and pin it in bootstrap/VERSION. Then this
  file can go.

## Conventions
- A zeroed struct is valid (Vec fields default empty; globals use @zeroes(T)).
- Absent names are "", absent nodes/pointers are ?*T (null). IR enums: Op, Cls (None = C's -1), CC, Vop.
- Errors: diag.at(tok, text.f("...", .{...})). Memory is never freed.
- Port C logic 1:1 so IR/asm stay byte-identical; printf %g -> text.g6.
- C setjmp/longjmp recovery -> error sets + try (see cimport/cparse.fl).
- Calling a variadic C function needs `...` in the extern (Apple passes varargs on the stack).

## Language changes made for the port (in the C compiler too, with tests)
@embedFile; std.fs, std.process; std.rt replaces rt.c; noreturn calls end blocks; constant array
globals; .zerofill for zero globals; if-expr unifies []T/[]const T, ptr/null (-> ?*T), T/?T;
`x orelse { ... }`; ?*T == ?*T; @bitcast between floats and ints. Fixed: comptime f64 formatting
used f32 precision; `open` must be declared variadic (std.fs).
