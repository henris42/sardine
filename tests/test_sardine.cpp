#include <sardine/sardine.hpp>

#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <print>
#include <span>
#include <string>
#include <variant>
#include <vector>

static int failures = 0;

#define EXPECT(cond)                                                     \
  do {                                                                   \
    if (!(cond)) {                                                       \
      ++failures;                                                        \
      std::println("FAIL {}:{}  {}", __FILE__, __LINE__, #cond);         \
    }                                                                    \
  } while (0)

#define EXPECT_EQ(a, b)                                                  \
  do {                                                                   \
    auto va = (a);                                                       \
    auto vb = (b);                                                       \
    if (!(va == vb)) {                                                   \
      ++failures;                                                        \
      std::println("FAIL {}:{}  {} == {}\n  lhs: {}\n  rhs: {}",         \
                   __FILE__, __LINE__, #a, #b, va, vb);                  \
    }                                                                    \
  } while (0)

// --- fixtures ---------------------------------------------------------------

struct User {
  [[=sardine::rename("user_id")]] int id = 0;
  [[=sardine::skip{}]] int cache = -1;
  std::string name;
  double balance = 0;
  bool active = false;
};

struct [[=sardine::rename_all("camelCase")]] Config {
  int retry_count = 0;
  std::string base_url;
  [[=sardine::rename("TIMEOUT")]] int timeout_ms = 0;  // explicit rename wins
};

enum class [[=sardine::rename_all("SCREAMING_SNAKE_CASE")]] Level {
  debug_info,
  warning,
  fatal_error,
};

enum class Color { red, green, blue };

struct Inner {
  std::vector<int> values;
  std::optional<std::string> note;
};

struct Outer {
  Inner inner;
  std::map<std::string, double> scores;
  std::vector<Inner> history;
  Color color = Color::red;
  Level level = Level::warning;
};

struct Directional {
  [[=sardine::skip_serializing{}]] int write_only = 0;   // read from JSON, never emitted
  [[=sardine::skip_deserializing{}]] int read_only = 42; // emitted, never read
  int normal = 0;
};

struct Circle { double radius = 0; };
struct Rect { double w = 0, h = 0; };
using Shape = std::variant<Circle, Rect>;

struct ShapeHolder {
  Shape ext;  // externally tagged (the default)
  [[=sardine::untagged{}]] std::variant<int, std::string> u;
  [[=sardine::tag("type")]] Shape tagged;
};

struct DocMeta { int version = 0; std::string author; };
struct Doc {
  std::string title;
  [[=sardine::flatten{}]] DocMeta meta;
  [[=sardine::flatten{}]] std::map<std::string, std::string> extra;
};

struct [[=sardine::deny_unknown_fields{}]] Strict { int a = 0; };

struct WithRequired {
  [[=sardine::required{}]] int must = 0;
  int optional_field = 0;
};

// --- tests ------------------------------------------------------------------

static void test_basic_roundtrip() {
  User u{.id = 7, .cache = 99, .name = "Henri", .balance = 12.5, .active = true};
  std::string j = sardine::to_json(u);
  EXPECT_EQ(j, R"({"user_id":7,"name":"Henri","balance":12.5,"active":true})");

  auto back = sardine::from_json<User>(j);
  EXPECT(back.has_value());
  EXPECT_EQ(back->id, 7);
  EXPECT_EQ(back->cache, -1);  // skipped: keeps its default
  EXPECT_EQ(back->name, "Henri");
  EXPECT_EQ(back->balance, 12.5);
  EXPECT_EQ(back->active, true);
}

static void test_pretty_json() {
  User u{.id = 7, .name = "Henri", .balance = 12.5, .active = true};
  EXPECT_EQ(sardine::to_json_pretty(u),
            "{\n"
            "  \"user_id\": 7,\n"
            "  \"name\": \"Henri\",\n"
            "  \"balance\": 12.5,\n"
            "  \"active\": true\n"
            "}");

  Inner i{.values = {1, 2}, .note = std::nullopt};
  EXPECT_EQ(sardine::to_json_pretty(i),
            "{\n"
            "  \"values\": [\n"
            "    1,\n"
            "    2\n"
            "  ],\n"
            "  \"note\": null\n"
            "}");

  // empty containers stay inline, indent width is configurable
  EXPECT_EQ(sardine::to_json_pretty(std::vector<int>{}), "[]");
  EXPECT_EQ(sardine::to_json_pretty(std::vector<int>{5}, 4), "[\n    5\n]");

  // pretty output parses back
  Outer o{.inner = {.values = {3}, .note = "n"}, .scores = {{"a", 1}}, .history = {}};
  auto back = sardine::from_json<Outer>(sardine::to_json_pretty(o));
  EXPECT(back.has_value());
  EXPECT((back->inner.values == std::vector{3}));
}

static void test_debug() {
  User u{.id = 7, .name = "Henri", .balance = 12.5, .active = true};
  EXPECT_EQ(sardine::debug(u),
            R"(User { id: 7, cache: -1, name: "Henri", balance: 12.5, active: true })");
  EXPECT_EQ(sardine::debug_pretty(u),
            "User {\n"
            "    id: 7,\n"
            "    cache: -1,\n"
            "    name: \"Henri\",\n"
            "    balance: 12.5,\n"
            "    active: true,\n"
            "}");

  // Rust conventions: floats keep a decimal point, Option is Some/None,
  // enum values print bare, Debug ignores sardine renames/skips.
  EXPECT_EQ(sardine::debug(3.0), "3.0");
  EXPECT_EQ(sardine::debug(std::optional<int>{}), "None");
  EXPECT_EQ(sardine::debug(std::optional<int>{5}), "Some(5)");
  EXPECT_EQ(sardine::debug(Level::fatal_error), "fatal_error");
  EXPECT_EQ(sardine::debug(std::vector{1, 2}), "[1, 2]");
  EXPECT_EQ(sardine::debug_pretty(std::vector{1, 2}), "[\n    1,\n    2,\n]");
  EXPECT_EQ(sardine::debug(std::map<std::string, int>{{"a", 1}}), R"({"a": 1})");
  EXPECT_EQ(sardine::debug(Shape{Circle{.radius = 2}}), "Circle { radius: 2.0 }");

  // std::format integration
  EXPECT_EQ(std::format("{}", sardine::dbg(u)), sardine::debug(u));
  EXPECT_EQ(std::format("{:#}", sardine::dbg(u)), sardine::debug_pretty(u));
}

static void test_rename_all() {
  Config c{.retry_count = 3, .base_url = "https://stor.ax", .timeout_ms = 250};
  std::string j = sardine::to_json(c);
  EXPECT_EQ(j, R"({"retryCount":3,"baseUrl":"https://stor.ax","TIMEOUT":250})");

  auto back = sardine::from_json<Config>(j);
  EXPECT(back.has_value());
  EXPECT_EQ(back->retry_count, 3);
  EXPECT_EQ(back->base_url, "https://stor.ax");
  EXPECT_EQ(back->timeout_ms, 250);
}

static void test_enums() {
  EXPECT_EQ(sardine::to_json(Color::green), R"("green")");
  EXPECT_EQ(sardine::to_json(Level::fatal_error), R"("FATAL_ERROR")");

  auto lvl = sardine::from_json<Level>(R"("DEBUG_INFO")");
  EXPECT(lvl.has_value() && *lvl == Level::debug_info);

  // Un-named enum value falls back to the underlying integer, both ways.
  EXPECT_EQ(sardine::to_json(static_cast<Color>(9)), "9");
  auto c9 = sardine::from_json<Color>("9");
  EXPECT(c9.has_value() && *c9 == static_cast<Color>(9));

  auto bad = sardine::from_json<Color>(R"("magenta")");
  EXPECT(!bad.has_value());
}

static void test_variants() {
  EXPECT_EQ(sardine::to_json(Shape{Circle{.radius = 2}}), R"({"Circle":{"radius":2}})");
  auto s = sardine::from_json<Shape>(R"({"Rect":{"w":3,"h":4}})");
  EXPECT(s.has_value() && std::holds_alternative<Rect>(*s));
  if (s) EXPECT_EQ(std::get<Rect>(*s).h, 4.0);

  ShapeHolder h{.ext = Rect{3, 4}, .u = std::string("hi"), .tagged = Circle{1.5}};
  std::string j = sardine::to_json(h);
  EXPECT_EQ(j,
            R"({"ext":{"Rect":{"w":3,"h":4}},"u":"hi","tagged":{"type":"Circle","radius":1.5}})");
  auto back = sardine::from_json<ShapeHolder>(j);
  EXPECT(back.has_value());
  EXPECT(std::holds_alternative<Rect>(back->ext));
  EXPECT((back->u == std::variant<int, std::string>(std::string("hi"))));
  EXPECT(std::holds_alternative<Circle>(back->tagged));
  if (std::holds_alternative<Circle>(back->tagged))
    EXPECT_EQ(std::get<Circle>(back->tagged).radius, 1.5);

  // untagged: alternatives tried in order
  auto n = sardine::from_json<ShapeHolder>(
      R"({"ext":{"Circle":{"radius":1}},"u":42,"tagged":{"type":"Rect","w":1,"h":2}})");
  EXPECT(n.has_value() && (n->u == std::variant<int, std::string>(42)));

  // unit alternatives (monostate) collapse to a plain string
  using MaybeShape = std::variant<std::monostate, Circle>;
  EXPECT_EQ(sardine::to_json(MaybeShape{}), R"("monostate")");
  auto m = sardine::from_json<MaybeShape>(R"("monostate")");
  EXPECT(m.has_value() && std::holds_alternative<std::monostate>(*m));

  EXPECT(!sardine::from_json<Shape>(R"({"Triangle":{}})").has_value());
  EXPECT(!sardine::from_json<ShapeHolder>(
      R"({"ext":{"Circle":{"radius":1}},"u":42,"tagged":{"w":1,"h":2}})")
      .has_value());  // missing tag
}

static void test_flatten() {
  Doc d{.title = "t",
        .meta = {.version = 2, .author = "hs"},
        .extra = {{"x", "1"}, {"y", "2"}}};
  std::string j = sardine::to_json(d);
  EXPECT_EQ(j, R"({"title":"t","version":2,"author":"hs","x":"1","y":"2"})");

  auto back = sardine::from_json<Doc>(j);
  EXPECT(back.has_value());
  EXPECT_EQ(back->title, "t");
  EXPECT_EQ(back->meta.version, 2);
  EXPECT_EQ(back->meta.author, "hs");
  EXPECT_EQ(back->extra.at("x"), "1");   // unknown keys land in the catch-all
  EXPECT_EQ(back->extra.size(), 2uz);
}

static void test_strictness() {
  EXPECT(sardine::from_json<Strict>(R"({"a":1})").has_value());
  auto e = sardine::from_json<Strict>(R"({"a":1,"b":2})");
  EXPECT(!e.has_value());

  EXPECT(sardine::from_json<WithRequired>(R"({"must":1})").has_value());
  auto r = sardine::from_json<WithRequired>(R"({"optional_field":2})");
  EXPECT(!r.has_value());
  if (!r) EXPECT(r.error().message.find("must") != std::string::npos);
}

static void test_int_map() {
  std::map<int, std::string> m{{1, "a"}, {2, "b"}};
  std::string j = sardine::to_json(m);
  EXPECT_EQ(j, R"({"1":"a","2":"b"})");
  auto back = sardine::from_json<std::map<int, std::string>>(j);
  EXPECT(back.has_value() && *back == m);
  EXPECT((!sardine::from_json<std::map<int, int>>(R"({"nope":1})").has_value()));
}

static void test_nested() {
  Outer o{
      .inner = {.values = {1, 2, 3}, .note = "hi"},
      .scores = {{"alpha", 1.5}, {"beta", -2.0}},
      .history = {{.values = {4}, .note = std::nullopt}},
      .color = Color::blue,
      .level = Level::fatal_error,
  };
  std::string j = sardine::to_json(o);
  auto back = sardine::from_json<Outer>(j);
  EXPECT(back.has_value());
  EXPECT((back->inner.values == std::vector{1, 2, 3}));
  EXPECT(back->inner.note == "hi");
  EXPECT_EQ(back->scores.at("alpha"), 1.5);
  EXPECT_EQ(back->scores.at("beta"), -2.0);
  EXPECT_EQ(back->history.size(), 1uz);
  EXPECT(back->history[0].note == std::nullopt);
  EXPECT(back->color == Color::blue);
  EXPECT(back->level == Level::fatal_error);
}

static void test_optionals() {
  EXPECT_EQ(sardine::to_json(std::optional<int>{}), "null");
  EXPECT_EQ(sardine::to_json(std::optional<int>{5}), "5");
  auto v = sardine::from_json<std::optional<int>>("null");
  EXPECT(v.has_value() && !v->has_value());
  auto w = sardine::from_json<std::optional<int>>(" 5 ");
  EXPECT(w.has_value() && **w == 5);
}

static void test_unknown_and_missing_fields() {
  auto u = sardine::from_json<User>(
      R"({"user_id": 1, "wat": {"deep": [1, {"x": "y"}]}, "name": "n"})");
  EXPECT(u.has_value());
  EXPECT_EQ(u->id, 1);
  EXPECT_EQ(u->name, "n");
  EXPECT_EQ(u->balance, 0.0);  // missing field keeps default
}

static void test_directional_skips() {
  Directional d{.write_only = 5, .read_only = 42, .normal = 1};
  std::string j = sardine::to_json(d);
  EXPECT_EQ(j, R"({"read_only":42,"normal":1})");

  auto back =
      sardine::from_json<Directional>(R"({"write_only":7,"read_only":100,"normal":2})");
  EXPECT(back.has_value());
  EXPECT_EQ(back->write_only, 7);
  EXPECT_EQ(back->read_only, 42);  // deserialization ignored it
  EXPECT_EQ(back->normal, 2);
}

static void test_string_escapes() {
  std::string s = "quote\" slash\\ nl\n tab\t bell\x07 snowman☃";
  std::string j = sardine::to_json(s);
  EXPECT_EQ(j, "\"quote\\\" slash\\\\ nl\\n tab\\t bell\\u0007 snowman☃\"");
  auto back = sardine::from_json<std::string>(j);
  EXPECT(back.has_value() && *back == s);

  // \u escapes incl. a surrogate pair (🍕 U+1F355)
  auto p = sardine::from_json<std::string>(R"("pizza 🍕 é")");
  EXPECT(p.has_value() && *p == "pizza \U0001F355 é");
}

// Serialization is read-only: spans over constexpr tables serialize without
// copying, and any type convertible to string_view serializes as a string
// (the constexpr-table-export pattern, examples.md §8b).
template <std::size_t N>
struct MiniFixed {
  char data[N]{};
  std::size_t len{};
  constexpr MiniFixed(const char* s) { while (s[len] && len < N - 1) { data[len] = s[len]; ++len; } }
  constexpr operator std::string_view() const { return {data, len}; }
};
struct TableRow { MiniFixed<16> name; std::uint32_t addr; };
inline constexpr TableRow kTable[] = { {"LedRed", 13}, {"Button", 11} };

static void test_views_and_fixed_strings() {
  struct Export { std::span<const TableRow> rows; };
  EXPECT_EQ(sardine::to_json(Export{kTable}),
            R"({"rows":[{"name":"LedRed","addr":13},{"name":"Button","addr":11}]})");
  // debug printing agrees that it's a string, not a struct
  EXPECT_EQ(sardine::debug(kTable[0]), R"(TableRow { name: "LedRed", addr: 13 })");
  // and the export round-trips into owning host-side mirrors
  struct HostRow { std::string name; std::uint32_t addr = 0; };
  struct HostExport { std::vector<HostRow> rows; };
  auto back = sardine::from_json<HostExport>(sardine::to_json(Export{kTable}));
  EXPECT(back.has_value() && back->rows.size() == 2 &&
         back->rows[1].name == "Button" && back->rows[1].addr == 11);
}

static void test_schema() {
  // annotations honored: rename, skip, required-per-level, unknown-fields policy
  EXPECT_EQ(sardine::schema<User>(),
            R"({"type":"object","title":"User","properties":{)"
            R"("user_id":{"type":"integer"},"name":{"type":"string"},)"
            R"("balance":{"type":"number"},"active":{"type":"boolean"}},)"
            R"("required":[],"additionalProperties":true})");
  // enums list their serialized names; optionals admit null; sequences nest
  struct Reading {
    [[=sardine::required{}]] std::string sensor;
    std::optional<double> value;
    std::vector<int> raw;
  };
  EXPECT_EQ(sardine::schema<Reading>(),
            R"({"type":"object","title":"Reading","properties":{)"
            R"("sensor":{"type":"string"},)"
            R"("value":{"anyOf":[{"type":"number"},{"type":"null"}]},)"
            R"("raw":{"type":"array","items":{"type":"integer"}}},)"
            R"("required":["sensor"],"additionalProperties":true})");
  enum class Mode { idle, running };
  EXPECT_EQ(sardine::schema<Mode>(), R"({"enum":["idle","running"]})");
}

// --- CBOR -------------------------------------------------------------------

static std::string hex(std::span<const std::uint8_t> bytes) {
  std::string s;
  for (auto b : bytes) std::format_to(std::back_inserter(s), "{:02x}", b);
  return s;
}

static std::vector<std::uint8_t> bytes(std::initializer_list<int> l) {
  return {l.begin(), l.end()};
}

static void test_cbor_encodings() {
  // RFC 8949 appendix A vectors
  EXPECT_EQ(hex(sardine::to_cbor(0)), "00");
  EXPECT_EQ(hex(sardine::to_cbor(23)), "17");
  EXPECT_EQ(hex(sardine::to_cbor(24)), "1818");
  EXPECT_EQ(hex(sardine::to_cbor(100000)), "1a000186a0");
  EXPECT_EQ(hex(sardine::to_cbor(-1)), "20");
  EXPECT_EQ(hex(sardine::to_cbor(-500)), "3901f3");
  EXPECT_EQ(hex(sardine::to_cbor(std::string("a"))), "6161");
  EXPECT_EQ(hex(sardine::to_cbor(true)), "f5");
  EXPECT_EQ(hex(sardine::to_cbor(std::optional<int>{})), "f6");
  EXPECT_EQ(hex(sardine::to_cbor(std::vector{1, 2, 3})), "83010203");
  EXPECT_EQ(hex(sardine::to_cbor(1.5)), "fb3ff8000000000000");
  EXPECT_EQ(hex(sardine::to_cbor(1.5f)), "fa3fc00000");
  // integer map keys stay integers (JSON stringifies them)
  EXPECT_EQ(hex(sardine::to_cbor(std::map<int, std::string>{{1, "a"}})), "a1016161");
  // enums serialize as their (renamed) names, like JSON
  EXPECT_EQ(hex(sardine::to_cbor(Color::green)), "65677265656e");
  // annotations honored: skip drops cache, rename keys the id
  User u{.id = 7, .cache = 99, .name = "Henri", .balance = 12.5, .active = true};
  EXPECT_EQ(hex(sardine::to_cbor(u)),
            "a4"                                   // map(4)
            "6775736572" "5f6964" "07"             // "user_id": 7
            "646e616d65" "6548656e7269"            // "name": "Henri"
            "6762616c616e6365" "fb4029000000000000"  // "balance": 12.5
            "66616374697665" "f5");                // "active": true
}

static void test_cbor_roundtrip() {
  User u{.id = 7, .cache = 99, .name = "Henri", .balance = 12.5, .active = true};
  auto back = sardine::from_cbor<User>(sardine::to_cbor(u));
  EXPECT(back.has_value());
  EXPECT_EQ(back->id, 7);
  EXPECT_EQ(back->cache, -1);  // skipped: keeps its default
  EXPECT_EQ(back->name, "Henri");
  EXPECT_EQ(back->balance, 12.5);
  EXPECT_EQ(back->active, true);

  Outer o{
      .inner = {.values = {1, 2, 3}, .note = "hi"},
      .scores = {{"alpha", 1.5}, {"beta", -2.0}},
      .history = {{.values = {4}, .note = std::nullopt}},
      .color = Color::blue,
      .level = Level::fatal_error,
  };
  auto ob = sardine::from_cbor<Outer>(sardine::to_cbor(o));
  EXPECT(ob.has_value());
  EXPECT((ob->inner.values == std::vector{1, 2, 3}));
  EXPECT(ob->inner.note == "hi");
  EXPECT_EQ(ob->scores.at("beta"), -2.0);
  EXPECT(ob->history[0].note == std::nullopt);
  EXPECT(ob->color == Color::blue);
  EXPECT(ob->level == Level::fatal_error);

  // variants: all three tagging modes
  ShapeHolder h{.ext = Rect{3, 4}, .u = std::string("hi"), .tagged = Circle{1.5}};
  auto hb = sardine::from_cbor<ShapeHolder>(sardine::to_cbor(h));
  EXPECT(hb.has_value());
  EXPECT(std::holds_alternative<Rect>(hb->ext));
  EXPECT((hb->u == std::variant<int, std::string>(std::string("hi"))));
  EXPECT(std::holds_alternative<Circle>(hb->tagged));

  // flatten: hoisted fields and the catch-all map
  Doc d{.title = "t",
        .meta = {.version = 2, .author = "hs"},
        .extra = {{"x", "1"}, {"y", "2"}}};
  auto db = sardine::from_cbor<Doc>(sardine::to_cbor(d));
  EXPECT(db.has_value());
  EXPECT_EQ(db->meta.author, "hs");
  EXPECT_EQ(db->extra.at("y"), "2");
  EXPECT_EQ(db->extra.size(), 2uz);

  // integer map keys, unit variants, unnamed enum values
  std::map<int, std::string> im{{-3, "a"}, {200, "b"}};
  auto imb = sardine::from_cbor<std::map<int, std::string>>(sardine::to_cbor(im));
  EXPECT(imb.has_value() && *imb == im);
  using MaybeShape = std::variant<std::monostate, Circle>;
  auto mb = sardine::from_cbor<MaybeShape>(sardine::to_cbor(MaybeShape{}));
  EXPECT(mb.has_value() && std::holds_alternative<std::monostate>(*mb));
  auto c9 = sardine::from_cbor<Color>(sardine::to_cbor(static_cast<Color>(9)));
  EXPECT(c9.has_value() && *c9 == static_cast<Color>(9));

  // nan/inf survive CBOR (JSON degrades them to null)
  auto nb = sardine::from_cbor<double>(
      sardine::to_cbor(std::numeric_limits<double>::infinity()));
  EXPECT(nb.has_value() && *nb == std::numeric_limits<double>::infinity());
}

static void test_cbor_decoder_lenience() {
  // indefinite-length array, map, and text are accepted on input
  auto a = sardine::from_cbor<std::vector<int>>(bytes({0x9f, 0x01, 0x02, 0xff}));
  EXPECT(a.has_value() && (*a == std::vector{1, 2}));
  auto s = sardine::from_cbor<std::string>(
      bytes({0x7f, 0x61, 0x61, 0x61, 0x62, 0xff}));  // "a" + "b" chunks
  EXPECT(s.has_value() && *s == "ab");
  auto m = sardine::from_cbor<std::map<std::string, int>>(
      bytes({0xbf, 0x61, 0x61, 0x01, 0xff}));
  EXPECT(m.has_value() && m->at("a") == 1);
  // half-precision float (f9 3c00 = 1.0), and ints promote to float targets
  auto hf = sardine::from_cbor<double>(bytes({0xf9, 0x3c, 0x00}));
  EXPECT(hf.has_value() && *hf == 1.0);
  auto i2f = sardine::from_cbor<double>(bytes({0x18, 0x2a}));
  EXPECT(i2f.has_value() && *i2f == 42.0);
  // semantic tags are skipped (tag 0 on a text string)
  auto t = sardine::from_cbor<std::string>(bytes({0xc0, 0x61, 0x61}));
  EXPECT(t.has_value() && *t == "a");
  // text-encoded integer map keys accepted (JSON-converted documents)
  auto tm = sardine::from_cbor<std::map<int, int>>(bytes({0xa1, 0x61, 0x31, 0x05}));
  EXPECT(tm.has_value() && tm->at(1) == 5);
  // undefined (f7) reads as null for optionals
  auto u = sardine::from_cbor<std::optional<int>>(bytes({0xf7}));
  EXPECT(u.has_value() && !u->has_value());
}

static void test_cbor_errors() {
  EXPECT(!sardine::from_cbor<int>(bytes({})).has_value());
  EXPECT(!sardine::from_cbor<int>(bytes({0x19, 0x01})).has_value());  // truncated
  EXPECT(!sardine::from_cbor<int>(bytes({0x01, 0x02})).has_value());  // trailing
  EXPECT(!sardine::from_cbor<int>(bytes({0x61, 0x61})).has_value());  // text, not int
  EXPECT(!sardine::from_cbor<std::uint8_t>(bytes({0x19, 0x01, 0x00})).has_value());
  EXPECT(!sardine::from_cbor<unsigned>(bytes({0x20})).has_value());  // -1 → unsigned
  EXPECT(!sardine::from_cbor<int>(bytes({0xfb, 0x3f, 0xf8, 0, 0, 0, 0, 0, 0}))
             .has_value());  // float, not int (JSON parity: from_json<int>("1.5"))
  EXPECT(!sardine::from_cbor<std::vector<int>>(bytes({0x9f, 0x01})).has_value());

  // strictness knobs behave as in JSON
  auto strict = sardine::from_cbor<Strict>(
      sardine::to_cbor(std::map<std::string, int>{{"a", 1}, {"b", 2}}));
  EXPECT(!strict.has_value());
  auto req = sardine::from_cbor<WithRequired>(
      sardine::to_cbor(std::map<std::string, int>{{"optional_field", 2}}));
  EXPECT(!req.has_value());
  if (!req) EXPECT(req.error().message.find("must") != std::string::npos);
  EXPECT(!sardine::from_cbor<Shape>(sardine::to_cbor(std::string("Triangle")))
             .has_value());
}

static void test_errors() {
  EXPECT(!sardine::from_json<User>(R"({"user_id": )").has_value());
  EXPECT(!sardine::from_json<User>(R"([1,2])").has_value());
  EXPECT(!sardine::from_json<int>(R"(1.5)").has_value());
  EXPECT(!sardine::from_json<int>(R"(1 2)").has_value());   // trailing data
  EXPECT(!sardine::from_json<std::string>("\"a\nb\"").has_value());  // raw ctrl char

  auto e = sardine::from_json<User>(R"({"name": 12})");
  EXPECT(!e.has_value());
  if (!e) EXPECT(e.error().offset > 0);
}

int main() {
  test_basic_roundtrip();
  test_pretty_json();
  test_debug();
  test_rename_all();
  test_enums();
  test_variants();
  test_flatten();
  test_strictness();
  test_int_map();
  test_nested();
  test_optionals();
  test_unknown_and_missing_fields();
  test_directional_skips();
  test_string_escapes();
  test_views_and_fixed_strings();
  test_schema();
  test_cbor_encodings();
  test_cbor_roundtrip();
  test_cbor_decoder_lenience();
  test_cbor_errors();
  test_errors();

  if (failures == 0) std::println("all tests passed");
  else std::println("{} FAILURES", failures);
  return failures != 0;
}
