#include <sardine/sardine.hpp>

#include <map>
#include <print>
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
  Outer o{.inner = {.values = {3}, .note = "n"}, .scores = {{"a", 1}}};
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
  test_errors();

  if (failures == 0) std::println("all tests passed");
  else std::println("{} FAILURES", failures);
  return failures != 0;
}
