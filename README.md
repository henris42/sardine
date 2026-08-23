# sardine

Serde for C++26: JSON, debug-printing and schemas for any struct or enum,
out of the box. One header, no macros, no codegen — just reflection.

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
std::println("{:#}", sardine::dbg(u));        // Rust {:#?}-style debug print
```

GCC 16.1, `-std=c++26 -freflection`; `make test` runs the suite.

- [examples.md](examples.md) — cookbook, every attribute and type
- [NOTES.md](NOTES.md) — Serde parity table, GCC quirks, deviations
- <https://godbolt.org/z/E7Ea44ojT> — live: static_asserts + roundtrip
  checks pass, and the asm shows reflection fully folds away
