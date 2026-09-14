# sardine

Serde for C++26: JSON, CBOR, debug-printing and schemas for any struct or
enum, out of the box. One header, no macros, no codegen — just reflection.

```cpp
struct User {
  [[=sardine::rename("user_id")]] int id = 0;
  [[=sardine::skip{}]]            int cache = -1;
  std::string name;
  double balance = 0;
};

User u{.id = 7, .name = "Henri", .balance = 12.5};
std::string j = sardine::to_json(u);          // {"user_id":7,"name":"Henri","balance":12.5}
std::expected<User, sardine::error> back = sardine::from_json<User>(j);
std::vector<std::uint8_t> c = sardine::to_cbor(u);  // same model, binary (RFC 8949)
std::println("{:#}", sardine::dbg(u));        // Rust {:#?}-style debug print
```

Errors carry a code and the dotted path of the failure
(`unknown_field` at `"items.0.name"`). CBOR does byte strings
(`vector<uint8_t>` ↔ major 2), COSE-style integer field keys
(`[[=sardine::int_key(3)]]`), verbatim passthrough (`sardine::cbor_raw`),
prefix decoding, and an opt-in strict profile (`sardine::cbor_strict`) for
signed wires. `sardine::value` is the generic JSON document for shapes that
aren't yours to model, and `from_json_into` overlays a document onto an
existing object.

GCC 16.1, `-std=c++26 -freflection`; `make test` runs the suite.

- [examples.md](examples.md) — cookbook, every attribute and type
- [NOTES.md](NOTES.md) — Serde parity table, GCC quirks, deviations
- <https://godbolt.org/z/E7Ea44ojT> — live: static_asserts + roundtrip
  checks pass, and the asm shows reflection fully folds away
