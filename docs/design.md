# Flint: Language Design

Flint is C with its historical accidents fixed, keeping its philosophy.

**Guiding principle.** What you write is roughly what the machine does. A competent reader can look at any line and predict the machine code to within a few instructions. Concretely:

- no garbage collector
- no hidden allocations
- no exceptions or stack unwinding
- no hidden control flow
- a tiny runtime
- trivial interoperability with C code and C ABIs

When two goals conflict, this document prefers **predictability and readability over cleverness**, and says so.

Contents:

1. Design rationale
2. Grammar
3. Type system
4. Semantics, by feature
5. C vs Flint, side by side
6. Unsafe operations, and checked vs unchecked behavior
7. Comparison with Zig, Odin, C3 and Hare
8. Open questions and trade-offs
9. Appendix: resolving conflicts in the brief, and migrating the current compiler

---

## 1. Design rationale

| Decision | The C problem it fixes |
|---|---|
| **Name first, type after** (`let p: *i32`), read left to right | C's "declaration mirrors use" syntax gives `int *a, b;` (only `a` is a pointer) and the spiral rule for `void (*signal(int, void (*)(int)))(int)`. In Flint every type is one left-to-right phrase, including function types. |
| **`let` immutable by default, `var` mutable** | C makes mutability the default and `const` an afterthought that's easy to cast away. Here, mutation is the thing you have to ask for. |
| **Fixed-width integers only**: `i8`…`i64`, `u8`…`u64`, `usize`, `isize` | `int`, `long` and `short` have platform-dependent sizes, which makes code non-portable and causes overflow bugs. |
| **No implicit narrowing, sign changes or integer promotion** | C silently promotes `u8 + u8` to `int`, converts `-1` to `UINT_MAX` in comparisons, and truncates on assignment. Each of these is a well-known bug class. |
| **Overflow traps in safe builds; `+%` (wrap) and `+\|` (saturate) are explicit** | In C, signed overflow is undefined behavior that the optimizer exploits, and unsigned overflow wraps silently. Flint asks you to say what you mean. |
| **Arrays are values; slices `[]T` carry their length** | Arrays decaying to pointers and (pointer, length) parameter pairs are the source of most buffer overflows. |
| **Bounds checks on by default; turned off only in `unsafe` blocks** | Unchecked indexing is C's largest source of CVEs. Turning a check off becomes a visible, local decision. |
| **Strings are `[]const u8`; `cstr` exists only for C interop** | NUL-terminated strings make `strlen` O(n), break on embedded zero bytes, and gave us `strcpy` and `gets`. |
| **Non-null pointers; `?T` and `?*T` for absence** | Null dereference is the "billion-dollar mistake". The type now says whether null is possible, and the compiler forces a check. |
| **Errors are values: `!T`, `try`, `defer`, `errdefer`** | `errno`, magic `-1` returns and `goto cleanup` chains are easy to get wrong and easy to ignore. Error unions are just tagged return values: no unwinding and no hidden cost. |
| **No preprocessor; modules plus `comptime`** | Textual macros ignore scope and types, headers duplicate declarations, and include order matters. Compile-time evaluation of ordinary code replaces all of it. |
| **Tagged unions and an exhaustive `switch`** | C unions plus a hand-maintained tag field get out of sync. A forgotten `case` or an accidental fallthrough is a silent bug. |
| **Syntax fixes**: precedence, mandatory braces, assignment as a statement, no comma operator, mandatory initialization | Each fixes a classic bug: `x & MASK == 0`, `goto fail`, `if (x = 5)`, `a, b` surprises, and uninitialized reads. |
| **A small, enumerated set of undefined behavior, all inside `unsafe`** | C has over 200 kinds of UB, and optimizers delete safety checks by reasoning from UB elsewhere in the program. |
| **Allocators are explicit parameters** | `malloc` hidden inside library functions makes memory ownership and cost invisible, and makes arenas or pools impossible to substitute. |
| **A build system and C header import in the toolchain** | Makefiles and CMake exist only because C has no module system or package format. |

**Out of scope, deliberately:** classes and inheritance, virtual methods, garbage collection, exceptions, general operator overloading, and a borrow checker. Each adds hidden control flow, hidden cost or a different language's trade-offs. The one concession to math code is built-in SIMD vector types (section 4.12); user-defined operator overloading isn't allowed.

---

## 2. Grammar

EBNF. `{ x }` means zero or more, `[ x ]` means optional, and `|` means alternation. Terminals are quoted. Comments are `//` to end of line. Statements end in `;` except those that end in a block.

```ebnf
(* ---------- files and declarations ---------- *)
file          = { import } { top_decl } ;
import        = "import" module_path [ "as" IDENT ] ";"
              | "import" "c" STRING "as" IDENT ";" ;              (* C header import *)
module_path   = IDENT { "." IDENT } ;

top_decl      = [ "pub" ] ( fn_decl | struct_decl | enum_decl | error_decl
                          | union_decl | global_decl | extern_global | test_decl )
              | comptime_block ;                                 (* runs while compiling *)
global_decl   = [ "export" ] ( "let" | "var" ) IDENT [ ":" type ] "=" expr ";" ;
extern_global = "extern" ( "let" | "var" ) IDENT ":" type ";" ;   (* defined in C *)
test_decl     = "test" STRING block ;

fn_decl       = [ "export" | "extern" [ STRING ] ] [ "unsafe" ] [ "inline" ]
                "fn" IDENT "(" [ params ] ")" [ "->" type ] [ "where" expr ] ( block | ";" ) ;
                                                         (* where: a comptime bool about the comptime params *)
params        = param { "," param } [ "," ] ;
param         = [ "comptime" ] IDENT ":" type | "..." ;

struct_decl   = [ "packed" ] "struct" IDENT struct_body ;
struct_body   = "{" [ field { "," field } [ "," ] ] { member } "}" ;
field         = IDENT ":" type [ "=" expr ] ;
member        = [ "pub" ] ( fn_decl | ( "let" | "var" ) IDENT [ ":" type ] "=" expr ";"
                          | struct_decl | enum_decl ) ;       (* Type.name, Outer.Inner *)

enum_decl     = "enum" IDENT [ ":" int_type ] enum_body ;
enum_body     = "{" variant { "," variant } [ "," ] { member } "}" ;
variant       = IDENT [ "(" type { "," type } ")"                (* tuple payload  *)
                      | "{" field { "," field } [ "," ] "}" ]   (* struct payload *)
                      [ "=" expr ] ;                            (* explicit tag   *)

error_decl    = "error" IDENT "{" IDENT { "," IDENT } [ "," ] "}" ;
union_decl    = "extern" "union" IDENT "{" field { "," field } [ "," ] "}" ;  (* C interop only *)

(* ---------- types ---------- *)
type          = "?" type                                 (* optional              *)
              | [ type_name ] "!" type                   (* error union: E!T, !T  *)
              | "*" [ "const" ] type                     (* single-item pointer   *)
              | "[*]" [ "const" ] type                   (* many-item pointer     *)
              | "[" "]" [ "const" ] type                 (* slice                 *)
              | "[" expr "]" type                        (* array, length comptime *)
              | "fn" "(" [ type { "," type } [ "," "..." ] ] ")" [ "->" type ]
              | "(" type "," type { "," type } ")"       (* tuple                 *)
              | "@" "Vector" "(" expr "," type ")"       (* SIMD vector           *)
              | "struct" struct_body                     (* anonymous: made by a type function *)
              | type_name [ "(" expr { "," expr } ")" ]  (* generic instantiation *)
              | "type" | "void" | "noreturn" | "bool" | "cstr" | int_type | "f32" | "f64" ;
type_name     = IDENT { "." IDENT } | "Self" ;
int_type      = "i8" | "i16" | "i32" | "i64" | "u8" | "u16" | "u32" | "u64" | "isize" | "usize" ;

(* ---------- statements ---------- *)
block         = "{" { stmt } "}" ;
stmt          = local_decl | assign | expr_stmt | if_stmt | while_stmt | for_stmt
              | switch_stmt | return_stmt | break_stmt | continue_stmt | defer_stmt
              | errdefer_stmt | unsafe_block | comptime_block | block | discard ;
local_decl    = ( "let" | "var" ) binding [ ":" type ] "=" expr ";" ;
binding       = IDENT | "(" IDENT { "," IDENT } ")" ;        (* tuple destructuring *)
assign        = lvalue assign_op expr ";" ;
assign_op     = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>="
              | "+%=" | "-%=" | "*%=" | "+|=" | "-|=" | "*|=" ;
lvalue        = IDENT | postfix_expr ;                       (* must denote a var location *)
discard       = "_" "=" expr ";" ;
expr_stmt     = expr ";" ;                                   (* calls only; values can't be dropped silently *)

if_stmt       = "if" "(" expr ")" [ capture ] block
                [ "else" [ capture ] ( if_stmt | block ) ] ;
capture       = "|" [ "*" ] IDENT { "," IDENT } "|" ;
while_stmt    = [ IDENT ":" ] "while" "(" expr ")" [ capture ] block ;
for_stmt      = [ IDENT ":" ] "for" "(" expr ")" capture block ;   (* slice, array or range *)
switch_stmt   = "switch" "(" expr ")" "{" { arm } "}" ;
arm           = pattern { "," pattern } "=>" ( block [ "," ] | simple_stmt "," ) ;
simple_stmt   = assign_nosemi | expr ;
pattern       = "." IDENT [ "(" IDENT { "," IDENT } ")" ]       (* enum / error tag *)
              | const_expr [ "..=" const_expr ]                 (* value or inclusive range *)
              | "else" ;
return_stmt   = "return" [ expr ] ";" ;
break_stmt    = "break" [ ":" IDENT ] ";" ;
continue_stmt = "continue" [ ":" IDENT ] ";" ;
defer_stmt    = "defer" ( block | simple_stmt ";" ) ;
errdefer_stmt = "errdefer" [ capture ] ( block | simple_stmt ";" ) ;
unsafe_block  = "unsafe" block ;
comptime_block= "comptime" block ;
(* inside a block arm of a switch, the last statement may be "fallthrough;" *)

(* ---------- expressions, lowest precedence first ---------- *)
expr          = if_expr | switch_expr | catch_expr ;
if_expr       = "if" "(" expr ")" [ capture ] expr "else" expr ;    (* else is mandatory *)
switch_expr   = "switch" "(" expr ")" "{" { pattern { "," pattern } "=>" expr "," } "}" ;
catch_expr    = or_expr { ( "orelse" | "catch" [ capture ] ) or_expr } ;
or_expr       = and_expr { "||" and_expr } ;
and_expr      = cmp_expr { "&&" and_expr } ;
cmp_expr      = bitor_expr [ cmp_op bitor_expr ] ;               (* non-associative *)
cmp_op        = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
bitor_expr    = bitxor_expr { "|" bitxor_expr } ;
bitxor_expr   = bitand_expr { "^" bitand_expr } ;
bitand_expr   = shift_expr { "&" shift_expr } ;
shift_expr    = add_expr { ( "<<" | ">>" ) add_expr } ;
add_expr      = mul_expr { ( "+" | "-" | "+%" | "-%" | "+|" | "-|" ) mul_expr } ;
mul_expr      = cast_expr { ( "*" | "/" | "%" | "*%" | "*|" ) cast_expr } ;
cast_expr     = prefix_expr { "as" type } ;
prefix_expr   = ( "-" | "!" | "~" | "&" | "try" ) prefix_expr
              | "comptime" expr                          (* the whole expression after it *)
              | postfix_expr ;
postfix_expr  = primary { "(" [ args ] ")" | "[" expr "]" | "[" [ expr ] ".." [ expr ] "]"
                        | "." IDENT | ".*" | ".?" } ;
args          = expr { "," expr } [ "," ] ;
primary       = INT | FLOAT | CHAR | STRING | "true" | "false" | "null" | "undefined"
              | "unreachable" | "return" [ expr ] | "break" [ ":" IDENT ] | "continue"
              | IDENT | "Self" | "(" expr ")" | builtin_call
              | type_name "{" [ field_init { "," field_init } [ "," ] ] "}"   (* struct literal *)
              | "[" ( expr | "_" ) "]" type "{" [ args ] "}"                  (* array literal; [_] counts the elements *)
              | "." "{" [ field_init { "," field_init } | args ] "}"          (* inferred literal *)
              | "." IDENT [ "(" args ")" | "{" field_init { "," field_init } "}" ]  (* enum / error tag *)
              | "error" "." IDENT
              | range ;
field_init    = IDENT ":" expr ;
range         = expr ".." expr | expr "..=" expr ;   (* only in for headers and slicing *)
builtin_call  = "@" IDENT "(" [ args ] ")" ;
```

**Lexical notes.**
- Integer literals: `42`, `0x2A`, `0b101010`, `0o52`, with `_` allowed as a separator.
- Float literals: `1.5`, `1e-9`.
- Character literals: `'a'`, which has type `u8`.
- Strings: `"…"` with the escapes `\n \t \r \\ \" \' \0 \xHH`.
- Keywords: `fn let var pub export extern struct enum error union if else while for switch return break continue defer errdefer try catch orelse unsafe comptime where import as test true false null undefined unreachable fallthrough noreturn type Self inline packed`.

---

## 3. Type system

### 3.1 Primitives

| Type | Size | C equivalent | Notes |
|---|---|---|---|
| `i8 i16 i32 i64` | 1/2/4/8 | `int8_t`… | two's complement |
| `u8 u16 u32 u64` | 1/2/4/8 | `uint8_t`… | |
| `isize usize` | pointer size | `ptrdiff_t`, `size_t` | lengths and indexes are `usize` |
| `f32 f64` | 4/8 | `float`, `double` | IEEE 754 |
| `bool` | 1 | `bool` | only `true`/`false`; no int ↔ bool coercion |
| `void` | 0 | `void` | |
| `noreturn` | 0 | `_Noreturn` | type of `return`, `break`, `unreachable`, `panic(...)` |
| `type` | comptime only | – | types are values at compile time |

**Literals have no runtime type.** Integer literals are compile-time integers of unbounded size, and take on the type their context requires. A literal that doesn't fit is a compile error: `let x: u8 = 300;` fails. Float literals are compile-time floats.

### 3.2 Pointers

| Type | Meaning | Index? | Arithmetic? | Null? |
|---|---|---|---|---|
| `*T` | exactly one `T` | no | no | never |
| `*const T` | one `T`, read-only through this pointer | no | no | never |
| `?*T` | one `T`, or null | after unwrapping | no | yes; same size as `*T`, `null` = 0 |
| `[*]T` | unknown number of `T` (C's `T*`) | `unsafe` only | `unsafe` only | never (`?[*]T` may be) |
| `*void` | address of unknown type | no | no | never |

Rules:
- Field access auto-dereferences a single pointer level: `p.x` where `p: *Point`.
- Explicit dereference is postfix, `p.*`, so it reads left to right: `node.next.*.value`.
- `&x` on a `let` binding gives `*const T`. On a `var` it gives `*T`.

### 3.3 Arrays and slices

- `[N]T` is a value type: assigning or passing it copies all N elements, and it never decays to a pointer. `N` must be known at compile time.
- `[]T` is a slice, `{ ptr: [*]T, len: usize }`, 16 bytes, passed in two registers. `[]const T` is the read-only version.
- `.len` is a field of both arrays and slices.
- Slicing: `a[i..j]`, `a[i..]` and `a[..j]` work on arrays, slices and (in `unsafe` code only) `[*]T`. The result is a slice that points into the original; nothing is copied.
- `&arr` (type `*[N]T`) coerces to `[]T`.

### 3.4 Strings

- A string is just `[]const u8`. String literals have that type, and are stored with a hidden trailing NUL so they also coerce to `cstr`.
- `cstr` is `[*]const u8` with the extra promise of a NUL terminator, for C interop only. Converting `cstr → []const u8` is explicit and O(n): `mem.span(c)`. Converting `[]const u8 → cstr` requires a copy: `mem.dupeZ(alloc, s)` (freed with `mem.freeZ(alloc, c)`).

### 3.5 Structs

```
struct Point { x: f32, y: f32 = 0 }
```
- **Fields are laid out in declaration order with C alignment rules.** No reordering, so every struct is C-compatible. This trades a few padding bytes for predictability (see section 8).
- `packed struct` has no padding and allows bit-width integer fields (`u3`, `i5`), to replace C bitfields. Its fields sit at bit offsets in one backing integer (the smallest of `u8`…`u64` that fits; at most 64 bits), first field in the lowest bits. Fields can be integers, `bool` (1 bit), plain enums and other packed structs. A bit field has no address, and `@bitcast` converts the whole struct to or from its backing integer. Bit-width integer types exist only as packed struct fields; reading one gives the next full-width type (`u3` reads as `u8`).
- Every field must be initialized in a literal unless it has a default value.

### 3.6 Enums and sum types

```
enum Color: u8 { Red = 1, Green, Blue }        // plain enum, explicit backing type
enum Shape { Circle(f32), Rect { w: f32, h: f32 } }
```
- A plain enum is an integer with named values.
- An enum with payloads is a tagged union. Its layout is `{ tag, union of payloads }`, with the tag as the smallest integer type that fits, so its size is predictable.
- Variants are written `Shape.Circle(1.0)`, or `.Circle(1.0)` when the type is known from context.

### 3.7 Error sets and error unions

```
error FileError { NotFound, AccessDenied }
```
- An error set is an enum whose tags live in one **global** error namespace. Every error has a single program-wide `u16` code, so any error set converts to `anyerror` for free.
- `E!T` holds either a `T` or an error from set `E`. `!T` is short for `anyerror!T`.
- Layout is `{ value: T, err: u16 }`. `err == 0` means success. It's returned in registers when it fits (`x0`/`x1` on ARM64).
- An error union can't be ignored. Using its value requires `try`, `catch` or `if/else |err|`. Writing `_ = f();` on an error union is a compile error; use `f() catch {}` to ignore an error on purpose.

### 3.8 Optionals

`?T` holds either a `T` or `null`.
- For pointer types (`?*T`, `?[*]T`, `?fn(...)`) the null value is address 0, so the optional costs nothing extra.
- For other types, layout is `{ value: T, present: bool }`.

### 3.9 Function types

`fn(i32, i32) -> i32` is the type of a function pointer. Omitting `-> T` means `-> void`, so `fn(i32)` is a pointer to a void function. Function pointers are non-null; use `?fn(i32)` for a nullable one. A function name used as a value coerces to its function type.

### 3.10 Tuples

`(u32, bool)` is an anonymous struct with fields `.0` and `.1`. Tuples exist mainly for multiple return values.

### 3.11 Implicit coercions (the complete list)

Every implicit conversion below is lossless and free (zero instructions, or one extension):

1. a literal to any numeric type it fits
2. an integer to a wider integer of the same signedness; an unsigned integer to a strictly wider signed one. `usize`/`isize` count as "at least 32 and at most 64 bits", so the rule is portable: `u32 → usize` and `usize → u64` are free, but `u64 → usize` needs `as`
3. `f32` to `f64`
4. `*T` to `*const T`, `[]T` to `[]const T`, `*T` to `*void`
5. `*[N]T` to `[]T`; a string literal to `[]const u8` or `cstr`
6. `T` to `?T` and `T` to `E!T`; an error set to `anyerror`; a subset error set to a superset
7. in binary operators, the narrower operand widens per rules 1–3 ("peer types"). Mixing signed and unsigned operands is an error.

Everything else needs an explicit cast:
- `x as T` is a **checked** numeric conversion. It traps in safe modes if the value doesn't fit; float-to-int traps on NaN or out of range.
- `@truncate(T, x)` deliberately keeps only the low bits.
- `@bitcast(T, x)` reinterprets bits between same-size number types, integers and floats alike (`@bitcast(u64, 1.0)` is `0x3ff0000000000000`); with pointer types it's allowed only in `unsafe`.
- `@ptrCast(*U, p)` and `@intToPtr(*T, n)` are `unsafe` only. `@ptrToInt(p)` is safe.

---

## 4. Semantics, by feature

### 4.1 Bindings and initialization

```
let limit: u32 = 100;          // immutable
var total: u64 = 0;            // mutable
var buf: [256]u8 = undefined;  // explicitly uninitialized (var only)
let zero = @zeroes(Point);     // all-zero value of any type
let n = 42;                    // n is a compile-time constant (untyped literal, never changes)
var m = 42;                    // ERROR: give m a type: `var m: i32 = 42;`
```
- Every binding must be initialized.
- `undefined` must be written explicitly, and is allowed only for `var`, since an immutable binding with no value is pointless.
- Reading `undefined` memory gives an unspecified but fixed value: never UB, never "time travel". Debug builds fill it with `0xAA` so such bugs show up.

### 4.2 Functions, multiple returns, function pointers

```
fn divmod(a: u32, b: u32) -> (u32, u32) {
    return .{ a / b, a % b };
}
let (q, r) = divmod(17, 5);

fn on_signal(sig: i32) { log.warn("signal {}", .{sig}); }
let handler: fn(i32) = on_signal;
fn signal(sig: i32, h: fn(i32)) -> fn(i32) { ... }   // vs C: void (*signal(int, void (*)(int)))(int)
```
- Parameters are immutable bindings.
- Struct and array arguments are passed by value, following the C ABI: small ones in registers, large ones by hidden pointer to a copy. Pass `*T` when you want a reference.
- A non-void result can't be dropped silently: `_ = f();`.

### 4.3 Methods (namespaced functions)

```
struct Vec2 {
    x: f32,
    y: f32,

    fn len(self: Vec2) -> f32 { return @sqrt(self.x * self.x + self.y * self.y); }
    fn scale(self: *Vec2, k: f32) { self.x *= k; self.y *= k; }
}
var v = Vec2 { x: 3, y: 4 };
v.scale(2);          // exactly Vec2.scale(&v, 2): a direct call, no dispatch
let l = v.len();     // exactly Vec2.len(v)
```
There's no `this`, no virtual dispatch and no inheritance. `x.f(...)` is pure syntax for `T.f(x, ...)`. It takes `&x` automatically only when `f`'s first parameter is `*T`, and then `x` must be a `var`. That automatic `&` is the only implicit step in the language (see section 8).

### 4.4 Integers and arithmetic

```
let a: u8 = 200;
let b: u8 = 100;
let c = a + b;          // traps in Debug/ReleaseSafe: 300 overflows u8
let d = a +% b;         // 44 (wrapping, always)
let e = a +| b;         // 255 (saturating, always)
let (sum, overflowed) = @addWithOverflow(a, b);   // (44, true)
let w: u32 = a;         // fine: u8 -> u32 is lossless
let n: u8 = w as u8;    // checked: traps if w > 255
let lo: u8 = @truncate(u8, w);   // keeps the low 8 bits on purpose
```
- `/` and `%` on integers trap on a divisor of 0 in **every** build mode; it's one compare.
- `x << n` requires an unsigned `n`. If `n` ≥ the bit width it traps in safe modes, and in ReleaseFast the shift amount is taken modulo the bit width (section 6).
- `>>` is arithmetic for signed types and logical for unsigned.
- Unary `-` on an unsigned value is a compile error; write `0 -% x` to wrap on purpose.
- There's no `++`/`--`; write `x += 1`.

### 4.5 Arrays, slices and bounds checks

```
var buf: [4]u8 = .{ 1, 2, 3, 4 };
let all: []u8 = buf[..];     // view of the whole array
let mid = buf[1..3];         // {2, 3}
buf[4] = 0;                  // compile error: constant index out of range
fn sum(xs: []const i32) -> i64 {
    var s: i64 = 0;
    for (xs) |x| { s += x; }
    return s;
}
```
Every index into an array or slice is checked unless the compiler can prove it's in range; loop indices over `xs.len` are proven. To remove checks you've measured as a cost:
```
unsafe {
    for (0..n) |i| { dst[i] = src[i] * k; }   // unchecked inside this block only
}
```

### 4.6 Optionals

```
fn find(users: []User, id: u64) -> ?*User {
    for (users) |*u| {
        if (u.id == id) { return u; }
    }
    return null;
}
if (find(users, 42)) |u| {
    try io.print("{s}\n", .{u.name});
} else {
    try io.print("not found\n", .{});
}
let name = if (find(users, 7)) |u| u.name else "nobody";
let port = config.port orelse 8080;
let u = find(users, 1).?;   // asserts non-null: traps if null, in every mode
let v = find(users, 2) orelse {
    try io.print("no user 2\n", .{});
    return;                 // an orelse block has no value, so it must leave
};
```
- An optional compares with `null`. Optional pointers also compare with each other and with plain pointers (`find(a, 1) == find(b, 1)`): null is address 0, so it's one compare. Other optionals don't compare with `==`; unwrap them first.
- An if-expression whose branches are a pointer and `null` has type `?*T`; one whose branches are `T` and `?T` has type `?T`.

### 4.7 Errors, `try`, `defer`, `errdefer`

```
error ConfigError { Missing, Malformed }

fn load(path: []const u8, alloc: Allocator) -> !Config {
    let f = try fs.open(path);           // on error: return it to the caller
    defer f.close();                     // runs on every exit from this scope
    let text = try f.readAll(alloc);
    defer alloc.free(text);
    var cfg = try Config.init(alloc);
    errdefer cfg.deinit();               // runs only if we leave with an error
    try cfg.parse(text);
    return cfg;
}

let cfg = load("app.conf", alloc) catch |err| switch (err) {
    error.NotFound => Config.default(),
    else => return err,
};
```
- `try e` is exactly `e catch |err| return err` (after any `errdefer`s run). It's an early return with a visible keyword.
- `defer`s run in reverse order when the scope exits. `errdefer` runs only when the exit is an error return.
- Debug builds keep a small fixed-size **error return trace**: the `try` sites an error passed through, printed if `main` returns it. There's no unwinding.

### 4.8 Sum types and `switch`

```
enum Shape { Circle(f32), Rect { w: f32, h: f32 }, Empty }

fn area(s: Shape) -> f32 {
    return switch (s) {
        .Circle(r) => PI * r * r,
        .Rect(rect) => rect.w * rect.h,   // a struct payload binds as a struct
        .Empty => 0,
    };
}

switch (code) {
    0 => log.info("ok", .{}),
    1..=9 => { retries += 1; fallthrough; }   // explicit: continues into the next arm
    10 => log.warn("giving up", .{}),
    else => return error.Unknown,
}
```
- `switch` must be exhaustive: a missing variant is a compile error. `else` covers the rest, but for enums the compiler warns when `else` hides variants added later.
- There's no implicit fallthrough. `fallthrough;` must be the last statement of a block arm, and the next arm can't capture payloads.
- Switching on an integer compiles to a jump table or compare chain, depending on density. The choice is documented, not left to chance.

### 4.9 Loops

```
for (items) |item| { ... }          // by value
for (items) |item, i| { ... }       // with index (usize)
for (items) |*item| { item.* += 1; }   // by pointer, to mutate
for (0..n) |i| { ... }              // half-open range; i is usize when both bounds are literals
outer: for (rows) |row| {
    for (row) |cell| {
        if (cell == target) { break :outer; }
    }
}
while (it.next()) |tok| { ... }     // loops while the optional is non-null
while (running) { ... }
```
Labelled `break`/`continue` replace C's legitimate uses of `goto`.

### 4.10 Modules and visibility

```
// file: net/http.fl
import std.mem;
pub fn get(url: []const u8, alloc: mem.Allocator) -> ![]u8 { ... }
fn helper() { ... }     // private to this file

// file: main.fl
import net.http;
import std.io as out;   // `as` picks the name the file uses
let body = try http.get("https://example.com", alloc);
```
- One file is one module. A dotted path names a file under the package root: `net.http` is `net/http.fl`, next to the root file (`flint.toml` says which file that is). `std.x` is the standard library, which is compiled into the `flint` binary. A dependency's name (from `flint.toml`) names its root module, and `dep.x` names a file inside it.
- Imports come first in a file. An import binds the last part of the path (`http`), or the name after `as`. A local variable with the same name hides it, but a top-level declaration with that name is an error.
- There's no implicit prelude: apart from the compiler's builtins (`panic`, `assert`, `args`, `sqrt`, ...), every name used in a file is declared in it or reached through one of its imports. `Allocator` is `mem.Allocator`, `ArrayList` is `list.ArrayList`.
- Declarations are private unless marked `pub`: functions, types, error sets, globals, and a type's methods and `let`s. Struct fields and enum variants are always visible: a type's layout isn't a secret in a language with value semantics.
- Every name is declared exactly once, there are no header files, and declaration order doesn't matter, within a file or across files. Imports may form cycles. A global's initializer is evaluated when the global is first needed; one that depends on itself is an error.
- Symbols are prefixed with the module's path (`net.http`'s `get` is `_fl_net_http__get`), so two modules can both have a private `helper`. The standard library's functions are compiled only when a program uses them.
- The standard library: `std.mem` (Allocator, functions on slices and byte strings), `std.heap` (allocators), `std.fmt` (Writer, formatting, parsing numbers), `std.io`, `std.list` (ArrayList), `std.map` (HashMap), `std.ascii`, `std.hash`, `std.testing`.
- Functions can't be attached to built-in types (there's no `fn str.trim`): `x.f()` always means `T.f(x)` for a struct or enum `T` that you can go and read. Functions on byte strings live in `std.mem`: `mem.trim(s)`, `mem.split(s, ",")`.

### 4.11 comptime and generics

```
fn max(comptime T: type, a: T, b: T) -> T {
    return if (a > b) a else b;
}
let m = max(u32, 3, 9);

fn List(comptime T: type) -> type {
    return struct {
        items: []T,
        len: usize,
        alloc: Allocator,

        fn init(alloc: Allocator) -> Self { return Self { items: &.{}, len: 0, alloc: alloc }; }
        fn push(self: *Self, x: T) -> !void {
            if (self.len == self.items.len) { try self.grow(); }
            self.items[self.len] = x;
            self.len += 1;
        }
        fn grow(self: *Self) -> !void { ... }
    };
}
var xs = List(i32).init(alloc);
try xs.push(5);

let table = comptime crc32_table();     // runs at compile time, stored as data

if (comptime target.os == .linux) {
    linux.futex_wait(addr, val);
} else {
    darwin.ulock_wait(addr, val);
}
```
- Generics are monomorphized: each distinct set of comptime arguments produces one copy, so costs are visible.
- A comptime parameter is `comptime T: type` or a constant of another type (`comptime n: usize`, `comptime fmt: []const u8`); its argument must be known at compile time. An `anytype` parameter takes its type from the argument (`@TypeOf(x)` names it). Comptime parameters must come before the parameters whose types use them.
- An instance of a type function is a struct named after its arguments (`List(i32)`). The same arguments give the same type. Inside it, `Self` is that struct, and `List(i32) { ... }` is a literal of it. A type function is only analyzed when it's called, and an instance's methods only when they're used. Its struct can declare `let`s and types like any struct (`Tree(i32).Node`).

**Comptime code is ordinary Flint, run by an interpreter inside the compiler.** The interpreter runs the same IR the backend compiles, unoptimized, so code behaves the same at compile time as at run time: overflow, bounds and `unreachable` checks are the same instructions, and a failed one is a compile error that shows the chain of calls that led to it. It's deterministic, can't do I/O or call C (`rt_write`, `malloc`, any `extern fn`: an error saying so), and stops after 10 million instructions. Memory it touches is checked: reading a `var` global (its value is only known at run time), writing any global, or keeping a pointer to a local of a function that returned are errors, not UB.

- `comptime expr` runs the whole expression after it while compiling. A number, bool, enum or type becomes a constant (`let n = comptime f();` can size an array). Anything else becomes read-only data, and the pointers inside it become relocations: a string points at the string's bytes, and comptime memory a slice points into becomes data of its own. That's how precomputed tables work (`let CRC = comptime crcTable();` is a `[256]u32` in the data section, and `crcTable` itself is only emitted if run-time code calls it).
- `comptime { ... }` runs a block for its effects: `assert`s about sizes and layouts, or `@compileError("...")`. It can appear in a function or at the top level.
- `@embedFile("path")` is a file's bytes, read while compiling, as a `[]const u8` constant (in read-only data). The path is relative to the directory of the file that uses it. It works in globals, struct fields of globals, and comptime code: `let SHADER = @embedFile("shader.metal");`.
- Comptime code sees the constants around it (comptime parameters, `let`s with constant values, globals declared with `let`) but not run-time variables: `comptime n + 1` with `var n` is an error.
- **Types are compile-time values.** `let T = List(i32);` names a type (only a `let`: a type can't be stored in memory); `T == u8` compares types; `@typeName(T)` is its name as a string. A type function can contain any logic, since it runs in the interpreter:
  ```
  fn Int(comptime bits: u16) -> type {
      if (bits <= 8) { return u8; }
      if (bits <= 16) { return u16; }
      return u64;
  }
  ```
  In a type function, a `let` whose value uses only compile-time-known names is evaluated when the function is compiled, so types can use it (`let words = (n + 63) / 64; return struct { w: [words]u64 };`). Each `struct { ... }` in its body is made once per instance; a struct that needs its own instance by name uses `Self` instead.
- **Constraints:** `where` after the return type is a compile-time `bool` about the comptime parameters (and, through `@TypeOf(x)`, the types of `anytype` ones). It's checked once per new instance, before the body is looked at, and a failure is reported at the call:
  ```
  fn isNumber(comptime T: type) -> bool { return @isInt(T) || @isFloat(T); }
  fn sum(comptime T: type, xs: []const T) -> T where isNumber(T) { ... }
  sum(bool, flags);   // error: sum(bool, ...) doesn't satisfy 'where isNumber(T)'
  ```
  Predicates are ordinary comptime functions built from `@isInt(T)`, `@isFloat(T)`, `@isSigned(T)`, `@hasField(T, "name")`, `@hasMethod(T, "name")`, `@sizeOf(T)` and type comparisons. For a custom message, a type function can say `if (comptime !ok) { @compileError("..."); }`.
- **Comptime `if`:** in `if (comptime c) { ... } else { ... }`, only the taken branch is compiled. The untaken one must parse and must refer only to names that exist (a misspelled function or a module member that isn't there is still an error), but it isn't type-checked, so it can call what only exists on another target. `std.target` holds the target: `target.os` (`.macos`, `.linux`, `.windows`), `target.arch` (`.aarch64`, `.x86_64`) and `target.pointer_size`.

### 4.12 SIMD vectors (the one operator concession)

```
let a: @Vector(4, f32) = .{ 1, 2, 3, 4 };
let b = a * @splat(2.0);       // element-wise, one SIMD instruction
let s = @reduceAdd(b);
```
**Justification.** The math types people most want operators for are small fixed-width vectors, and those map 1:1 onto hardware instructions. A built-in vector type gives readable math with zero hidden cost. Matrices and big numbers stay as function calls, because one operator there can hide arbitrarily large loops or allocations.

- `@Vector(N, T)` is exactly one SIMD register: `N * @sizeOf(T)` must be 8 or 16 bytes, with at least two elements of an integer or float type (`@Vector(4, f32)`, `@Vector(16, u8)`, `@Vector(2, f64)`). Anything bigger is an array of vectors, written out, so the cost stays visible.
- Operators work element by element, each as one instruction: `+ - *` on any element type, `/` on floats only (ARM64 has no SIMD integer division), `& | ^` on integers, and unary `-`. There's no 64-bit integer lane multiply on ARM64, so that's a compile error. Both operands must be the same vector type; a scalar is turned into a vector with `@splat(x)` (one `dup`), so the broadcast is explicit.
- **Vector integer arithmetic wraps in every build mode**, as the hardware does. Checking each lane would cost several instructions per operation, which is what the type exists to avoid. `+%` and friends are accepted and mean the same thing.
- `v[i]` reads and writes an element (bounds-checked like an array), `v.len` is `N`, `for (v) |x|` loops over the elements, `.{ ... }` makes one, and `@reduceAdd(v)` sums the elements (`addv`, or pairwise `faddp` for floats, so the order of float additions is fixed). Vectors are values, passed and copied like arrays, and work in comptime code too.
- *Implemented so far:* vectors live in memory between operations (`ldr q`, the operation, `str q`); keeping them in registers is a backend TODO.

### 4.13 Memory and allocators

```
struct AllocatorVTable {    // what an allocator implements
    alloc: fn(*void, usize, usize) -> ?[*]u8,       // (ctx, len, alignment); null when out of memory
    resize: fn(*void, [*]u8, usize, usize) -> bool,  // grow or shrink in place, if it can
    free: fn(*void, [*]u8, usize, usize),            // (ctx, memory, len, alignment)
}

struct Allocator {          // in std.mem: just two pointers, no magic
    ctx: *void,
    vtable: *const AllocatorVTable,
    fn alloc(self: Allocator, comptime T: type, n: usize) -> AllocError![]T { ... }
    fn create(self: Allocator, comptime T: type) -> AllocError!*T { ... }
    fn free(self: Allocator, memory: anytype) { ... }     // a slice from alloc
    fn destroy(self: Allocator, p: anytype) { ... }       // a pointer from create
    fn realloc(self: Allocator, comptime T: type, old: []T, n: usize) -> AllocError![]T { ... }
    fn dupe(self: Allocator, comptime T: type, s: []const T) -> AllocError![]T { ... }
}

var arena = heap.Arena.init(heap.page_allocator);
defer arena.deinit();                    // frees everything at once
let a = arena.allocator();
let names = try a.alloc([]const u8, 100);
```
- **Every function that allocates takes an `Allocator`.** If a signature has no allocator, the function doesn't allocate. You can check that by reading it.
- Running out of memory is `error.OutOfMemory`, not an abort.
- The standard library provides:
  - `heap.c_allocator` (`malloc`)
  - `heap.page_allocator`
  - `heap.Arena`
  - `heap.FixedBuffer` (allocates from a byte array; useful on the stack)
  - `heap.DebugAllocator`, which detects leaks, double frees and use-after-free (freed pages are quarantined and poisoned). Each allocation gets its own pages, filled with `0xAA`. `free` panics on a double free, on memory it didn't allocate, and on a length that doesn't match the allocation. Freed pages are filled with `0xDD`, then made inaccessible, so a later access stops the program with "use after free". `deinit()` lists every allocation that was never freed, numbered in the order they were made.
- Containers take the allocator when they're made and keep it: `list.ArrayList(T).init(alloc)`, `map.HashMap(K, V).init(alloc)`. `HashMap` keys are integers, bools, plain enums, pointers or strings (hashed with `@hash`, compared with `@eql`). A string key isn't copied.
- A slice is `{ ptr, len }`; it can't grow. `.len` can be assigned, which is how containers shrink their view of the memory they own.

### 4.14 Formatting and I/O (no hidden allocation)

```
try io.print("x = {}, name = {s}\n", .{ x, name });      // buffered stdout writer
let msg = try fmt.allocPrint(alloc, "{}-{}", .{ a, b }); // allocates: visibly
let line = try fmt.bufPrint(&buf, "{d:>8}", .{n});       // writes into a buffer you own
```
- Format strings are checked at compile time against the argument tuple. A wrong count or type is a compile error, unlike C's `printf`.
- A placeholder is `{[spec][:[fill][align][width][.precision]]}`:

  | spec | formats |
  |---|---|
  | (none) | the default for the type: numbers, `bool`, enums (tag name), errors (`error.Name`), optionals (value or `null`), pointers (address) |
  | `s` | text: `[]const u8`, `[]u8`, `cstr` |
  | `d` | a number in decimal |
  | `x` `X` `b` `o` | an integer in hex, binary or octal; `x`/`X` on a string prints its bytes in hex |
  | `c` | a `u8` as a character |
  | `e` | a float in scientific notation |
  | `*` | a pointer's address |
  | `any` | any value, including structs (`Point{ x: 1, y: 2 }`), tuples, arrays and slices (`{ 1, 2, 3 }`) |
  | `!` | before another spec: an error union, as its value or its error |

  `align` is `<`, `>` or `^` (text defaults to left, numbers to right), `precision` is the number of digits after the point for floats, and `{{` / `}}` are literal braces. `{}` on a `[]const u8` is a compile error that suggests `{s}`: a byte slice could mean text or numbers.
- `io.print` and `io.eprint` write to buffered stdout and to stderr (stdout is flushed first, so the two stay in order). Both return `!void`. `Writer` is a two-pointer interface like `Allocator`, so `fmt` can write to a buffer, to stdout, or to an `ArrayList(u8)`.
- The compiler expands each format string into calls to `std.fmt`'s helpers. Each distinct format string and argument type list makes its own instance, the same as any other generic call.
- There's no `sprintf`, `strcpy`, `strcat` or `gets`.

### 4.15 C interop

```
import c "stdio.h" as c;                 // translates the header at compile time
_ = c.printf("%d\n", 42);

extern fn qsort(base: *void, n: usize, size: usize, cmp: fn(*const void, *const void) -> i32);
export fn flint_add(a: i32, b: i32) -> i32 { return a + b; }   // C ABI, unmangled
extern union Value { i: i64, f: f64 }                          // C union; fields are read in unsafe
```
- Structs already have C layout.
- `?*T` corresponds to a nullable C pointer, `[*]T` to `T*` used as an array, and `cstr` to `const char*`.
- Calling a C function is *not* marked `unsafe`: the `extern` declaration (or the imported header) is the trust boundary. Using what C hands back (for example `[*]T` arithmetic) still follows Flint's rules.
- `import c "header.h" as name;` runs the system C preprocessor on the header (looking next to the importing file, then in the package's `[c] include` directories, then the system's) and reads the declarations it finds. Only what the program uses is translated, so a declaration Flint can't use is an error only if it's used, and the error says why.

  | C | Flint |
  |---|---|
  | `char` / `signed char` / `unsigned char` | `u8` / `i8` / `u8` |
  | `short`, `int`, `long`, `long long` (and unsigned) | `i16`, `i32`, `i64`, `i64` (`u16`...) |
  | `_Bool`, `float`, `double` | `bool`, `f32`, `f64` |
  | `size_t`, `uintptr_t` / `ssize_t`, `intptr_t`, `ptrdiff_t` | `usize` / `isize` |
  | `const char *` / `char *` | `?cstr` / `?[*]u8` |
  | `void *` / `const void *` | `?*void` / `?*const void` |
  | `T *` / `const T *` | `?*T` / `?*const T` (C pointers can be null) |
  | function pointer | `?fn(...)` |
  | `T x[N]` in a struct / an array parameter | `[N]T` / a pointer |
  | `struct s` / `union u` / `enum e` | `name.struct_s` (C layout) / `name.union_u` (an extern union) / its integer type; a typedef names the same type |
  | a struct with no body, or with bit fields | opaque: usable only behind a pointer |
  | enum constants, and `#define`s of numbers, strings, or constant expressions of them | constants: `c.EOF`, `math.M_PI` |
  | extern variables | globals: `c.stderr` |

  Not translated: function-like macros, macros whose value isn't constant (`errno`), `static inline` functions (there's no symbol to link), `va_list`, `long double`, 128-bit integers, and (for now) functions that pass or return a struct by value.

### 4.16 Build modes and the toolchain

```
flint build                  # reads flint.toml, builds out/<name>
flint build -O ReleaseFast    # -O Debug (the default) | ReleaseSafe | ReleaseFast
flint test                   # runs `test "name" { ... }` blocks
flint run main.fl
```
Debug runs every check, fills `undefined` memory with `0xAA` and skips the optimizer. ReleaseSafe optimizes and keeps every check. ReleaseFast optimizes and lets integer overflow wrap; it keeps every memory-safety check (section 6.2).

`flint.toml` declares the package: its name, its root file, its dependencies, and the C code it contains. There are no Makefiles. C sources are compiled with the system C compiler and linked in.

```toml
[package]
name = "app"
root = "src/main.fl"                 # the default

[dependencies]
geometry = { path = "../geometry" }  # import geometry;  import geometry.shapes;

[c]
sources = ["c/fast.c"]
include = ["c"]                      # also searched by import c "..."
flags = ["-O2"]
link = ["m"]                         # -lm
```
- In a package directory (or below it), `flint build`, `flint run -- args...` and `flint test` take no file name. Given a file, they work on that file, and its directory is the root that module paths start from.
- Dependencies are local paths for now. Fetching them by URL plus a hash is planned, and `url`/`hash` entries are rejected until it exists.
- **The runtime is Flint too**: `std.rt` holds the program's entry point (`main`), buffered output, panics, the `flint test` runner and the DebugAllocator's fault handler. Every program includes it, the way C programs include crt0; nothing imports it. It never allocates on the program's behalf. Panics and `exit()` flush every C stdio stream and end the process without running C `atexit` handlers; returning from `main` is a normal C exit.
- **The compiler is written in Flint** (`compiler/`) and builds itself. Building it from source starts from a prebuilt `flint` (a release binary) or from the original C compiler kept in `src/`; both produce the same compiler.
- `test "name" { ... }` declares a test: a block that runs like the body of a `fn() -> !void`. Ordinary builds skip tests. `flint test` runs every test in the program's own modules (with a package, in every `.fl` file under its root) and reports each one: ok, failed with an error (`error.TestExpectedEqual`), or panicked. Each test runs in its own process, so a panic fails only that test. `std.testing` has `expect`, `expectEqual`, `expectEqualStrings` and `expectError`.

---

## 5. C vs Flint, side by side

**Task.** Read `people.csv`, where each line is `name,age`. Print the average age. Report malformed lines with their line numbers and keep going. Fail cleanly if the file can't be read.

### C (49 lines)

```c
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    int rc = 1;
    char *line = NULL;
    size_t cap = 0;
    FILE *f = fopen("people.csv", "r");
    if (!f) {
        perror("people.csv");
        return 1;
    }
    unsigned long long total = 0;
    unsigned count = 0, line_no = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1) {     /* assignment inside condition */
        line_no++;
        if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
        if (n == 0) continue;
        char *comma = strchr(line, ',');
        if (!comma) {
            fprintf(stderr, "line %u: MissingComma\n", line_no);
            continue;
        }
        errno = 0;                                     /* global error state */
        char *end;
        long age = strtol(comma + 1, &end, 10);
        if (errno || end == comma + 1 || *end != '\0' || age < 0 || age > UINT32_MAX) {
            fprintf(stderr, "line %u: BadAge\n", line_no);
            continue;
        }
        total += (unsigned long long)age;
        count++;
    }
    if (ferror(f)) {                                   /* easy to forget */
        perror("read");
        goto cleanup;
    }
    if (count == 0) printf("no people\n");
    else printf("average age: %llu\n", total / count); /* format must match by hand */
    rc = 0;
cleanup:
    free(line);
    fclose(f);
    return rc;
}
```

### Flint (38 lines)

```
import std.fs;
import std.fmt;
import std.heap;
import std.io;
import std.mem;

error ParseError { MissingComma, BadAge }

fn parse_age(line: []const u8) -> ParseError!u32 {
    let comma = mem.indexOf(u8, line, ',') orelse return error.MissingComma;
    return fmt.parseInt(u32, line[comma + 1..], 10) catch return error.BadAge;
}

fn main() -> !void {
    var gpa = heap.DebugAllocator.init();
    defer gpa.deinit();                          // reports leaks in Debug builds
    let alloc = gpa.allocator();

    let text = try fs.readFile(alloc, "people.csv");
    defer alloc.free(text);

    var total: u64 = 0;
    var count: u32 = 0;
    var lines = mem.splitScalar(u8, text, '\n');
    var line_no: u32 = 0;
    while (lines.next()) |line| {
        line_no += 1;
        if (line.len == 0) { continue; }
        let age = parse_age(line) catch |err| {
            try io.eprint("line {}: {}\n", .{ line_no, err });
            continue;
        };
        total += age;
        count += 1;
    }
    if (count == 0) { return io.print("no people\n", .{}); }
    try io.print("average age: {}\n", .{ total / count });
}
```

**What changed:**

| C hazard | Flint |
|---|---|
| `errno` global, reset by hand | `parseInt` returns `!u32`; the error is a value |
| `strtol` range checks written by hand | `parseInt(u32, ...)` knows the target range |
| `goto cleanup`, manual cleanup order | `defer`, which runs on every exit including `try` |
| Read errors (`ferror`) easy to forget | `readFile` returns an error union, which can't be ignored |
| `"%llu"` must match the type by hand | `{}` is checked at compile time against the argument type |
| `line[n - 1]` with `n == 0` would be out of bounds | slices are bounds-checked |
| Allocation hidden inside `getline` | `readFile` takes the allocator; the leak check is free in Debug |
| `strchr` scans for a NUL terminator | slices carry their length |

---

## 6. Unsafe operations, and checked vs unchecked behavior

### 6.1 What requires `unsafe { ... }`

This is the complete list:

1. Indexing or slicing a `[*]T`, and pointer arithmetic on `[*]T` (`p + n`)
2. `@ptrCast`, `@intToPtr`, and `@bitcast` between pointer types; casting `*void` to `*T`
3. Reading a field of an `extern union`
4. Unchecked indexing: inside `unsafe`, array and slice indexing isn't bounds-checked
5. `@assume(cond)`, an optimizer hint where a false `cond` is UB
6. Calling a function declared `unsafe fn`, or using one as a value. The body of an `unsafe fn` is itself an unsafe context: its caller has promised what the compiler can't check.
7. Inline assembly, `asm { ... }` (not implemented yet)

A `defer` keeps the safety of the scope it's written in: a `defer` declared outside an `unsafe` block stays checked even when a `return` inside the block runs it.

**Undefined behavior can only arise from code inside `unsafe` blocks**, plus the memory-lifetime holes in section 6.3. Searching a codebase for `unsafe` finds every place that can break the type system.

### 6.2 Checks per build mode

| Check | Debug | ReleaseSafe | ReleaseFast | Inside `unsafe` |
|---|---|---|---|---|
| Array/slice bounds | trap | trap | **trap** | unchecked (UB if violated) |
| Integer overflow (`+ - *`, negation) | trap | trap | wraps (defined) | wraps (defined) |
| Shift amount ≥ bit width | trap | trap | amount taken mod width (defined) | same as mode |
| Division by zero | trap | trap | trap | trap |
| `INT_MIN / -1` | trap | trap | wraps (defined) | wraps |
| `as` numeric cast that doesn't fit | trap | trap | **trap** | trap |
| `.?` on `null` | trap | trap | trap | trap |
| `unreachable` reached | trap | trap | trap | trap |
| Exhaustive `switch` with an invalid tag (e.g. from C) | trap | trap | trap | UB if `@assume`d |
| Reading `undefined` | value `0xAA…` | unspecified, stable (the compiler zeroes scalars and small aggregates) | unspecified, stable | same |
| Stack overflow | trap (guard page) | trap | trap | trap |
| Leaks, double free, use-after-free | `DebugAllocator` detects them | not detected | not detected | not detected |

**The rule behind this table:** build modes may only change **arithmetic** results (trap vs wrap). Checks that protect **memory safety** (bounds, null, casts, `unreachable`) are never removed by a compiler flag, only locally by an `unsafe` block. Also, the optimizer may not use UB in one place to delete a check somewhere else. Wrapping in ReleaseFast is *defined*, so there's no "signed overflow can't happen" reasoning to exploit.

### 6.3 What stays unchecked (the honest list)

Without a borrow checker or a garbage collector, these are not prevented at compile time:

- **Use-after-free and double free.** `DebugAllocator` catches most of them in testing; nothing catches them in release builds.
- **Dangling pointers to stack memory.** Returning `&local` directly is a compile error, but pointers that escape indirectly are not tracked.
- **Data races.** Shared memory between threads is unchecked. The standard library offers atomics and mutexes, but nothing enforces their use.
- **Violated `extern` declarations.** A wrong signature for a C function is UB at the boundary.
- **Uninitialized `undefined` buffers.** Reading one is defined (an unspecified value), but the value is still garbage.
- **Vector integer overflow.** `@Vector` integer arithmetic always wraps (section 4.12).

---

## 7. Comparison with Zig, Odin, C3 and Hare

| Topic | Flint | Zig | Odin | C3 | Hare |
|---|---|---|---|---|---|
| Declarations | `let`/`var name: T` | `const`/`var name: T` | `name: T = v`, `::` | C-style `int x` | `let`/`const name: T` |
| Errors | `!T`, `try`, `errdefer` | same model | multiple returns + `or_return` | optionals with faults (`int!`) | tagged unions with `!` and `?` |
| Generics | comptime type params | comptime | parametric polymorphism | generic modules + macros | none |
| Allocators | explicit parameter | explicit parameter | implicit `context` allocator | temp/heap allocators, mostly implicit | mostly implicit `malloc` |
| Struct layout | C order always | compiler may reorder | C order | C order | C order |
| Overflow in fast builds | wraps (defined) | UB | wraps | wraps | wraps |
| Bounds checks in fast builds | **kept**; off only in `unsafe` | removed (UB) | kept; `#no_bounds_check` | removed in release | kept |
| `unreachable` in fast builds | traps | UB (optimizer hint) | – | UB | aborts |
| Unsafe marking | `unsafe` blocks | none (`@setRuntimeSafety`) | none | none | none |
| C headers | `import c` | `@cImport` | `foreign import` + bindings | parse C-like declarations | manual bindings |
| Macros | none (comptime only) | none | none | semantic macros | none |

**Where Flint agrees with them.**
- **Zig** is the closest relative. Flint takes error unions, `defer`/`errdefer`, optionals with captures, comptime generics, explicit allocators, slices, `@builtins`, build modes and C header import nearly as Zig has them, because Zig has already shown they work together.
- From **Odin**: struct layout that matches C by default, wrapping overflow in release builds, and per-block bounds-check control.
- From **C3**: the spirit of "evolve C, don't replace it", and keeping C's mental model intact.
- From **Hare**: valuing a small language, and keeping safety checks in release builds.

**Where Flint deliberately differs.**
- **Release builds have no UB.** Zig's ReleaseFast turns every safety check into UB. Flint only switches arithmetic from trap to wrap, and memory checks are removed only by visible `unsafe` blocks. This costs some speed (section 8) for a much stronger guarantee: release builds behave the same as tested builds, apart from wrapping arithmetic.
- **`unsafe` blocks exist.** None of the four languages marks unsafe regions. Flint borrows that one idea from Rust, without the borrow checker, because it makes the dangerous parts of a codebase searchable.
- **`let` instead of `const`.** Flint uses `const` only for pointee immutability (`*const T`), so the word means one thing.
- **Allocators must be explicit.** Odin's implicit context allocator is convenient, but it hides where memory comes from, which breaks the guiding principle.
- **Checked `as` casts.** Zig needs `@intCast`, `@floatFromInt` and others; Flint has a single checked `as`, plus `@truncate`/`@bitcast` for deliberate bit-level conversions.
- **Generics.** Hare has none and C3 uses semantic macros. Flint takes Zig's comptime, because it is "just Flint code" and replaces macros, generics and conditional compilation with one mechanism.

---

## 8. Open questions and trade-offs

1. **Error payloads.** Errors are payload-free `u16` codes, which is cheap and predictable but loses context such as which line failed to parse. The options are a caller-provided diagnostics struct (Zig's convention) or error sets with payloads (`!T` would grow). *Leaning:* keep codes, add a standard `Diagnostics` out-parameter pattern.

2. **Bounds checks in ReleaseFast cost real speed.** Our own n-body benchmark measured 2.41s with checks vs 0.94s without. The spec keeps them anyway and relies on (a) the optimizer proving most checks unnecessary and (b) `unsafe` blocks for measured hot spots. If check elimination proves too weak, this is the decision most likely to be revisited.

3. **Wrapping overflow in ReleaseFast** gives up optimizations that rely on "signed overflow never happens" (for example, widening loop counters). It's the right default for predictability, but it costs 0–5% in some loops.

4. **`let n = 42;` as a compile-time constant** rather than a defaulted `i32`. It's consistent with "no implicit types", but `var i = 0;` failing to compile surprises newcomers.

5. **The automatic `&` in method calls** (`v.scale(2)` passes `&v`) is the one implicit step. The alternative is to require `(&v).scale(2)`, which is honest but noisy. *Kept*, because the method's signature already says `*Self`.

6. **Parentheses around conditions plus mandatory braces** are redundant together. Parens are kept because the capture syntax `if (x) |v|` reads badly without them. Dropping the parens (`if x { }`) would mean a different capture syntax.

7. **Semicolons are required.** That's predictable, but noisy compared with the newline-terminated syntax in the current compiler.

8. **Comptime power vs compile times and error messages.** Unrestricted comptime (Zig) makes generic code hard to diagnose. *Decided:* a `where` clause (section 4.11): a comptime `bool` checked at the call before the body is analyzed, built from ordinary functions and a few type questions (`@isInt`, `@hasMethod`, ...). It's opt-in, reuses comptime instead of adding a type-class language, and the error names the call and the clause. A generic function without `where` still reports errors inside the instance, with a note at the call.

9. **Untaken comptime branches aren't type-checked**, which is needed for platform code. Names are still checked, so a misspelled function shows up everywhere, but a wrong argument type in the Windows branch only shows up when you build for Windows.

10. **Fixed C struct layout** wastes padding where reordering would help. Should there be an opt-in `auto struct` for compiler-chosen layout?

11. **Use-after-free remains possible.** Should the standard library promote generational handles (`Pool(T)` with `Handle`) as the default way to hold long-lived references?

12. **Build system:** declarative `flint.toml` vs a build script written in Flint (Zig's choice). Declarative is predictable; scripts are flexible. *Done so far:* declarative (`flint.toml`, section 4.16). An optional `build.fl` escape hatch is still open.

13. **No `++`/`--`.** It removes a whole class of expression puzzles, but C programmers will miss `i++` in loops (mostly replaced by `for (0..n) |i|`).

---

## 9. Appendix

### 9.1 Conflicts in the brief, and how they're resolved

- `var buf: [256]u8;` and `let handler: fn(i32) -> void;` contradict "all variables must be initialized". They're written `var buf: [256]u8 = undefined;` and `let handler: fn(i32) = on_signal;`. A `let` must have a real value.
- `return if (a > b) a else b;` vs "braces are mandatory": the *statement* `if` requires braces, and the *expression* `if` is a separate form with a mandatory `else` and no braces.
- `fn(i32)` vs `fn(i32) -> void`: both are allowed in types, and omitting `-> T` means void.
- `.Rect(rect)` on a struct-payload variant binds the whole payload struct, which is why `rect.w` works.

### 9.2 What the current compiler already has, and what has to change

**Already in place:**
- modules, `pub`, and the standard library split into `std` modules; `flint.toml` packages and `flint test`
- C header import (the system preprocessor plus a small parser for declarations)
- the ARM64 backend, IR and optimizer
- `defer`, slices with bounds checks, fixed-width integers, structs and methods
- `extern` C calls, checked `as` casts, checked compile-time literal ranges
- the safety model of section 6: overflow checks, `unsafe` blocks and `unsafe fn`, and the three build modes
- no hidden allocation (sections 4.13–4.14): allocators, `str` as `[]const u8`, `io.print`/`fmt` with checked format strings, `ArrayList` and `HashMap`; template literals, `str + str`, heap array literals and `.push()` are gone
- comptime, as an IR interpreter: `comptime` expressions and blocks, comptime `if`, types as values, type functions with logic in them, and `where` constraints
- SIMD vectors (`@Vector`)
- self-hosting: the compiler and the runtime are written in Flint, and the compiler builds itself to the same output (milestone 7)

**Has to change:**
- **Syntax:**
  - required semicolons (instead of newline insertion)
  - parens around conditions
  - `let`/`var`, with immutable by default
  - postfix `.*` dereference
  - remove `++`/`--`
  - methods declared inside the struct body
  - `for (xs) |x, i|` loops
- **Types:**
  - drop the `int`/`float`/`byte` aliases
  - `[]const T`, `[*]T`, `cstr` and arrays `[N]T`
  - optionals, error unions, sum types, tuples
  - the precedence fix
- **New in the compiler:**
  - keeping vectors in registers
