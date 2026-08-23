// sardine.hpp — Rust-Serde-style JSON serialization for C++26.
//
// Uses P2996 compile-time reflection as the engine and P3394 annotations
// as the user-facing attribute syntax:
//
//   struct User {
//     [[=sardine::rename("user_id")]] int         id;
//     [[=sardine::skip{}]]            int         cache;
//                                   std::string name;
//   };
//
//   std::string j  = sardine::to_json(u);
//   std::string jp = sardine::to_json_pretty(u);
//   std::expected<User, sardine::error> u2 = sardine::from_json<User>(j);
//
//   std::println("{}",  sardine::dbg(u));   // Rust {:?}  : User { id: 7, ... }
//   std::println("{:#}", sardine::dbg(u));  // Rust {:#?} : multi-line
//
// Requires: g++ >= 16.1 with -std=c++26 -freflection
#pragma once

#include <meta>
#include <array>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace sardine {

// ---------------------------------------------------------------------------
// Annotations.
//
// Annotation objects must have structural type, and pointers into string
// literals do not survive constant normalization — so names are stored by
// value in a fixed char buffer.
// ---------------------------------------------------------------------------

inline constexpr unsigned max_name_length = 64;

namespace detail {
  struct fixed_string {
    char text[max_name_length] = {};
    unsigned size = 0;
    consteval fixed_string(const char* s) {
      for (; s[size]; ++size) {
        if (size >= max_name_length) throw "sardine: annotation name too long";
        text[size] = s[size];
      }
    }
    constexpr std::string_view str() const { return {text, size}; }
  };
}

// [[=sardine::rename("json_name")]] — serialize/deserialize under this key.
struct rename : detail::fixed_string {
  using fixed_string::fixed_string;
};

// [[=sardine::skip{}]] — field is invisible to both directions.
struct skip {};
// [[=sardine::skip_serializing{}]] — never written; still read if present.
struct skip_serializing {};
// [[=sardine::skip_deserializing{}]] — never read; still written.
struct skip_deserializing {};

// [[=sardine::flatten{}]] — on a struct member: hoist its fields into the
// parent object. On a map member: catch-all for keys no field matched.
struct flatten {};

// [[=sardine::required{}]] — deserialization fails if the field is absent.
struct required {};

// [[=sardine::deny_unknown_fields{}]] — on a struct: unknown keys are an
// error instead of being ignored.
struct deny_unknown_fields {};

// [[=sardine::untagged{}]] — on a std::variant member: no tag, alternatives
// are tried in order until one parses.
struct untagged {};

// [[=sardine::tag("type")]] — on a std::variant member whose alternatives are
// all structs: internally tagged, {"type":"AltName", ...fields}.
struct tag : detail::fixed_string {
  using fixed_string::fixed_string;
};

enum class case_style : std::uint8_t {
  lower_camel, pascal, snake, screaming_snake, kebab, screaming_kebab,
};

// [[=sardine::rename_all("camelCase")]] on a struct or enum — converts every
// field/enumerator identifier (assumed snake_case, the C++ norm) to the
// given style. Accepts the same spellings as Serde.
struct rename_all {
  case_style style;
  consteval rename_all(std::string_view s)
    : style(s == "camelCase"            ? case_style::lower_camel
          : s == "PascalCase"           ? case_style::pascal
          : s == "snake_case"           ? case_style::snake
          : s == "SCREAMING_SNAKE_CASE" ? case_style::screaming_snake
          : s == "kebab-case"           ? case_style::kebab
          : s == "SCREAMING-KEBAB-CASE" ? case_style::screaming_kebab
          : throw "sardine: unknown rename_all style") {}
};

// ---------------------------------------------------------------------------
// Errors.
// ---------------------------------------------------------------------------

struct error {
  std::string message;
  std::size_t offset = 0;  // byte offset into the input JSON
};

// ---------------------------------------------------------------------------
// Reflection helpers (all consteval).
// ---------------------------------------------------------------------------

namespace detail {

template <typename T>
consteval auto members_of() {
  return std::define_static_array(std::meta::nonstatic_data_members_of(
      ^^T, std::meta::access_context::unchecked()));
}

template <typename E>
consteval auto enumerators_of() {
  return std::define_static_array(std::meta::enumerators_of(^^E));
}

template <std::size_t N>
consteval std::array<std::size_t, N> indices() {
  std::array<std::size_t, N> a{};
  for (std::size_t i = 0; i < N; ++i) a[i] = i;
  return a;
}

template <typename A>
consteval std::optional<A> annotation_of(std::meta::info item) {
  auto found = std::meta::annotations_of_with_type(item, ^^A);
  if (found.empty()) return std::nullopt;
  return std::meta::extract<A>(found.back());  // last one wins
}

template <typename A>
consteval bool has(std::meta::info item) {
  return annotation_of<A>(item).has_value();
}

consteval std::string convert_case(std::string_view id, case_style style) {
  std::string out;
  bool up_next = (style == case_style::pascal);
  bool upper_all =
      style == case_style::screaming_snake || style == case_style::screaming_kebab;
  char sep = (style == case_style::kebab || style == case_style::screaming_kebab) ? '-'
           : (style == case_style::snake || style == case_style::screaming_snake) ? '_'
           : '\0';
  auto to_up = [](char c) { return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c; };
  auto to_lo = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
  for (char c : id) {
    if (c == '_') {
      if (sep) out += sep;
      else up_next = true;
      continue;
    }
    if (upper_all) out += to_up(c);
    else if (up_next) { out += to_up(c); up_next = false; }
    else out += to_lo(c);
  }
  return out;
}

// The JSON key for a member (or enumerator) `item` of type `Owner`:
// explicit rename beats owner-level rename_all beats the raw identifier.
template <typename Owner>
consteval std::string_view json_name(std::meta::info item) {
  if (auto r = annotation_of<rename>(item))
    return std::string_view(std::define_static_string(r->str()));
  std::string_view id = std::meta::identifier_of(item);
  if (auto all = annotation_of<rename_all>(^^Owner))
    return std::string_view(std::define_static_string(convert_case(id, all->style)));
  return id;
}

consteval bool skip_ser(std::meta::info m) {
  return has<skip>(m) || has<skip_serializing>(m);
}
consteval bool skip_de(std::meta::info m) {
  return has<skip>(m) || has<skip_deserializing>(m);
}

template <typename T>
consteval std::string_view type_name() {
  if (std::meta::has_identifier(^^T)) return std::meta::identifier_of(^^T);
  return std::string_view(std::define_static_string(std::meta::display_string_of(^^T)));
}

// ---------------------------------------------------------------------------
// Type classification.
// ---------------------------------------------------------------------------

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {};

template <typename T> struct is_variant : std::false_type {};
template <typename... A> struct is_variant<std::variant<A...>> : std::true_type {};

template <typename T>
concept string_like = std::convertible_to<const T&, std::string_view>;

template <typename T>
concept map_key_type =
    string_like<T> || (std::integral<T> && !std::same_as<T, bool>);

template <typename T>
concept map_like = std::ranges::input_range<T> && requires {
  typename T::key_type;
  typename T::mapped_type;
} && map_key_type<typename T::key_type>;

template <typename T>
concept sequence_like =
    std::ranges::input_range<T> && !string_like<T> && !map_like<T>;

template <typename T>
concept reflectable_struct = std::is_class_v<T> && !string_like<T> &&
    !std::ranges::input_range<T> && !is_optional<T>::value &&
    !is_variant<T>::value && !std::same_as<T, std::monostate>;

template <typename V, std::size_t I>
consteval std::string_view alt_name() {
  return type_name<std::variant_alternative_t<I, V>>();
}

// ---------------------------------------------------------------------------
// Shared text helpers.
// ---------------------------------------------------------------------------

inline void escape_into(std::string& out, std::string_view s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 0xf];
        } else {
          out += char(c);  // UTF-8 passes through verbatim
        }
    }
  }
  out += '"';
}

template <typename T>
void number_into(std::string& out, T v) {
  if constexpr (std::floating_point<T>) {
    if (v != v || v == T(1) / T(0) || v == T(-1) / T(0)) {  // nan/inf: not JSON
      out += "null";
      return;
    }
  }
  char buf[64];
  auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, end);
}

// ---------------------------------------------------------------------------
// JSON serialization.
// ---------------------------------------------------------------------------

struct json_writer {
  std::string out;
  int indent = 0;  // 0 = compact
  int level = 0;

  bool pretty() const { return indent > 0; }
  void nl() {
    if (pretty()) {
      out += '\n';
      out.append(std::size_t(level) * std::size_t(indent), ' ');
    }
  }
  void begin(char c) { out += c; ++level; }
  void end(char c, bool empty) {
    --level;
    if (!empty) nl();
    out += c;
  }
  void comma(bool& first) {
    if (!std::exchange(first, false)) out += ',';
    nl();
  }
  void colon() {
    out += ':';
    if (pretty()) out += ' ';
  }

  template <typename K>
  void key(const K& k) {
    if constexpr (string_like<K>) escape_into(out, std::string_view(k));
    else {  // integer map key: JSON object keys must be strings
      out += '"';
      number_into(out, k);
      out += '"';
    }
    colon();
  }

  template <typename T>
  void write(const T& v) {
    if constexpr (std::same_as<T, bool>) {
      out += v ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
      template for (constexpr auto e : enumerators_of<T>()) {
        if (v == [:e:]) {
          escape_into(out, json_name<T>(e));
          return;
        }
      }
      number_into(out, std::to_underlying(v));  // value outside named enumerators
    } else if constexpr (std::integral<T> || std::floating_point<T>) {
      number_into(out, v);
    } else if constexpr (string_like<T>) {
      escape_into(out, std::string_view(v));
    } else if constexpr (is_optional<T>::value) {
      if (v) write(*v);
      else out += "null";
    } else if constexpr (std::same_as<T, std::monostate>) {
      out += "null";
    } else if constexpr (is_variant<T>::value) {
      variant_external(v);
    } else if constexpr (map_like<T>) {
      begin('{');
      bool first = true;
      for (const auto& [k, mv] : v) {
        comma(first);
        key(k);
        write(mv);
      }
      end('}', first);
    } else if constexpr (sequence_like<T>) {
      begin('[');
      bool first = true;
      for (const auto& e : v) {
        comma(first);
        write(e);
      }
      end(']', first);
    } else if constexpr (reflectable_struct<T>) {
      begin('{');
      bool first = true;
      struct_body(v, first);
      end('}', first);
    } else {
      static_assert(false, "sardine: type is not serializable");
    }
  }

  // Emits the fields of `v` without the surrounding braces (used by flatten
  // and internally tagged variants).
  template <typename T>
  void struct_body(const T& v, bool& first) {
    template for (constexpr auto m : members_of<T>()) {
      if constexpr (!skip_ser(m)) {
        using M = [:std::meta::type_of(m):];
        if constexpr (has<flatten>(m) && reflectable_struct<M>) {
          struct_body(v.[:m:], first);
        } else if constexpr (has<flatten>(m) && map_like<M>) {
          for (const auto& [k, mv] : v.[:m:]) {
            comma(first);
            key(k);
            write(mv);
          }
        } else {
          comma(first);
          escape_into(out, json_name<T>(m));
          colon();
          write_member<m>(v.[:m:]);
        }
      }
    }
  }

  // Field write with member-annotation-driven variant modes.
  template <std::meta::info M, typename F>
  void write_member(const F& field) {
    if constexpr (is_variant<F>::value) {
      if constexpr (has<untagged>(M)) {
        variant_untagged(field);
      } else if constexpr (annotation_of<tag>(M).has_value()) {
        constexpr auto tg = annotation_of<tag>(M);
        constexpr std::string_view tn{std::define_static_string(tg->str())};
        variant_internal(field, tn);
      } else {
        variant_external(field);
      }
    } else {
      write(field);
    }
  }

  // {"AltName": value} — unit (monostate) alternatives collapse to "AltName".
  template <typename V>
  void variant_external(const V& v) {
    template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
      if (v.index() == I) {
        using A = std::variant_alternative_t<I, V>;
        if constexpr (std::same_as<A, std::monostate>) {
          escape_into(out, alt_name<V, I>());
        } else {
          begin('{');
          bool first = true;
          comma(first);
          escape_into(out, alt_name<V, I>());
          colon();
          write(std::get<I>(v));
          end('}', false);
        }
        return;
      }
    }
    out += "null";  // valueless_by_exception
  }

  template <typename V>
  void variant_untagged(const V& v) {
    template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
      if (v.index() == I) {
        write(std::get<I>(v));
        return;
      }
    }
    out += "null";
  }

  // {"<tag>":"AltName", ...alt fields} — alternatives must all be structs.
  template <typename V>
  void variant_internal(const V& v, std::string_view tag_key) {
    template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
      if (v.index() == I) {
        using A = std::variant_alternative_t<I, V>;
        static_assert(reflectable_struct<A>,
                      "sardine::tag requires all variant alternatives to be structs");
        begin('{');
        bool first = true;
        comma(first);
        escape_into(out, tag_key);
        colon();
        escape_into(out, alt_name<V, I>());
        struct_body(std::get<I>(v), first);
        end('}', false);
        return;
      }
    }
    out += "null";
  }
};

// ---------------------------------------------------------------------------
// Deserialization: recursive-descent JSON parser writing through splices.
// ---------------------------------------------------------------------------

struct parse_error {
  const char* message;
  std::size_t offset;
};

struct parser {
  std::string_view in;
  std::size_t pos = 0;
  int depth = 0;
  static constexpr int max_depth = 256;

  [[noreturn]] void fail(const char* msg) const { throw parse_error{msg, pos}; }

  void skip_ws() {
    while (pos < in.size() &&
           (in[pos] == ' ' || in[pos] == '\t' || in[pos] == '\n' || in[pos] == '\r'))
      ++pos;
  }
  char peek() {
    skip_ws();
    if (pos >= in.size()) fail("unexpected end of input");
    return in[pos];
  }
  bool consume(char c) {
    skip_ws();
    if (pos < in.size() && in[pos] == c) { ++pos; return true; }
    return false;
  }
  void expect(char c) {
    if (!consume(c)) fail("unexpected character");
  }
  bool consume_word(std::string_view w) {
    skip_ws();
    if (in.substr(pos, w.size()) == w) { pos += w.size(); return true; }
    return false;
  }

  struct depth_guard {
    parser& p;
    explicit depth_guard(parser& p) : p(p) {
      if (++p.depth > max_depth) p.fail("nesting too deep");
    }
    ~depth_guard() { --p.depth; }
  };

  void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }

  std::uint32_t parse_hex4() {
    if (pos + 4 > in.size()) fail("truncated \\u escape");
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = in[pos++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= std::uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= std::uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= std::uint32_t(c - 'A' + 10);
      else fail("bad \\u escape");
    }
    return v;
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (pos >= in.size()) fail("unterminated string");
      char c = in[pos++];
      if (c == '"') return out;
      if (c == '\\') {
        if (pos >= in.size()) fail("unterminated escape");
        char e = in[pos++];
        switch (e) {
          case '"':  out += '"';  break;
          case '\\': out += '\\'; break;
          case '/':  out += '/';  break;
          case 'b':  out += '\b'; break;
          case 'f':  out += '\f'; break;
          case 'n':  out += '\n'; break;
          case 'r':  out += '\r'; break;
          case 't':  out += '\t'; break;
          case 'u': {
            std::uint32_t cp = parse_hex4();
            if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate
              if (pos + 1 < in.size() && in[pos] == '\\' && in[pos + 1] == 'u') {
                pos += 2;
                std::uint32_t lo = parse_hex4();
                if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid low surrogate");
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                fail("lone high surrogate");
              }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              fail("lone low surrogate");
            }
            append_utf8(out, cp);
            break;
          }
          default: fail("unknown escape");
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        fail("raw control character in string");
      } else {
        out += c;
      }
    }
  }

  std::string_view number_token() {
    skip_ws();
    std::size_t start = pos;
    if (pos < in.size() && in[pos] == '-') ++pos;
    while (pos < in.size() &&
           ((in[pos] >= '0' && in[pos] <= '9') || in[pos] == '.' ||
            in[pos] == 'e' || in[pos] == 'E' || in[pos] == '+' || in[pos] == '-'))
      ++pos;
    if (pos == start) fail("expected a number");
    return in.substr(start, pos - start);
  }

  // Consume any well-formed value without storing it (for unknown keys).
  void skip_value() {
    depth_guard g(*this);
    char c = peek();
    if (c == '"') { parse_string(); return; }
    if (c == '{') {
      ++pos;
      if (consume('}')) return;
      do { parse_string(); expect(':'); skip_value(); } while (consume(','));
      expect('}');
      return;
    }
    if (c == '[') {
      ++pos;
      if (consume(']')) return;
      do skip_value(); while (consume(','));
      expect(']');
      return;
    }
    if (consume_word("true") || consume_word("false") || consume_word("null")) return;
    number_token();
  }
};

template <typename T>
void read_value(parser& p, T& out);

template <typename T>
void read_number(parser& p, T& out) {
  std::string_view tok = p.number_token();
  auto [end, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), out);
  if (ec != std::errc{} || end != tok.data() + tok.size())
    p.fail(std::floating_point<T> ? "invalid number" : "expected an integer");
}

// --- variant readers --------------------------------------------------------

template <typename V>
void read_variant_external(parser& p, V& out) {
  if (p.peek() == '"') {  // unit form: "AltName" selects a default-constructed alt
    std::string name = p.parse_string();
    bool matched = false;
    template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
      if (!matched && name == alt_name<V, I>()) {
        matched = true;
        out.template emplace<I>();
      }
    }
    if (!matched) p.fail("unknown variant name");
    return;
  }
  parser::depth_guard g(p);
  p.expect('{');
  std::string name = p.parse_string();
  p.expect(':');
  bool matched = false;
  template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
    if (!matched && name == alt_name<V, I>()) {
      matched = true;
      std::variant_alternative_t<I, V> tmp{};
      read_value(p, tmp);
      out.template emplace<I>(std::move(tmp));
    }
  }
  if (!matched) p.fail("unknown variant name");
  p.expect('}');
}

template <typename V>
void read_variant_untagged(parser& p, V& out) {
  std::size_t save = p.pos;
  bool matched = false;
  template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
    if (!matched) {
      try {
        std::variant_alternative_t<I, V> tmp{};
        read_value(p, tmp);
        out.template emplace<I>(std::move(tmp));
        matched = true;
      } catch (const parse_error&) {
        p.pos = save;
      }
    }
  }
  if (!matched) p.fail("no variant alternative matched");
}

template <typename V>
void read_variant_internal(parser& p, V& out, std::string_view tag_key) {
  p.skip_ws();
  std::size_t start = p.pos;
  // First pass: scan the object for the tag key.
  std::string name;
  bool found = false;
  {
    parser::depth_guard g(p);
    p.expect('{');
    if (!p.consume('}')) {
      do {
        std::string k = p.parse_string();
        p.expect(':');
        if (!found && k == tag_key) {
          name = p.parse_string();
          found = true;
        } else {
          p.skip_value();
        }
      } while (p.consume(','));
      p.expect('}');
    }
  }
  if (!found) p.fail("missing variant tag");
  // Second pass: re-parse the object as the selected alternative; the tag
  // key is dropped as an unknown field.
  p.pos = start;
  bool matched = false;
  template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
    if (!matched && name == alt_name<V, I>()) {
      matched = true;
      std::variant_alternative_t<I, V> tmp{};
      static_assert(reflectable_struct<std::variant_alternative_t<I, V>>,
                    "sardine::tag requires all variant alternatives to be structs");
      read_value(p, tmp);
      out.template emplace<I>(std::move(tmp));
    }
  }
  if (!matched) p.fail("unknown variant tag value");
}

// Field read with member-annotation-driven variant modes.
template <std::meta::info M, typename F>
void read_member(parser& p, F& field) {
  if constexpr (is_variant<F>::value) {
    if constexpr (has<untagged>(M)) {
      read_variant_untagged(p, field);
    } else if constexpr (annotation_of<tag>(M).has_value()) {
      constexpr auto tg = annotation_of<tag>(M);
      constexpr std::string_view tn{std::define_static_string(tg->str())};
      read_variant_internal(p, field, tn);
    } else {
      read_variant_external(p, field);
    }
  } else {
    read_value(p, field);
  }
}

// --- struct reader ----------------------------------------------------------

// Route one "key": value pair into `out`. Direct fields first, then fields of
// flattened structs (recursively), then a flattened catch-all map if present.
// `seen` (when non-null) tracks which of this level's direct members matched.
template <typename T>
bool try_read_key(parser& p, T& out, std::string_view key, bool* seen) {
  constexpr auto mems = members_of<T>();
  bool handled = false;
  template for (constexpr auto I : indices<mems.size()>()) {
    constexpr auto m = mems[I];
    if constexpr (!skip_de(m)) {
      using M = [:std::meta::type_of(m):];
      if constexpr (has<flatten>(m) && reflectable_struct<M>) {
        if (!handled) handled = try_read_key(p, out.[:m:], key, nullptr);
      } else if constexpr (has<flatten>(m) && map_like<M>) {
        // catch-all: only after every real field had its chance
      } else {
        if (!handled && key == json_name<T>(m)) {
          handled = true;
          read_member<m>(p, out.[:m:]);
          if (seen) seen[I] = true;
        }
      }
    }
  }
  if (handled) return true;
  template for (constexpr auto m : members_of<T>()) {
    if constexpr (!skip_de(m)) {
      using M = [:std::meta::type_of(m):];
      if constexpr (has<flatten>(m) && map_like<M>) {
        if (!handled) {
          handled = true;
          auto& map = out.[:m:];
          read_value(p, map[typename M::key_type(std::string(key))]);
        }
      }
    }
  }
  return handled;
}

template <typename T>
void read_struct(parser& p, T& out) {
  constexpr auto mems = members_of<T>();
  std::array<bool, members_of<T>().size()> seen{};
  parser::depth_guard g(p);
  p.expect('{');
  if (!p.consume('}')) {
    do {
      std::string key = p.parse_string();
      p.expect(':');
      if (!try_read_key(p, out, key, seen.data())) {
        if constexpr (has<deny_unknown_fields>(^^T)) p.fail("unknown field");
        else p.skip_value();
      }
    } while (p.consume(','));
    p.expect('}');
  }
  template for (constexpr auto I : indices<mems.size()>()) {
    constexpr auto m = mems[I];
    if constexpr (has<required>(m)) {
      if (!seen[I]) {
        constexpr const char* msg = std::define_static_string(
            std::string("missing required field '") +
            std::string(json_name<T>(m)) + "'");
        p.fail(msg);
      }
    }
  }
}

template <typename T>
void read_value(parser& p, T& out) {
  if constexpr (std::same_as<T, bool>) {
    if (p.consume_word("true")) out = true;
    else if (p.consume_word("false")) out = false;
    else p.fail("expected true or false");
  } else if constexpr (std::is_enum_v<T>) {
    if (p.peek() == '"') {
      std::size_t at = p.pos;
      std::string name = p.parse_string();
      template for (constexpr auto e : enumerators_of<T>()) {
        if (name == json_name<T>(e)) {
          out = [:e:];
          return;
        }
      }
      p.pos = at;
      p.fail("unknown enumerator");
    } else {
      std::underlying_type_t<T> raw;
      read_number(p, raw);
      out = static_cast<T>(raw);
    }
  } else if constexpr (std::integral<T> || std::floating_point<T>) {
    read_number(p, out);
  } else if constexpr (std::same_as<T, std::string>) {
    out = p.parse_string();
  } else if constexpr (is_optional<T>::value) {
    if (p.consume_word("null")) {
      out.reset();
    } else {
      out.emplace();
      read_value(p, *out);
    }
  } else if constexpr (std::same_as<T, std::monostate>) {
    if (!p.consume_word("null")) p.fail("expected null");
  } else if constexpr (is_variant<T>::value) {
    read_variant_external(p, out);
  } else if constexpr (map_like<T>) {
    parser::depth_guard g(p);
    p.expect('{');
    out.clear();
    if (p.consume('}')) return;
    do {
      std::string key = p.parse_string();
      p.expect(':');
      using K = typename T::key_type;
      if constexpr (string_like<K>) {
        read_value(p, out[K(std::move(key))]);
      } else {  // integer key encoded as a JSON string
        K k{};
        auto [end, ec] = std::from_chars(key.data(), key.data() + key.size(), k);
        if (ec != std::errc{} || end != key.data() + key.size())
          p.fail("expected an integer map key");
        read_value(p, out[k]);
      }
    } while (p.consume(','));
    p.expect('}');
  } else if constexpr (sequence_like<T>) {
    parser::depth_guard g(p);
    p.expect('[');
    out.clear();
    if (p.consume(']')) return;
    do read_value(p, out.emplace_back()); while (p.consume(','));
    p.expect(']');
  } else if constexpr (reflectable_struct<T>) {
    read_struct(p, out);
  } else {
    static_assert(false, "sardine: type is not deserializable");
  }
}

// ---------------------------------------------------------------------------
// Debug formatting (Rust's {:?} / {:#?}).
//
// Ignores sardine annotations on purpose, exactly like Rust's derive(Debug):
// raw C++ identifiers, skipped fields included.
// ---------------------------------------------------------------------------

struct debug_writer {
  std::string out;
  bool pretty = false;
  int level = 0;
  static constexpr int iw = 4;  // Rust uses 4-space indents in {:#?}

  void nl() {
    if (pretty) {
      out += '\n';
      out.append(std::size_t(level) * iw, ' ');
    }
  }

  template <typename T>
  void write(const T& v) {
    if constexpr (std::same_as<T, bool>) {
      out += v ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
      template for (constexpr auto e : enumerators_of<T>()) {
        if (v == [:e:]) {
          out += std::meta::identifier_of(e);
          return;
        }
      }
      out += type_name<T>();
      out += '(';
      number_into(out, std::to_underlying(v));
      out += ')';
    } else if constexpr (std::integral<T>) {
      number_into(out, v);
    } else if constexpr (std::floating_point<T>) {
      std::size_t at = out.size();
      char buf[64];
      auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
      out.append(buf, end);
      // Rust prints floats with a decimal point: 3.0, not 3
      if (out.find_first_not_of("-0123456789", at) == std::string::npos)
        out += ".0";
    } else if constexpr (string_like<T>) {
      escape_into(out, std::string_view(v));
    } else if constexpr (is_optional<T>::value) {
      if (v) {
        out += "Some(";
        write(*v);
        out += ')';
      } else {
        out += "None";
      }
    } else if constexpr (std::same_as<T, std::monostate>) {
      out += "monostate";
    } else if constexpr (is_variant<T>::value) {
      bool matched = false;
      template for (constexpr auto I : indices<std::variant_size_v<T>>()) {
        if (!matched && v.index() == I) {
          matched = true;
          write(std::get<I>(v));
        }
      }
      if (!matched) out += "<valueless>";
    } else if constexpr (map_like<T>) {
      if (std::ranges::empty(v)) {
        out += "{}";
        return;
      }
      out += '{';
      ++level;
      bool first = true;
      for (const auto& [k, mv] : v) {
        if (pretty) nl();
        else out += std::exchange(first, false) ? "" : ", ";
        write(k);
        out += ": ";
        write(mv);
        if (pretty) out += ',';
      }
      --level;
      nl();
      out += '}';
    } else if constexpr (sequence_like<T>) {
      if (std::ranges::empty(v)) {
        out += "[]";
        return;
      }
      out += '[';
      ++level;
      bool first = true;
      for (const auto& e : v) {
        if (pretty) nl();
        else out += std::exchange(first, false) ? "" : ", ";
        write(e);
        if (pretty) out += ',';
      }
      --level;
      nl();
      out += ']';
    } else if constexpr (reflectable_struct<T>) {
      out += type_name<T>();
      constexpr auto mems = members_of<T>();
      if constexpr (mems.size() == 0) return;  // unit struct: just the name
      out += " {";
      ++level;
      bool first = true;
      template for (constexpr auto m : members_of<T>()) {
        if (pretty) nl();
        else out += std::exchange(first, false) ? " " : ", ";
        out += std::meta::identifier_of(m);
        out += ": ";
        write(v.[:m:]);
        if (pretty) out += ',';
      }
      --level;
      if (pretty) nl();
      else out += ' ';
      out += '}';
    } else {
      static_assert(false, "sardine: type is not debug-printable");
    }
  }
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

template <typename T>
std::string to_json(const T& value) {
  detail::json_writer w;
  w.write(value);
  return std::move(w.out);
}

template <typename T>
std::string to_json_pretty(const T& value, int indent = 2) {
  detail::json_writer w;
  w.indent = indent > 0 ? indent : 2;
  w.write(value);
  return std::move(w.out);
}

template <typename T>
std::expected<T, error> from_json(std::string_view json) {
  detail::parser p{json};
  T value{};
  try {
    detail::read_value(p, value);
    p.skip_ws();
    if (p.pos != json.size())
      return std::unexpected(error{"trailing characters after value", p.pos});
  } catch (const detail::parse_error& e) {
    return std::unexpected(error{e.message, e.offset});
  }
  return value;
}

// Rust's {:?}
template <typename T>
std::string debug(const T& value) {
  detail::debug_writer w;
  w.write(value);
  return std::move(w.out);
}

// Rust's {:#?}
template <typename T>
std::string debug_pretty(const T& value) {
  detail::debug_writer w;
  w.pretty = true;
  w.write(value);
  return std::move(w.out);
}

// std::format integration: "{}" == debug, "{:#}" == debug_pretty.
//   std::println("{:#}", sardine::dbg(value));
template <typename T>
struct dbg {
  const T& value;
};
template <typename T>
dbg(const T&) -> dbg<T>;

}  // namespace sardine

template <typename T>
struct std::formatter<sardine::dbg<T>, char> {
  bool alt = false;
  constexpr auto parse(std::format_parse_context& ctx) {
    auto it = ctx.begin();
    if (it != ctx.end() && *it == '#') {
      alt = true;
      ++it;
    }
    if (it != ctx.end() && *it != '}')
      throw std::format_error("sardine::dbg supports only '{}' and '{:#}'");
    return it;
  }
  template <typename FmtCtx>
  auto format(const sardine::dbg<T>& d, FmtCtx& ctx) const {
    std::string s = alt ? sardine::debug_pretty(d.value) : sardine::debug(d.value);
    return std::ranges::copy(s, ctx.out()).out;
  }
};
