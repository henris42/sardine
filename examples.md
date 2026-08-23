# sardine cookbook

Every JSON/output string below is asserted verbatim in
[tests/test_sardine.cpp](tests/test_sardine.cpp). Compile everything with:

```
g++ -std=c++26 -freflection -Iinclude your_file.cpp
```

## 1. Quick start

No derive, no macros, no registration — any struct just works:

```cpp
#include <sardine/sardine.hpp>

struct Point { int x; int y; };

std::string j = sardine::to_json(Point{1, 2});
// {"x":1,"y":2}

auto p = sardine::from_json<Point>(j);   // std::expected<Point, sardine::error>
if (p) use(p->x);
```

## 2. Renaming fields

```cpp
struct User {
  [[=sardine::rename("user_id")]] int id = 0;
  std::string name;
};
// {"user_id":7,"name":"Henri"}
```

Whole-struct case conversion, identical spellings to Serde
(`camelCase`, `PascalCase`, `snake_case`, `SCREAMING_SNAKE_CASE`,
`kebab-case`, `SCREAMING-KEBAB-CASE`). An explicit `rename` always wins:

```cpp
struct [[=sardine::rename_all("camelCase")]] Config {
  int retry_count = 0;                                // -> "retryCount"
  std::string base_url;                               // -> "baseUrl"
  [[=sardine::rename("TIMEOUT")]] int timeout_ms = 0;   // -> "TIMEOUT"
};
// {"retryCount":3,"baseUrl":"https://stor.ax","TIMEOUT":250}
```

An unknown style string fails **at compile time** (the annotation is a
consteval constructor).

## 3. Skipping fields

```cpp
struct Session {
  std::string token;
  [[=sardine::skip{}]] int local_cache = 0;              // invisible both ways
  [[=sardine::skip_serializing{}]] int write_only = 0;   // read, never written
  [[=sardine::skip_deserializing{}]] int computed = 42;  // written, never read
};
```

Skipped-on-read fields keep whatever the default constructor gave them.

## 4. Optionals and missing fields

`std::optional<T>` maps to `null`:

```cpp
struct Profile {
  std::string name;
  std::optional<std::string> bio;   // absent or null -> nullopt
};

sardine::to_json(Profile{"hs", std::nullopt});  // {"name":"hs","bio":null}
```

Missing fields keep their default value. To make absence an error, mark the
field `required`:

```cpp
struct WithRequired {
  [[=sardine::required{}]] int must = 0;
  int optional_field = 0;
};

auto r = sardine::from_json<WithRequired>(R"({"optional_field":2})");
// r.error().message == "missing required field 'must'"
```

And to reject unexpected keys entirely (Serde's `deny_unknown_fields`):

```cpp
struct [[=sardine::deny_unknown_fields{}]] Strict { int a = 0; };

sardine::from_json<Strict>(R"({"a":1,"b":2})");   // error: unknown field
```

## 5. Enums

Enums serialize as their enumerator name, and `rename_all` works on them too:

```cpp
enum class Color { red, green, blue };
sardine::to_json(Color::green);                   // "green"

enum class [[=sardine::rename_all("SCREAMING_SNAKE_CASE")]] Level {
  debug_info, warning, fatal_error,
};
sardine::to_json(Level::fatal_error);             // "FATAL_ERROR"
sardine::from_json<Level>(R"("DEBUG_INFO")");     // Level::debug_info
```

A value with no matching enumerator falls back to the underlying integer in
both directions; an unknown *string* is an error.

## 6. Variants (Serde enums with payloads)

`std::variant` maps to Serde's tagged enums. The variant "name" is the
alternative's type name. Default is **externally tagged**:

```cpp
struct Circle { double radius = 0; };
struct Rect   { double w = 0, h = 0; };
using Shape = std::variant<Circle, Rect>;

sardine::to_json(Shape{Circle{2}});   // {"Circle":{"radius":2}}
```

**Internally tagged** (`#[serde(tag = "type")]`) via a member annotation —
alternatives must all be structs:

```cpp
struct Scene {
  [[=sardine::tag("type")]] Shape shape;
};
// {"shape":{"type":"Circle","radius":1.5}}
```

**Untagged** — alternatives are tried in declaration order until one parses:

```cpp
struct Setting {
  [[=sardine::untagged{}]] std::variant<int, std::string> value;
};
// {"value":42}  or  {"value":"auto"}
```

Unit alternatives (`std::monostate`) collapse to a plain string, like Serde's
unit variants:

```cpp
std::variant<std::monostate, Circle> maybe;   // -> "monostate"
```

## 7. Flatten

Hoist a nested struct's fields into the parent, and/or collect leftovers into
a map (Serde's `#[serde(flatten)]` catch-all pattern):

```cpp
struct DocMeta { int version = 0; std::string author; };

struct Doc {
  std::string title;
  [[=sardine::flatten{}]] DocMeta meta;                          // inlined
  [[=sardine::flatten{}]] std::map<std::string, std::string> extra;  // catch-all
};

// {"title":"t","version":2,"author":"hs","x":"1","y":"2"}
//            \--- from meta ---/  \-- unknown keys land in extra --/
```

Round-trips: on deserialization, keys are matched against direct fields
first, then flattened structs (recursively), and anything left goes into the
catch-all map.

## 8. Maps and sequences

Any range serializes; anything with `emplace_back` deserializes. Map keys may
be strings **or integers** (encoded as JSON string keys, like Serde):

```cpp
std::map<int, std::string> m{{1, "a"}, {2, "b"}};
sardine::to_json(m);   // {"1":"a","2":"b"}
```

## 8b. Exporting constexpr tables (spans, custom string types)

Serialization is read-only, so it works on **views** — `std::span` over
`constexpr` arrays serializes without copying. And any type convertible to
`std::string_view` serializes as a JSON string, so a compile-time fixed
string needs one conversion operator, not an adapter:

```cpp
template <std::size_t N> struct FixedString {
  char data[N]{}; std::size_t len{};
  // ...
  constexpr operator std::string_view() const { return {data, len}; }  // <-- enough
};

struct Row  { FixedString<16> name; std::uint32_t addr; };
inline constexpr Row kRows[] = { {"LedRed", 13}, {"Button", 11} };

struct Export { std::span<const Row> rows; };
sardine::to_json_pretty(Export{kRows});
// {"rows":[{"name":"LedRed","addr":13},{"name":"Button","addr":11}]}
```

This is how an embedded project whose configuration lives in `constexpr`
tables (e.g. [firn](https://github.com/henris42/firn)) emits its topology as
JSON for host tooling: the firmware's compile-time data is the single source
of truth; the JSON is generated *from* it. The host side re-imports with the
same header into `std::string`/`std::vector` mirror structs — deserialization
targets owning types (`std::string`, containers with `emplace_back`), views
and fixed strings are serialize-only.

## 8c. Schema ("meta class")

`sardine::schema<T>()` describes a type's **serialized form** as a JSON
Schema document — generated by reflection, honoring the same annotations the
writer does, so it is always true of `to_json<T>()`'s output:

```cpp
sardine::schema<User>();
// {"type":"object","title":"User","properties":{
//   "user_id":{"type":"integer"},"name":{"type":"string"},
//   "balance":{"type":"number"},"active":{"type":"boolean"}},
//  "required":[],"additionalProperties":true}
```

Enums list their serialized names (`{"enum":["idle","running"]}`), optionals
become `anyOf [T, null]`, variants `oneOf` in the externally-tagged form,
flattened structs are hoisted, and `additionalProperties` reflects
`deny_unknown_fields`. Use it as the machine-readable protocol description an
endpoint hands to its peers — the IDL/WSDL role, generated from the type
instead of maintained beside it.

## 9. Pretty printing

```cpp
sardine::to_json_pretty(user);       // 2-space indent (default)
sardine::to_json_pretty(user, 4);    // custom indent
```

```json
{
  "user_id": 7,
  "name": "Henri",
  "balance": 12.5,
  "active": true
}
```

Empty containers stay inline (`[]`, `{}`).

## 10. Debug output (Rust's `{:?}` / `{:#?}`)

Like `derive(Debug)`, this deliberately ignores all sardine annotations:
raw C++ field names, skipped fields included, `Some`/`None` for optionals,
floats always with a decimal point.

```cpp
User u{.id = 7, .name = "Henri", .balance = 12.5, .active = true};

sardine::debug(u);
// User { id: 7, cache: -1, name: "Henri", balance: 12.5, active: true }

std::println("{}",  sardine::dbg(u));    // same, via std::format
std::println("{:#}", sardine::dbg(u));   // pretty, Rust {:#?} style:
```

```text
User {
    id: 7,
    cache: -1,
    name: "Henri",
    balance: 12.5,
    active: true,
}
```

More Rust-isms:

```cpp
sardine::debug(3.0);                      // 3.0   (not "3")
sardine::debug(std::optional<int>{5});    // Some(5)
sardine::debug(std::vector{1, 2});        // [1, 2]
sardine::debug(Shape{Circle{2}});         // Circle { radius: 2.0 }
```

## 11. Error handling

`from_json` never throws — it returns `std::expected<T, sardine::error>` with a
message and the byte offset of the failure:

```cpp
auto r = sardine::from_json<User>(R"({"name": 12})");
if (!r) {
  std::println("parse failed at byte {}: {}", r.error().offset,
               r.error().message);
}
```

Malformed JSON, type mismatches, integer overflow, lone surrogates, raw
control characters in strings, unknown enumerators, unmatched variant tags,
and trailing garbage are all reported this way. Nesting depth is capped at
256 to keep hostile inputs from blowing the stack.

## 12. Everything composes

```cpp
struct [[=sardine::rename_all("camelCase")]] ApiResponse {
  [[=sardine::required{}]] std::string request_id;
  std::optional<std::vector<std::map<std::string, Shape>>> payload;
  [[=sardine::flatten{}]] std::map<std::string, std::string> meta;
};
```

Nesting of optional/vector/map/variant/struct/enum is unrestricted — the
serializer is generated recursively from the reflected structure at compile
time.
