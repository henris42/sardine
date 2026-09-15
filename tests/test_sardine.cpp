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
// Numeric enum reads are opt-in; Mode takes the opt-in, Color stays strict.
enum class [[=sardine::enum_from_number{}]] Mode { off, on };

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

  // A disengaged optional member is omitted, not written as null.
  Inner i{.values = {1, 2}, .note = std::nullopt};
  EXPECT_EQ(sardine::to_json_pretty(i),
            "{\n"
            "  \"values\": [\n"
            "    1,\n"
            "    2\n"
            "  ]\n"
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

  // Un-named enum value falls back to the underlying integer on WRITE; the
  // read side takes a number only where the enum opted in.
  EXPECT_EQ(sardine::to_json(static_cast<Color>(9)), "9");
  auto c9 = sardine::from_json<Color>("9");
  EXPECT(!c9.has_value());
  EXPECT(c9.error().code == sardine::errc::type_mismatch);

  auto m1 = sardine::from_json<Mode>("1");
  EXPECT(m1.has_value() && *m1 == Mode::on);
  auto m9 = sardine::from_json<Mode>("9");
  EXPECT(m9.has_value() && *m9 == static_cast<Mode>(9));

  auto bad = sardine::from_json<Color>(R"("magenta")");
  EXPECT(!bad.has_value());
  EXPECT(bad.error().code == sardine::errc::unknown_enum);
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
  EXPECT(!c9.has_value());  // numeric enum reads are opt-in (enum_from_number)
  auto m9 = sardine::from_cbor<Mode>(sardine::to_cbor(static_cast<Mode>(9)));
  EXPECT(m9.has_value() && *m9 == static_cast<Mode>(9));

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


// --- new-feature tests --------------------------------------------------------

static void test_error_codes_and_paths() {
  struct Leaf { [[=sardine::required{}]] int n = 0; };
  struct [[=sardine::deny_unknown_fields{}]] Doc {
    std::string name;
    std::vector<Leaf> items;
  };

  // unknown field: code + dotted path naming the offender
  auto u = sardine::from_json<Doc>(R"({"name":"x","surprise":1})");
  EXPECT(!u.has_value());
  EXPECT(u.error().code == sardine::errc::unknown_field);
  EXPECT_EQ(u.error().path, "surprise");

  // type mismatch deep inside an array: "items.0.n"
  auto t = sardine::from_json<Doc>(R"({"items":[{"n":"nope"}]})");
  EXPECT(!t.has_value());
  EXPECT(t.error().code == sardine::errc::type_mismatch);
  EXPECT_EQ(t.error().path, "items.0.n");

  // missing required field: path names it
  auto m = sardine::from_json<Doc>(R"({"items":[{}]})");
  EXPECT(!m.has_value());
  EXPECT(m.error().code == sardine::errc::missing_field);
  EXPECT_EQ(m.error().path, "items.0.n");

  // fractional where an integer belongs is a type error, garbage is not
  auto frac = sardine::from_json<int>("1.5");
  EXPECT(!frac.has_value() && frac.error().code == sardine::errc::type_mismatch);
  auto junk = sardine::from_json<int>("--");
  EXPECT(!junk.has_value() && junk.error().code == sardine::errc::invalid_number);

  auto trail = sardine::from_json<int>("1 2");
  EXPECT(!trail.has_value() && trail.error().code == sardine::errc::trailing);

  // CBOR carries the same codes and paths
  auto cd = sardine::from_cbor<Doc>(
      bytes({0xa1, 0x64, 'n', 'a', 'm', 'e', 0x01}));
  EXPECT(!cd.has_value());
  EXPECT(cd.error().code == sardine::errc::type_mismatch);
  EXPECT_EQ(cd.error().path, "name");
}

static void test_cbor_strict_profile() {
  using sardine::cbor_strict;

  // Non-minimal heads: 23 in a one-byte argument, 24 in a two-byte argument.
  auto nm1 = sardine::from_cbor<std::uint64_t>(bytes({0x18, 0x17}), cbor_strict);
  EXPECT(!nm1.has_value() && nm1.error().code == sardine::errc::invalid_encoding);
  auto nm2 = sardine::from_cbor<std::uint64_t>(bytes({0x19, 0x00, 0x18}), cbor_strict);
  EXPECT(!nm2.has_value());
  // ...while the minimal spellings of the boundary values pass.
  auto ok24 = sardine::from_cbor<std::uint64_t>(bytes({0x18, 0x18}), cbor_strict);
  EXPECT(ok24.has_value() && *ok24 == 24);
  auto ok256 = sardine::from_cbor<std::uint64_t>(bytes({0x19, 0x01, 0x00}), cbor_strict);
  EXPECT(ok256.has_value() && *ok256 == 256);

  // Indefinite lengths.
  auto ind = sardine::from_cbor<std::vector<int>>(bytes({0x9f, 0x01, 0xff}), cbor_strict);
  EXPECT(!ind.has_value() && ind.error().code == sardine::errc::invalid_encoding);

  // Tags.
  auto tag = sardine::from_cbor<std::string>(bytes({0xc0, 0x60}), cbor_strict);
  EXPECT(!tag.has_value() && tag.error().code == sardine::errc::invalid_encoding);

  // Substitutions: half float, int-as-float, undefined-as-null, text int key.
  auto half = sardine::from_cbor<double>(bytes({0xf9, 0x3c, 0x00}), cbor_strict);
  EXPECT(!half.has_value());
  auto intf = sardine::from_cbor<double>(bytes({0x01}), cbor_strict);
  EXPECT(!intf.has_value());
  auto undef = sardine::from_cbor<std::optional<int>>(bytes({0xf7}), cbor_strict);
  EXPECT(!undef.has_value());
  auto tkey = sardine::from_cbor<std::map<int, int>>(
      bytes({0xa1, 0x61, '3', 0x01}), cbor_strict);
  EXPECT(!tkey.has_value());

  // The default profile still takes all of them.
  EXPECT(sardine::from_cbor<std::uint64_t>(bytes({0x18, 0x17})).has_value());
  EXPECT(sardine::from_cbor<std::vector<int>>(bytes({0x9f, 0x01, 0xff})).has_value());
  EXPECT(sardine::from_cbor<std::string>(bytes({0xc0, 0x60})).has_value());
  EXPECT(sardine::from_cbor<double>(bytes({0xf9, 0x3c, 0x00})).has_value());
}

static void test_cbor_wellformedness() {
  // RFC 8949 §3.3: additional info 31 is indefinite length (majors 2-5) or
  // break (major 7) — on an integer or tag head it is not well-formed. These
  // used to decode as 0, -1, and tag 0.
  auto i0 = sardine::from_cbor<int>(bytes({0x1f}));
  EXPECT(!i0.has_value() && i0.error().code == sardine::errc::invalid_encoding);
  auto i1 = sardine::from_cbor<int>(bytes({0x3f}));
  EXPECT(!i1.has_value() && i1.error().code == sardine::errc::invalid_encoding);
  auto tg = sardine::from_cbor<int>(bytes({0xdf, 0x00}));
  EXPECT(!tg.has_value() && tg.error().code == sardine::errc::invalid_encoding);

  // A two-byte simple value below 32 is not well-formed (0xf8 0x1f).
  auto sv = sardine::from_cbor<sardine::cbor_raw>(bytes({0xf8, 0x1f}));
  EXPECT(!sv.has_value() && sv.error().code == sardine::errc::invalid_encoding);
  // ...while 32 is (0xf8 0x20), even under minimal_heads (major 7 exempt).
  auto ok = sardine::from_cbor<sardine::cbor_raw>(bytes({0xf8, 0x20}),
                                                  sardine::cbor_strict);
  EXPECT(ok.has_value());

  // A break outside an indefinite container is invalid_encoding, not a
  // type mismatch — fuzz oracles classify on the code.
  auto br = sardine::from_cbor<int>(bytes({0xff}));
  EXPECT(!br.has_value() && br.error().code == sardine::errc::invalid_encoding);
  auto braw = sardine::from_cbor<sardine::cbor_raw>(bytes({0xff}));
  EXPECT(!braw.has_value() &&
         braw.error().code == sardine::errc::invalid_encoding);

  // The empty indefinite array stays valid; bad chunk types and nested
  // indefinite text chunks stay refused.
  EXPECT(sardine::from_cbor<sardine::cbor_raw>(bytes({0x9f, 0xff})).has_value());
  auto chunk = sardine::from_cbor<sardine::cbor_raw>(bytes({0x5f, 0x00, 0xff}));
  EXPECT(!chunk.has_value() &&
         chunk.error().code == sardine::errc::invalid_encoding);
  auto nest = sardine::from_cbor<sardine::cbor_raw>(
      bytes({0x7f, 0x7f, 0x61, 0x61, 0xff, 0xff}));
  EXPECT(!nest.has_value() &&
         nest.error().code == sardine::errc::invalid_encoding);
}

static void test_cbor_bytes() {
  // RFC 8949 appendix A: h\'\' and h\'01020304\'.
  EXPECT_EQ(sardine::to_cbor(std::vector<std::uint8_t>{}), bytes({0x40}));
  EXPECT_EQ(sardine::to_cbor(std::vector<std::uint8_t>{1, 2, 3, 4}),
            bytes({0x44, 0x01, 0x02, 0x03, 0x04}));

  auto back = sardine::from_cbor<std::vector<std::uint8_t>>(
      bytes({0x44, 0x01, 0x02, 0x03, 0x04}));
  EXPECT(back.has_value() && (*back == std::vector<std::uint8_t>{1, 2, 3, 4}));

  // std::array and std::span write as byte strings too.
  std::array<std::uint8_t, 2> arr{0xaa, 0xbb};
  EXPECT_EQ(sardine::to_cbor(arr), bytes({0x42, 0xaa, 0xbb}));
  EXPECT_EQ(sardine::to_cbor(std::span<const std::uint8_t>(arr)),
            bytes({0x42, 0xaa, 0xbb}));

  // As a struct member.
  struct Blob { std::vector<std::uint8_t> data; };
  Blob b{.data = {0xde, 0xad}};
  EXPECT_EQ(sardine::to_cbor(b),
            bytes({0xa1, 0x64, 'd', 'a', 't', 'a', 0x42, 0xde, 0xad}));
  auto bb = sardine::from_cbor<Blob>(sardine::to_cbor(b));
  EXPECT(bb.has_value() && bb->data == b.data);

  // Lenient mode also reads the pre-bytes spelling: an array of integers.
  auto legacy = sardine::from_cbor<std::vector<std::uint8_t>>(
      bytes({0x82, 0x01, 0x02}));
  EXPECT(legacy.has_value() && (*legacy == std::vector<std::uint8_t>{1, 2}));
  auto strict_legacy = sardine::from_cbor<std::vector<std::uint8_t>>(
      bytes({0x82, 0x01, 0x02}), sardine::cbor_strict);
  EXPECT(!strict_legacy.has_value());

  // Indefinite-length byte string: chunks concatenate (lenient only).
  auto chunked = sardine::from_cbor<std::vector<std::uint8_t>>(
      bytes({0x5f, 0x41, 0x01, 0x41, 0x02, 0xff}));
  EXPECT(chunked.has_value() && (*chunked == std::vector<std::uint8_t>{1, 2}));

  // JSON side is unchanged: bytes are an array of numbers.
  EXPECT_EQ(sardine::to_json(std::vector<std::uint8_t>{1, 2}), "[1,2]");
}

// COSE_Key-shaped: RFC 9052 integer labels, negative for the EC2 parameters.
struct CoseKey {
  [[=sardine::int_key(1), =sardine::required{}]] std::int64_t kty = 0;
  [[=sardine::int_key(3), =sardine::required{}]] std::int64_t alg = 0;
  [[=sardine::int_key(-1)]] std::int64_t crv = 0;
  [[=sardine::int_key(-2)]] std::vector<std::uint8_t> x;
  [[=sardine::int_key(-3)]] std::optional<std::vector<std::uint8_t>> y;
};

static void test_int_key() {
  CoseKey k{.kty = 2, .alg = -7, .crv = 1, .x = {0x11, 0x22}, .y = std::nullopt};
  // {1: 2, 3: -7, -1: 1, -2: h\'1122\'} — y omitted while disengaged.
  EXPECT_EQ(sardine::to_cbor(k),
            bytes({0xa4, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01, 0x21, 0x42, 0x11,
                   0x22}));
  auto back = sardine::from_cbor<CoseKey>(sardine::to_cbor(k));
  EXPECT(back.has_value());
  EXPECT(back->kty == 2 && back->alg == -7 && back->crv == 1);
  EXPECT((back->x == std::vector<std::uint8_t>{0x11, 0x22}) && !back->y);

  // Unknown integer labels skip like unknown text keys (no deny on CoseKey).
  auto extra = sardine::from_cbor<CoseKey>(
      bytes({0xa3, 0x01, 0x02, 0x03, 0x26, 0x04, 0x63, 'e', 'x', 't'}));
  EXPECT(extra.has_value() && extra->kty == 2);

  // A required int_key member missing: code + decimal path.
  auto missing = sardine::from_cbor<CoseKey>(bytes({0xa1, 0x01, 0x02}));
  EXPECT(!missing.has_value());
  EXPECT(missing.error().code == sardine::errc::missing_field);
  EXPECT_EQ(missing.error().path, "3");

  // JSON spells the labels in decimal.
  EXPECT_EQ(sardine::to_json(k), R"({"1":2,"3":-7,"-1":1,"-2":[17,34]})");
  auto jback = sardine::from_json<CoseKey>(sardine::to_json(k));
  EXPECT(jback.has_value() && jback->alg == -7);
}

static void test_duplicate_keys() {
  constexpr sardine::json_options reject{.reject_duplicate_keys = true};

  // Default: last occurrence wins, matching common JSON parsers.
  auto lax = sardine::from_json<User>(R"({"user_id":1,"user_id":2,"name":"n"})");
  EXPECT(lax.has_value() && lax->id == 2);
  // Under the option the same document is one signature covering two
  // meanings — refused.
  auto dup =
      sardine::from_json<User>(R"({"user_id":1,"user_id":2,"name":"n"})", reject);
  EXPECT(!dup.has_value() && dup.error().code == sardine::errc::duplicate_field);
  EXPECT_EQ(dup.error().path, "user_id");

  // Duplicates inside a flattened level and among skipped unknowns count too.
  auto flat = sardine::from_json<Doc>(
      R"({"title":"t","version":1,"version":2})", reject);
  EXPECT(!flat.has_value() &&
         flat.error().code == sardine::errc::duplicate_field);
  auto unk = sardine::from_json<User>(R"({"name":"n","w":1,"w":2})", reject);
  EXPECT(!unk.has_value() && unk.error().code == sardine::errc::duplicate_field);

  // Map targets, string- and int-keyed.
  auto m = sardine::from_json<std::map<std::string, int>>(R"({"a":1,"a":2})",
                                                          reject);
  EXPECT(!m.has_value() && m.error().code == sardine::errc::duplicate_field);
  auto ml = sardine::from_json<std::map<std::string, int>>(R"({"a":1,"a":2})");
  EXPECT(ml.has_value() && ml->size() == 1uz && ml->at("a") == 2);
  auto im = sardine::from_json<std::map<int, int>>(R"({"1":1,"1":2})", reject);
  EXPECT(!im.has_value() && im.error().code == sardine::errc::duplicate_field);

  // sardine::value keeps duplicates by design (a document is evidence);
  // value is therefore not for signed wires.
  auto v = sardine::from_json<sardine::value>(R"({"a":1,"a":2})", reject);
  EXPECT(v.has_value() && v->as_object().size() == 2uz);

  // CBOR: cbor_strict now rejects; the default profile keeps last-wins.
  auto ct = sardine::from_cbor<std::map<std::string, int>>(
      bytes({0xa2, 0x61, 'a', 0x01, 0x61, 'a', 0x02}), sardine::cbor_strict);
  EXPECT(!ct.has_value() && ct.error().code == sardine::errc::duplicate_field);
  auto ctl = sardine::from_cbor<std::map<std::string, int>>(
      bytes({0xa2, 0x61, 'a', 0x01, 0x61, 'a', 0x02}));
  EXPECT(ctl.has_value() && ctl->at("a") == 2);
  auto ci = sardine::from_cbor<std::map<int, int>>(
      bytes({0xa2, 0x01, 0x01, 0x01, 0x02}), sardine::cbor_strict);
  EXPECT(!ci.has_value() && ci.error().code == sardine::errc::duplicate_field);
  // Struct target with a repeated int_key label: {1: 2, 1: 2, 3: -7}.
  auto ck = sardine::from_cbor<CoseKey>(
      bytes({0xa3, 0x01, 0x02, 0x01, 0x02, 0x03, 0x26}), sardine::cbor_strict);
  EXPECT(!ck.has_value() && ck.error().code == sardine::errc::duplicate_field);
}

static void test_int_key_text_spelling() {
  // {1: 2, "3": -7}: under no_substitutions a text key must not match an
  // int_key member's decimal spelling, so required alg goes unseen.
  auto strict = sardine::from_cbor<CoseKey>(
      bytes({0xa2, 0x01, 0x02, 0x61, '3', 0x26}), sardine::cbor_strict);
  EXPECT(!strict.has_value() &&
         strict.error().code == sardine::errc::missing_field);
  // The lenient default keeps accepting the text spelling.
  auto lax = sardine::from_cbor<CoseKey>(
      bytes({0xa2, 0x01, 0x02, 0x61, '3', 0x26}));
  EXPECT(lax.has_value() && lax->alg == -7);

  // Both spellings in one map: {1: 2, 3: -7, "3": -100}. Strict skips the
  // text one as an unknown key (it is a DIFFERENT key, not a duplicate);
  // lenient last-wins lets it overwrite alg.
  auto both = sardine::from_cbor<CoseKey>(
      bytes({0xa3, 0x01, 0x02, 0x03, 0x26, 0x61, '3', 0x38, 0x63}),
      sardine::cbor_strict);
  EXPECT(both.has_value() && both->alg == -7);
  auto overwrite = sardine::from_cbor<CoseKey>(
      bytes({0xa3, 0x01, 0x02, 0x03, 0x26, 0x61, '3', 0x38, 0x63}));
  EXPECT(overwrite.has_value() && overwrite->alg == -100);

  // JSON is unaffected: its keys are only ever text.
  auto j = sardine::from_json<CoseKey>(R"({"1":2,"3":-7})",
                                       {.reject_duplicate_keys = true});
  EXPECT(j.has_value() && j->kty == 2 && j->alg == -7);
}

static void test_cbor_raw() {
  struct Att {
    std::string fmt;
    [[=sardine::rename("attStmt")]] sardine::cbor_raw att_stmt;
    std::vector<std::uint8_t> auth;
  };
  // {"fmt":"none","attStmt":{},"auth":h\'ff\'}
  auto wire = bytes({0xa3, 0x63, 'f', 'm', 't', 0x64, 'n', 'o', 'n', 'e',
                     0x67, 'a', 't', 't', 'S', 't', 'm', 't', 0xa0,
                     0x64, 'a', 'u', 't', 'h', 0x41, 0xff});
  auto a = sardine::from_cbor<Att>(wire);
  EXPECT(a.has_value());
  EXPECT_EQ(a->fmt, "none");
  EXPECT_EQ(a->att_stmt.bytes, bytes({0xa0}));  // verbatim, uninterpreted
  EXPECT_EQ(a->auth, bytes({0xff}));
  // Round-trip splices the raw item back byte-for-byte.
  EXPECT_EQ(sardine::to_cbor(*a), wire);

  // The raw item must still be well-formed...
  auto bad = sardine::from_cbor<sardine::cbor_raw>(bytes({0xa1, 0x01}));
  EXPECT(!bad.has_value());
  // ...and the active profile applies inside it.
  auto ind = sardine::from_cbor<sardine::cbor_raw>(bytes({0x9f, 0x01, 0xff}),
                                                   sardine::cbor_strict);
  EXPECT(!ind.has_value());

  // Empty raw writes null.
  EXPECT_EQ(sardine::to_cbor(sardine::cbor_raw{}), bytes({0xf6}));
}

static void test_value_tree() {
  // Any document parses; order and duplicate keys survive; find is first-wins.
  auto v = sardine::from_json<sardine::value>(
      R"({"z":1,"a":[true,null,"s",2.5],"z":2})");
  EXPECT(v.has_value());
  EXPECT(v->is_object() && v->as_object().size() == 3);
  EXPECT(v->find("z") && v->find("z")->as_int() == 1);
  const auto& arr = v->find("a")->as_array();
  EXPECT(arr.size() == 4 && arr[0].as_bool() && arr[1].is_null());
  EXPECT(arr[2].as_string() == "s" && arr[3].as_double() == 2.5);

  // Dump is compact, insertion-ordered, shortest-round-trip numbers.
  EXPECT_EQ(sardine::to_json(*v), R"({"z":1,"a":[true,null,"s",2.5],"z":2})");

  // int64 overflow degrades to double instead of failing.
  auto big = sardine::from_json<sardine::value>("18446744073709551616");
  EXPECT(big.has_value() && big->is_double());

  // Works as a struct member: the typed envelope / generic payload split.
  struct Envelope {
    std::string kind;
    std::optional<sardine::value> payload;
  };
  auto e = sardine::from_json<Envelope>(R"({"kind":"x","payload":{"n":[1]}})");
  EXPECT(e.has_value() && e->payload.has_value());
  EXPECT(e->payload->find("n")->as_array()[0].as_int() == 1);
  EXPECT_EQ(sardine::to_json(*e), R"({"kind":"x","payload":{"n":[1]}})");

  // Errors inside a document still carry paths.
  auto bad = sardine::from_json<sardine::value>(R"({"a":[1,)");
  EXPECT(!bad.has_value());
  EXPECT(bad.error().code == sardine::errc::truncated);
}

static void test_from_json_into() {
  struct Limits { int lo = 1; int hi = 99; };
  struct Cfg {
    std::string name = "default";
    std::optional<int> timeout = 30;
    Limits limits;
    std::vector<int> tags = {1, 2};
  };

  Cfg c;
  // Absent fields keep their values; nested structs merge; containers replace;
  // an explicit null resets an optional.
  auto r = sardine::from_json_into(
      R"({"timeout":null,"limits":{"hi":50},"tags":[9]})", c);
  EXPECT(r.has_value());
  EXPECT_EQ(c.name, "default");            // absent: kept
  EXPECT(!c.timeout.has_value());          // null: reset
  EXPECT(c.limits.lo == 1 && c.limits.hi == 50);  // nested merge
  EXPECT((c.tags == std::vector{9}));      // container replaced whole

  // Errors leave a diagnosable result and report like from_json.
  auto bad = sardine::from_json_into("{bad", c);
  EXPECT(!bad.has_value());
}

static void test_cbor_prefix() {
  // One item decoded off the front; the caller learns where it ended.
  auto buf = bytes({0xa1, 0x01, 0x02, /* trailing: */ 0xde, 0xad});
  std::size_t used = 0;
  auto m = sardine::from_cbor_prefix<std::map<int, int>>(buf, used);
  EXPECT(m.has_value() && m->at(1) == 2);
  EXPECT_EQ(used, 3uz);

  // from_cbor on the same buffer refuses the trailing bytes.
  auto whole = sardine::from_cbor<std::map<int, int>>(buf);
  EXPECT(!whole.has_value() && whole.error().code == sardine::errc::trailing);
}

static void test_omit_none_and_emit_null() {
  struct P {
    std::optional<int> a;
    [[=sardine::emit_null{}]] std::optional<int> b;
  };
  // a vanishes when disengaged; b opted back into explicit null.
  EXPECT_EQ(sardine::to_json(P{}), R"({"b":null})");
  EXPECT_EQ(sardine::to_json(P{.a = 1, .b = 2}), R"({"a":1,"b":2})");
  // CBOR map arity matches what is actually emitted.
  EXPECT_EQ(sardine::to_cbor(P{}), bytes({0xa1, 0x61, 'b', 0xf6}));
  EXPECT_EQ(sardine::to_cbor(P{.a = 1, .b = std::nullopt}),
            bytes({0xa2, 0x61, 'a', 0x01, 0x61, 'b', 0xf6}));
  // Reading is symmetric: absent keeps the default, null resets.
  auto back = sardine::from_json<P>(R"({"b":null})");
  EXPECT(back.has_value() && !back->a && !back->b);
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
  test_error_codes_and_paths();
  test_cbor_strict_profile();
  test_cbor_wellformedness();
  test_cbor_bytes();
  test_int_key();
  test_duplicate_keys();
  test_int_key_text_spelling();
  test_cbor_raw();
  test_value_tree();
  test_from_json_into();
  test_cbor_prefix();
  test_omit_none_and_emit_null();

  if (failures == 0) std::println("all tests passed");
  else std::println("{} FAILURES", failures);
  return failures != 0;
}
