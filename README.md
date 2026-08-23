# sardine — Serde-style JSON (de)serialization with C++26 reflection

An investigation into whether C++26 can match Rust's Serde experience —
serialize/deserialize with *just annotations*, no macros, no code generation,
no per-type boilerplate. **Answer: yes.** The whole library is one header,
[include/sardine/sardine.hpp](include/sardine/sardine.hpp), driven entirely by
compile-time reflection. See [examples.md](examples.md) for a cookbook.

```cpp
struct User {
  [[=sardine::rename("user_id")]] int id = 0;
  [[=sardine::skip{}]]            int cache = -1;
  std::string name;
  double balance = 0;
};

User u{.id = 7, .name = "Henri", .balance = 12.5};
std::string j = sardine::to_json(u);          // {"user_id":7,"name":"Henri","balance":12.5}
std::string p = sardine::to_json_pretty(u);   // indented

std::expected<User, sardine::error> back = sardine::from_json<User>(j);

std::println("{}",  sardine::dbg(u));  // Rust {:?}  -> User { id: 7, ... }
std::println("{:#}", sardine::dbg(u)); // Rust {:#?} -> multi-line
```

Rust equivalent for comparison:

```rust
#[derive(Serialize, Deserialize, Debug)]
struct User {
    #[serde(rename = "user_id")] id: i32,
    #[serde(skip)]               cache: i32,
    name: String,
    balance: f64,
}
```

The C++ version needs *less* than the Rust one: no `derive` at all — any
class/enum is (de)serializable and debug-printable by default, annotations
only customize.

## Building

Requires GCC 16.1 (installed at `/opt/gcc-16.1` on this machine):

```
make test        # g++ -std=c++26 -freflection ...
```

## Feature matrix (Serde parity)

| Serde | sardine | Notes |
|---|---|---|
| `#[serde(rename = "x")]` | `[[=sardine::rename("x")]]` | fields *and* enumerators |
| `#[serde(rename_all = "camelCase")]` | `[[=sardine::rename_all("camelCase")]]` | on structs and enums; all 6 Serde spellings |
| `#[serde(skip)]` | `[[=sardine::skip{}]]` | |
| `#[serde(skip_serializing)]` | `[[=sardine::skip_serializing{}]]` | |
| `#[serde(skip_deserializing)]` | `[[=sardine::skip_deserializing{}]]` | |
| `#[serde(flatten)]` | `[[=sardine::flatten{}]]` | struct inlining *and* map catch-all |
| `#[serde(deny_unknown_fields)]` | `[[=sardine::deny_unknown_fields{}]]` | on the struct |
| required fields (Serde default) | `[[=sardine::required{}]]` | opt-in here; plain fields keep their default when absent |
| enum w/ payload, externally tagged | `std::variant` | default; alt type name is the tag |
| `#[serde(tag = "type")]` | `[[=sardine::tag("type")]]` | internally tagged; struct alternatives only |
| `#[serde(untagged)]` | `[[=sardine::untagged{}]]` | alternatives tried in order |
| unit variants | `std::monostate` | collapses to `"monostate"` string |
| `Option<T>` ↔ null | `std::optional<T>` ↔ null | |
| fieldless enums as strings | enum class ↔ `"enumerator_name"` | un-named values fall back to the integer |
| unknown fields ignored | same | unless `deny_unknown_fields` |
| integer map keys as strings | same | `map<int, T>` ↔ `{"1": ...}` |
| `serde_json::to_string_pretty` | `sardine::to_json_pretty(v, indent = 2)` | |
| `#[derive(Debug)]` + `{:?}` | `sardine::debug(v)` / `std::format("{}", sardine::dbg(v))` | ignores sardine attrs, like Rust |
| `{:#?}` | `sardine::debug_pretty(v)` / `"{:#}"` | 4-space Rust style, trailing commas |
| `Result<T, Error>` | `std::expected<T, sardine::error>` | error message + byte offset |

Supported types: `bool`, integers, floats (nan/inf → `null`), `std::string`,
`std::optional<T>`, `std::variant<...>`, `std::monostate`, sequence
containers, maps with string or integer keys, nested structs,
scoped/unscoped enums — all composing freely. The JSON parser handles
escapes incl. `\uXXXX` with surrogate pairs, and has a nesting-depth guard.

## How it works — the three C++26 pieces

1. **P2996 reflection** is the engine. `^^T` reflects a type;
   `std::meta::nonstatic_data_members_of` enumerates fields;
   `std::meta::enumerators_of` enumerates enum values;
   `obj.[:member:]` (splicing) reads/writes a field given its reflection.

2. **P1306 expansion statements** (`template for`) iterate over the member
   list in ordinary imperative code — this replaces Serde's proc-macro
   codegen. The loop body is stamped out per member at compile time, so the
   serializer that falls out is the same flat code you'd write by hand.

3. **P3394 annotations** (`[[= expr]]`) are the user syntax. An annotation is
   an arbitrary constant object attached to a declaration, read back with
   `std::meta::annotations_of_with_type` + `std::meta::extract<T>`. This is
   what makes the `#[serde(...)]` feel possible without macros.

Field-name resolution happens entirely at compile time
(`rename` > owner's `rename_all` > identifier); converted names are promoted
to static storage with `std::define_static_string`, so runtime serialization
never computes names. Even the "missing required field 'x'" error strings
are assembled at compile time.

## Gotchas found during the investigation (GCC 16.1)

- **`-freflection` is required** — reflection isn't enabled by `-std=c++26`
  alone yet. Feature macro: `__cpp_impl_reflection == 202603`.
- **Annotations must have structural type.** `std::string_view` members
  disqualify the annotation, and a `const char*` pointing at a string literal
  fails constant normalization (`reflect_constant failed`). Fix: store the
  name *by value* in a fixed `char[64]` buffer (`sardine::detail::fixed_string`).
- **Annotation syntax quirk:** `[[=rename{"x"}]]` (bare braced-init) does not
  parse; `[[=rename("x")]]` (constructor call) and `[[=(rename{"x"})]]` do.
  Empty braces are fine: `[[=sardine::skip{}]]`.
- **`nonstatic_data_members_of` returns a `std::vector`** (heap), so it can't
  sit in a `constexpr` variable — pipe it through `std::define_static_array`
  before `template for`. Same trap for any local `constexpr auto mems = ...`:
  the expansion range must be a constant expression, so call the consteval
  helper directly in the `template for` head.
- **Names collide with globals**: an annotation type called `rename` clashes
  with C's `::rename` at global scope — keep annotations in a namespace.
- GCC's P3394 library API uses the earlier spelling
  `annotations_of_with_type(info, info)`; the paper's
  `annotation_of_type<T>` doesn't exist (easily wrapped).
- `std::meta::info` works as an NTTP (it's structural), which enables
  per-member dispatch like `read_member<m>(...)` inside `template for`.

## Design deviations from Serde

- Missing fields default silently unless marked `[[=sardine::required{}]]`
  (Serde errors unless `#[serde(default)]`). Chosen because C++ default
  member initializers make this the natural fit.
- Variant "names" are the alternative's *type* name (Rust variants have
  their own names).
- `required` is not tracked through `flatten` boundaries (fields of a
  flattened struct can't be marked required — the annotation is honored only
  on direct members of the type being parsed).
- Internally tagged variants re-scan the object for the tag, then re-parse —
  the alternative must therefore tolerate the tag key (don't combine with
  `deny_unknown_fields` on the alternative).

## Not implemented (future work)

Adjacently tagged variants (`tag` + `content`), `serialize_with` custom
converters, `#[serde(default = "path")]` custom defaults, non-struct
alternatives for internally tagged variants, `std::tuple`, duplicate-key
detection.
