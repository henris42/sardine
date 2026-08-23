// sardine on Compiler Explorer — two claims, both checked by the compiler:
//
//  1. CORRECT: the static_asserts below are verified during compilation
//     (green check = proof for those inputs), and main() re-checks the
//     runtime paths — run the program in the Execution pane.
//
//  2. REDUCES: open the asm pane at -O2. There is no reflection at runtime:
//     serialization is flat string appends of .rodata literals ("user_id",
//     "name", ...), enum names are a compile-time table, and the parser is
//     an ordinary hand-rolled-looking scanner.
//
// Local build:   g++ -std=c++26 -freflection -O2 examples/godbolt.cpp
// On godbolt:    swap the include for the raw.githubusercontent.com URL.
#include <sardine/sardine.hpp>

#include <cassert>
#include <print>

struct User {
  [[=sardine::rename("user_id")]] int id = 0;
  [[=sardine::skip{}]]            int cache = -1;
  std::string name;
  double balance = 0;
};

enum class Color { red, green, deep_blue };

// ---------------------------------------------------------------------------
// 1a. Compile-time correctness: field-name resolution is consteval, so the
//     rename/rename_all logic is provable with static_assert today.
// ---------------------------------------------------------------------------
namespace ct {
consteval std::string_view field_name(std::size_t i) {
  return sardine::detail::json_name<User>(sardine::detail::members_of<User>()[i]);
}
static_assert(field_name(0) == "user_id");   // [[=rename]] honored
static_assert(field_name(2) == "name");      // plain identifier
static_assert(sardine::detail::skip_ser(sardine::detail::members_of<User>()[1]));

static_assert(sardine::detail::convert_case("deep_blue",
              sardine::case_style::lower_camel) == "deepBlue");
}  // namespace ct

// ---------------------------------------------------------------------------
// 2. Codegen witnesses — read these in the asm pane.
// ---------------------------------------------------------------------------

// Flat appends; "user_id"/"name"/"balance" are .rodata, no metadata walk.
std::string ser(const User& u) { return sardine::to_json(u); }

// Enum -> string: compile-time stamped comparisons against static names.
std::string color(Color c) { return sardine::to_json(c); }

// The parser: plain scanner code, exceptions only on the error path.
std::expected<User, sardine::error> de(std::string_view j) {
  return sardine::from_json<User>(j);
}

// ---------------------------------------------------------------------------
// 1b. Runtime correctness: executed on godbolt (Execution pane).
// ---------------------------------------------------------------------------
int main() {
  User u{.id = 7, .name = "Henri \"H\" S", .balance = 12.5};

  std::string j = ser(u);
  assert(j == R"({"user_id":7,"name":"Henri \"H\" S","balance":12.5})");

  // roundtrip: everything serialized comes back identical
  User b = de(j).value();
  assert(b.id == u.id && b.name == u.name && b.balance == u.balance);
  assert(b.cache == -1);                        // skipped field keeps default

  assert(sardine::to_json(Color::deep_blue) == R"("deep_blue")");
  assert(sardine::from_json<Color>(R"("red")").value() == Color::red);

  // malformed input is rejected with an offset, not UB
  auto bad = de(R"({"user_id":)");
  assert(!bad.has_value() && bad.error().offset == 11);
  auto trail = de(R"({} x)");
  assert(!trail.has_value());

  // \uXXXX incl. surrogate pairs
  assert(sardine::from_json<std::string>(R"("😀")").value() ==
         "\xf0\x9f\x98\x80");

  std::println("all godbolt checks passed");
  std::println("{}", sardine::to_json_pretty(u));
}
