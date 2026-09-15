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
//   std::vector<std::uint8_t> c = sardine::to_cbor(u);           // RFC 8949
//   std::expected<User, sardine::error> u3 = sardine::from_cbor<User>(c);
//
//   std::println("{}",  sardine::dbg(u));   // Rust {:?}  : User { id: 7, ... }
//   std::println("{:#}", sardine::dbg(u));  // Rust {:#?} : multi-line
//
// Requires: g++ >= 16.1 with -std=c++26 -freflection
#pragma once

#include <meta>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

// [[=sardine::emit_null{}]] — a disengaged std::optional member is OMITTED
// from struct output by default (absent key, not "k":null); this opts a field
// back into writing an explicit null. Reading is unaffected: absent keeps the
// default, explicit null resets. (Deviation from Serde, which emits null
// unless told otherwise — see NOTES.md.)
struct emit_null {};

// [[=sardine::deny_unknown_fields{}]] — on a struct: unknown keys are an
// error instead of being ignored.
struct deny_unknown_fields {};

// [[=sardine::enum_from_number{}]] — on an enum: reading accepts the numeric
// underlying value as well as enumerator names. Without it a number where an
// enum belongs is a type error: names are the wire format, and a stray
// integer silently becoming an enumerator is how bad states are smuggled in.
struct enum_from_number {};

// [[=sardine::untagged{}]] — on a std::variant member: no tag, alternatives
// are tried in order until one parses.
struct untagged {};

// [[=sardine::tag("type")]] — on a std::variant member whose alternatives are
// all structs: internally tagged, {"type":"AltName", ...fields}.
struct tag : detail::fixed_string {
  using fixed_string::fixed_string;
};

// [[=sardine::int_key(3)]] — the field's map key is this integer (negative
// allowed). CBOR writes it natively (COSE/CTAP-shaped maps: RFC 9052 labels);
// JSON, whose object keys must be strings, spells it in decimal — the same
// convention integer-keyed maps already use.
struct int_key {
  std::int64_t label;
  consteval int_key(std::int64_t l) : label(l) {}
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

// What went wrong, as a code a caller can branch on. The message is for
// humans; the code is the contract.
enum class errc : std::uint8_t {
  syntax,            // malformed input: unexpected character or structure
  truncated,         // input ended inside a value
  invalid_escape,    // bad \-sequence or \u pair
  invalid_number,    // number literal that parses as nothing
  depth_exceeded,    // nesting deeper than max_depth
  trailing,          // input continues after the top-level value
  type_mismatch,     // well-formed value of the wrong type
  unknown_field,     // object key with no matching member (deny_unknown_fields)
  missing_field,     // required{} member absent
  unknown_enum,      // string is not an enumerator name
  unknown_variant,   // no variant alternative matched / unknown tag
  out_of_range,      // number does not fit the destination type
  invalid_encoding,  // CBOR: reserved bits, bad chunks, or a strict-profile refusal
};

struct error {
  std::string message;
  std::size_t offset = 0;  // byte offset into the input (JSON text or CBOR)
  errc code = errc::syntax;
  // Dotted location of the failure inside the document ("profile.valid_secs",
  // "items.0.name"); empty at the top level.
  std::string path;
};

// ---------------------------------------------------------------------------
// CBOR decode profiles.
//
// The default decoder is liberal: it accepts every encoding of a meaning.
// A wire whose messages are SIGNED needs the opposite — exactly one encoding
// per meaning, everything else refused — because two byte sequences that
// decode alike are two messages with one signature. The flags are separate
// because real consumers sit between the extremes (WebAuthn wants definite
// lengths and no tags, but authenticators do emit non-minimal heads).
// ---------------------------------------------------------------------------

struct cbor_options {
  bool minimal_heads    = false;  // reject non-minimal argument encodings
  bool definite_only    = false;  // reject indefinite-length strings/arrays/maps
  bool no_tags          = false;  // reject semantic tags instead of skipping them
  bool no_substitutions = false;  // reject half-floats, ints where floats are
                                  // expected, text-encoded integer map keys,
                                  // and undefined-as-null
};

// RFC 8949 §4.2-shaped strictness (heads and lengths; key ORDER stays the
// writer's, deliberately — sardine encodes declaration/container order).
inline constexpr cbor_options cbor_strict{true, true, true, true};

// Exactly one CBOR data item, kept as its verbatim bytes. Reading validates
// well-formedness (under the active cbor_options) without interpreting;
// writing splices the bytes back. For fields whose shape is not yours to
// model — WebAuthn's attStmt, protocol extensions carried through unread.
// CBOR-only: the JSON pair refuses the type at compile time. An empty
// cbor_raw writes null (a map entry cannot simply vanish).
struct cbor_raw {
  std::vector<std::uint8_t> bytes;
};

// ---------------------------------------------------------------------------
// sardine::value — the generic JSON document (serde_json::Value's role).
//
// Typed structs are the front door; this is the escape hatch for documents
// whose shape is the DATA's, not the program's: configuration written by
// another tool, protocol payloads passed through unread, walkers over
// user-authored trees. It works everywhere a struct member does, and
// from_json<value> parses any document.
//
// Semantics chosen for document fidelity rather than strictness:
//   - objects preserve insertion order AND duplicate keys; find() returns the
//     first match (a document is evidence — deduplicating it would forge it)
//   - a number is int64 if it parses as one, double otherwise (so an integer
//     too large for int64 degrades to double instead of failing)
// ---------------------------------------------------------------------------

class value {
 public:
  using array = std::vector<value>;
  using object = std::vector<std::pair<std::string, value>>;

  value() : v_(nullptr) {}
  value(std::nullptr_t) : v_(nullptr) {}
  value(bool b) : v_(b) {}
  value(std::int64_t n) : v_(n) {}
  value(int n) : v_(std::int64_t(n)) {}
  value(double d) : v_(d) {}
  value(std::string s) : v_(std::move(s)) {}
  value(std::string_view s) : v_(std::string(s)) {}
  value(const char* s) : v_(std::string(s)) {}
  value(array a) : v_(std::move(a)) {}
  value(object o) : v_(std::move(o)) {}

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(v_); }
  bool is_bool() const { return std::holds_alternative<bool>(v_); }
  bool is_int() const { return std::holds_alternative<std::int64_t>(v_); }
  bool is_double() const { return std::holds_alternative<double>(v_); }
  bool is_number() const { return is_int() || is_double(); }
  bool is_string() const { return std::holds_alternative<std::string>(v_); }
  bool is_array() const { return std::holds_alternative<array>(v_); }
  bool is_object() const { return std::holds_alternative<object>(v_); }

  bool as_bool() const { return std::get<bool>(v_); }
  std::int64_t as_int() const { return std::get<std::int64_t>(v_); }
  double as_double() const {
    return is_int() ? double(as_int()) : std::get<double>(v_);
  }
  const std::string& as_string() const { return std::get<std::string>(v_); }
  const array& as_array() const { return std::get<array>(v_); }
  const object& as_object() const { return std::get<object>(v_); }
  array& as_array() { return std::get<array>(v_); }
  object& as_object() { return std::get<object>(v_); }

  // Object lookup; nullptr when absent (or not an object). First match wins.
  const value* find(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& [k, v] : as_object())
      if (k == key) return &v;
    return nullptr;
  }

  bool operator==(const value&) const = default;

 private:
  std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, array,
               object>
      v_;
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

consteval std::string int_string(std::int64_t v) {
  if (v == 0) return "0";
  bool neg = v < 0;
  unsigned long long u = neg ? 0ull - static_cast<unsigned long long>(v)
                             : static_cast<unsigned long long>(v);
  std::string s;
  while (u) {
    s.insert(s.begin(), char('0' + u % 10));
    u /= 10;
  }
  if (neg) s.insert(s.begin(), '-');
  return s;
}

consteval std::optional<std::int64_t> int_label(std::meta::info m) {
  if (auto k = annotation_of<int_key>(m)) return k->label;
  return std::nullopt;
}

// The textual key of a member: an int_key spells its label in decimal,
// everything else goes through rename / rename_all / the identifier. This is
// what JSON writes and what both readers match text keys against; the CBOR
// pair additionally writes/matches int_key labels natively.
template <typename Owner>
consteval std::string_view member_key(std::meta::info m) {
  if (auto k = annotation_of<int_key>(m))
    return std::string_view(std::define_static_string(int_string(k->label)));
  return json_name<Owner>(m);
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

// A sequence of exactly uint8_t is bytes, and CBOR has a type for bytes:
// major 2. (JSON does not, so the JSON pair keeps writing an array of
// numbers.) C++ can dispatch on the element type where Rust's serde could
// not — this is serde_bytes, without the wrapper.
template <typename T>
concept byte_sequence = sequence_like<T> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<T>>, std::uint8_t>;

template <typename T>
concept resizable_bytes = byte_sequence<T> && requires(T t) {
  t.clear();
  t.push_back(std::uint8_t{});
};

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
    } else if constexpr (std::same_as<T, cbor_raw>) {
      static_assert(false, "sardine: cbor_raw is CBOR-only");
    } else if constexpr (std::same_as<T, value>) {
      if (v.is_null()) out += "null";
      else if (v.is_bool()) write(v.as_bool());
      else if (v.is_int()) number_into(out, v.as_int());
      else if (v.is_double()) number_into(out, v.as_double());
      else if (v.is_string()) escape_into(out, v.as_string());
      else if (v.is_array()) {
        begin('[');
        bool first = true;
        for (const auto& e : v.as_array()) {
          comma(first);
          write(e);
        }
        end(']', first);
      } else {
        begin('{');
        bool first = true;
        for (const auto& [k, mv] : v.as_object()) {
          comma(first);
          key(k);
          write(mv);
        }
        end('}', first);
      }
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
          bool present = true;
          if constexpr (is_optional<M>::value && !has<emit_null>(m))
            present = v.[:m:].has_value();
          if (present) {
            comma(first);
            escape_into(out, member_key<T>(m));
            colon();
            write_member<m>(v.[:m:]);
          }
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
  errc code;
  // Joined at throw time: unwinding pops the reader's path stack before any
  // catch could read it, so the exception carries its own copy.
  std::string path;
};

// Both readers keep a stack of where they are in the document (object keys,
// array indices); errors report it dotted. The guard pops on scope exit —
// including during unwinding, which is why fail() joins first.
inline std::string joined_path(const std::vector<std::string>& segs) {
  std::string out;
  for (const auto& s : segs) {
    if (!out.empty()) out += '.';
    out += s;
  }
  return out;
}

template <typename P>
struct path_guard {
  P& p;
  path_guard(P& p, std::string seg) : p(p) { p.path.push_back(std::move(seg)); }
  ~path_guard() { p.path.pop_back(); }
};

struct parser {
  std::string_view in{};
  std::size_t pos = 0;
  int depth = 0;
  std::vector<std::string> path{};
  static constexpr int max_depth = 256;

  [[noreturn]] void fail(const char* msg, errc code = errc::syntax) const {
    throw parse_error{msg, pos, code, joined_path(path)};
  }

  void skip_ws() {
    while (pos < in.size() &&
           (in[pos] == ' ' || in[pos] == '\t' || in[pos] == '\n' || in[pos] == '\r'))
      ++pos;
  }
  char peek() {
    skip_ws();
    if (pos >= in.size()) fail("unexpected end of input", errc::truncated);
    return in[pos];
  }
  bool consume(char c) {
    skip_ws();
    if (pos < in.size() && in[pos] == c) { ++pos; return true; }
    return false;
  }
  void expect(char c) {
    if (!consume(c)) {
      if (pos >= in.size()) fail("unexpected end of input", errc::truncated);
      fail("unexpected character");
    }
  }
  bool consume_word(std::string_view w) {
    skip_ws();
    if (in.substr(pos, w.size()) == w) { pos += w.size(); return true; }
    return false;
  }

  struct depth_guard {
    parser& p;
    explicit depth_guard(parser& p) : p(p) {
      if (++p.depth > max_depth) p.fail("nesting too deep", errc::depth_exceeded);
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
    if (pos + 4 > in.size()) fail("truncated \\u escape", errc::invalid_escape);
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = in[pos++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= std::uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= std::uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= std::uint32_t(c - 'A' + 10);
      else fail("bad \\u escape", errc::invalid_escape);
    }
    return v;
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (pos >= in.size()) fail("unterminated string", errc::truncated);
      char c = in[pos++];
      if (c == '"') return out;
      if (c == '\\') {
        if (pos >= in.size()) fail("unterminated escape", errc::truncated);
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
                if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid low surrogate", errc::invalid_escape);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                fail("lone high surrogate", errc::invalid_escape);
              }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              fail("lone low surrogate", errc::invalid_escape);
            }
            append_utf8(out, cp);
            break;
          }
          default: fail("unknown escape", errc::invalid_escape);
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
    if (pos == start) {
      if (pos >= in.size()) fail("expected a number", errc::truncated);
      // A value of another type sitting where the number belongs is a type
      // error; anything else is malformed input.
      char c = in[pos];
      if (c == '"' || c == '{' || c == '[' || c == 't' || c == 'f' || c == 'n')
        fail("expected a number", errc::type_mismatch);
      fail("expected a number");
    }
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

// Parse ANY JSON document into a sardine::value tree. Duplicate keys and
// insertion order survive; oversized integers degrade to double.
inline void read_document(parser& p, value& out) {
  char c = p.peek();
  if (c == '"') {
    out = value(p.parse_string());
    return;
  }
  if (c == '{') {
    parser::depth_guard g(p);
    ++p.pos;
    value::object o;
    if (!p.consume('}')) {
      do {
        std::string k = p.parse_string();
        p.expect(':');
        path_guard where(p, k);
        value v;
        read_document(p, v);
        o.emplace_back(std::move(k), std::move(v));
      } while (p.consume(','));
      p.expect('}');
    }
    out = value(std::move(o));
    return;
  }
  if (c == '[') {
    parser::depth_guard g(p);
    ++p.pos;
    value::array a;
    if (!p.consume(']')) {
      std::size_t index = 0;
      do {
        path_guard where(p, std::to_string(index++));
        read_document(p, a.emplace_back());
      } while (p.consume(','));
      p.expect(']');
    }
    out = value(std::move(a));
    return;
  }
  if (p.consume_word("null")) { out = value(nullptr); return; }
  if (p.consume_word("true")) { out = value(true); return; }
  if (p.consume_word("false")) { out = value(false); return; }
  std::string_view tok = p.number_token();
  std::int64_t i;
  auto [iend, iec] = std::from_chars(tok.data(), tok.data() + tok.size(), i);
  if (iec == std::errc{} && iend == tok.data() + tok.size()) {
    out = value(i);
    return;
  }
  double d;
  auto [dend, dec] = std::from_chars(tok.data(), tok.data() + tok.size(), d);
  if (dec == std::errc{} && dend == tok.data() + tok.size()) {
    out = value(d);
    return;
  }
  p.fail("invalid number", errc::invalid_number);
}

template <typename T>
void read_number(parser& p, T& out) {
  std::string_view tok = p.number_token();
  auto [end, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), out);
  if (ec == std::errc{} && end == tok.data() + tok.size()) return;
  if (ec == std::errc::result_out_of_range)
    p.fail("number out of range", errc::out_of_range);
  if constexpr (std::floating_point<T>) {
    p.fail("invalid number", errc::invalid_number);
  } else {
    // A token that is a well-formed number but not an integer (1.5, 1e3) is
    // the wrong TYPE; a token that is no number at all is malformed input.
    double d;
    auto [dend, dec] = std::from_chars(tok.data(), tok.data() + tok.size(), d);
    if (dec == std::errc{} && dend == tok.data() + tok.size())
      p.fail("expected an integer", errc::type_mismatch);
    p.fail("invalid number", errc::invalid_number);
  }
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
    if (!matched) p.fail("unknown variant name", errc::unknown_variant);
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
  if (!matched) p.fail("unknown variant name", errc::unknown_variant);
  p.expect('}');
}

// Shared between the JSON and CBOR readers: both expose pos/fail and throw
// parse_error, which is all the retry loop needs.
template <typename P, typename V>
void read_variant_untagged(P& p, V& out) {
  std::size_t save = p.pos;
  std::size_t path_save = p.path.size();
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
        p.path.resize(path_save);
      }
    }
  }
  if (!matched) p.fail("no variant alternative matched", errc::unknown_variant);
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
  if (!found) p.fail("missing variant tag", errc::missing_field);
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
  if (!matched) p.fail("unknown variant tag value", errc::unknown_variant);
}

// Field read with member-annotation-driven variant modes. Templated on the
// reader so the JSON and CBOR parsers share it; the variant readers resolve
// to the overload matching `p`.
template <std::meta::info M, typename P, typename F>
void read_member(P& p, F& field) {
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
// Format-independent: works for any reader `p` that read_member accepts.
template <typename P, typename T>
bool try_read_key(P& p, T& out, std::string_view key, bool* seen) {
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
        if (!handled && key == member_key<T>(m)) {
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

// Route a native CBOR integer key into `out`: only int_key members (their own
// and, through flattened structs, their children's) can match it.
template <typename P, typename T>
bool try_read_int_key(P& p, T& out, std::int64_t label, bool* seen) {
  constexpr auto mems = members_of<T>();
  bool handled = false;
  template for (constexpr auto I : indices<mems.size()>()) {
    constexpr auto m = mems[I];
    if constexpr (!skip_de(m)) {
      using M = [:std::meta::type_of(m):];
      if constexpr (has<flatten>(m) && reflectable_struct<M>) {
        if (!handled) handled = try_read_int_key(p, out.[:m:], label, nullptr);
      } else if constexpr (constexpr auto want = int_label(m); want.has_value()) {
        if (!handled && label == *want) {
          handled = true;
          read_member<m>(p, out.[:m:]);
          if (seen) seen[I] = true;
        }
      }
    }
  }
  return handled;
}

// Fail if any required member of T went unseen. Shared by both readers.
template <typename T, typename P>
void check_required(P& p, const bool* seen) {
  constexpr auto mems = members_of<T>();
  template for (constexpr auto I : indices<mems.size()>()) {
    constexpr auto m = mems[I];
    if constexpr (has<required>(m)) {
      if (!seen[I]) {
        constexpr const char* msg = std::define_static_string(
            std::string("missing required field '") +
            std::string(member_key<T>(m)) + "'");
        // The missing field is part of the failure's location.
        p.path.push_back(std::string(member_key<T>(m)));
        p.fail(msg, errc::missing_field);
      }
    }
  }
}

template <typename T>
void read_struct(parser& p, T& out) {
  std::array<bool, members_of<T>().size()> seen{};
  parser::depth_guard g(p);
  p.expect('{');
  if (!p.consume('}')) {
    do {
      std::string key = p.parse_string();
      p.expect(':');
      path_guard where(p, key);
      if (!try_read_key(p, out, key, seen.data())) {
        if constexpr (has<deny_unknown_fields>(^^T)) p.fail("unknown field", errc::unknown_field);
        else p.skip_value();
      }
    } while (p.consume(','));
    p.expect('}');
  }
  check_required<T>(p, seen.data());
}

template <typename T>
void read_value(parser& p, T& out) {
  if constexpr (std::same_as<T, bool>) {
    if (p.consume_word("true")) out = true;
    else if (p.consume_word("false")) out = false;
    else p.fail("expected true or false", errc::type_mismatch);
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
      p.fail("unknown enumerator", errc::unknown_enum);
    } else if constexpr (has<enum_from_number>(^^T)) {
      std::underlying_type_t<T> raw;
      read_number(p, raw);
      out = static_cast<T>(raw);
    } else {
      p.fail("expected an enumerator name", errc::type_mismatch);
    }
  } else if constexpr (std::integral<T> || std::floating_point<T>) {
    read_number(p, out);
  } else if constexpr (std::same_as<T, std::string>) {
    if (p.peek() != '"') p.fail("expected a string", errc::type_mismatch);
    out = p.parse_string();
  } else if constexpr (std::same_as<T, cbor_raw>) {
    static_assert(false, "sardine: cbor_raw is CBOR-only");
  } else if constexpr (std::same_as<T, value>) {
    read_document(p, out);
  } else if constexpr (is_optional<T>::value) {
    if (p.consume_word("null")) {
      out.reset();
    } else {
      out.emplace();
      read_value(p, *out);
    }
  } else if constexpr (std::same_as<T, std::monostate>) {
    if (!p.consume_word("null")) p.fail("expected null", errc::type_mismatch);
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
      path_guard where(p, key);
      using K = typename T::key_type;
      if constexpr (string_like<K>) {
        read_value(p, out[K(std::move(key))]);
      } else {  // integer key encoded as a JSON string
        K k{};
        auto [end, ec] = std::from_chars(key.data(), key.data() + key.size(), k);
        if (ec != std::errc{} || end != key.data() + key.size())
          p.fail("expected an integer map key", errc::type_mismatch);
        read_value(p, out[k]);
      }
    } while (p.consume(','));
    p.expect('}');
  } else if constexpr (sequence_like<T>) {
    parser::depth_guard g(p);
    p.expect('[');
    out.clear();
    if (p.consume(']')) return;
    std::size_t index = 0;
    do {
      path_guard where(p, std::to_string(index++));
      read_value(p, out.emplace_back());
    } while (p.consume(','));
    p.expect(']');
  } else if constexpr (reflectable_struct<T>) {
    read_struct(p, out);
  } else {
    static_assert(false, "sardine: type is not deserializable");
  }
}

// ---------------------------------------------------------------------------
// CBOR serialization (RFC 8949). Same data model and annotations as the JSON
// pair; the differences are the ones the format invites: integer map keys
// stay integers, nan/inf encode natively, and output is definite-length
// throughout (except unsized input ranges, which use indefinite arrays).
// ---------------------------------------------------------------------------

struct cbor_writer {
  std::vector<std::uint8_t> out;

  void byte(std::uint8_t b) { out.push_back(b); }
  void be(std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) byte(std::uint8_t(v >> (8 * i)));
  }
  // Major type + minimal-length argument encoding.
  void head(std::uint8_t major, std::uint64_t v) {
    std::uint8_t m = std::uint8_t(major << 5);
    if (v < 24) byte(m | std::uint8_t(v));
    else if (v <= 0xff) { byte(m | 24); be(v, 1); }
    else if (v <= 0xffff) { byte(m | 25); be(v, 2); }
    else if (v <= 0xffffffff) { byte(m | 26); be(v, 4); }
    else { byte(m | 27); be(v, 8); }
  }
  void text(std::string_view s) {
    head(3, s.size());
    out.insert(out.end(), s.begin(), s.end());
  }
  template <typename T>
  void integer(T v) {
    auto u = static_cast<std::uint64_t>(v);
    if constexpr (std::signed_integral<T>) {
      if (v < 0) { head(1, ~u); return; }  // ~u == -1 - v in unsigned math
    }
    head(0, u);
  }
  void null() { byte(0xf6); }

  template <typename K>
  void key(const K& k) {
    if constexpr (string_like<K>) text(std::string_view(k));
    else integer(k);  // CBOR map keys need not be strings
  }

  template <typename T>
  void write(const T& v) {
    if constexpr (std::same_as<T, bool>) {
      byte(v ? 0xf5 : 0xf4);
    } else if constexpr (std::is_enum_v<T>) {
      template for (constexpr auto e : enumerators_of<T>()) {
        if (v == [:e:]) {
          text(json_name<T>(e));
          return;
        }
      }
      integer(std::to_underlying(v));  // value outside named enumerators
    } else if constexpr (std::integral<T>) {
      integer(v);
    } else if constexpr (std::same_as<T, float>) {
      byte(0xfa);
      be(std::bit_cast<std::uint32_t>(v), 4);
    } else if constexpr (std::floating_point<T>) {
      byte(0xfb);
      be(std::bit_cast<std::uint64_t>(double(v)), 8);
    } else if constexpr (string_like<T>) {
      text(std::string_view(v));
    } else if constexpr (std::same_as<T, cbor_raw>) {
      if (v.bytes.empty()) null();
      else out.insert(out.end(), v.bytes.begin(), v.bytes.end());
    } else if constexpr (std::same_as<T, value>) {
      static_assert(false, "sardine: value is JSON-only (for now)");
    } else if constexpr (is_optional<T>::value) {
      if (v) write(*v);
      else null();
    } else if constexpr (std::same_as<T, std::monostate>) {
      null();
    } else if constexpr (is_variant<T>::value) {
      variant_external(v);
    } else if constexpr (map_like<T>) {
      head(5, std::uint64_t(std::ranges::distance(v)));
      for (const auto& [k, mv] : v) {
        key(k);
        write(mv);
      }
    } else if constexpr (byte_sequence<T> && std::ranges::forward_range<T>) {
      head(2, std::uint64_t(std::ranges::distance(v)));
      if constexpr (std::ranges::contiguous_range<T>) {
        auto d = std::ranges::data(v);
        out.insert(out.end(), d, d + std::ranges::size(v));
      } else {
        for (std::uint8_t b : v) byte(b);
      }
    } else if constexpr (sequence_like<T>) {
      if constexpr (std::ranges::forward_range<T>) {
        head(4, std::uint64_t(std::ranges::distance(v)));
        for (const auto& e : v) write(e);
      } else {  // single-pass range: length unknown up front
        byte(0x9f);
        for (const auto& e : v) write(e);
        byte(0xff);
      }
    } else if constexpr (reflectable_struct<T>) {
      head(5, field_count(v));
      struct_body(v);
    } else {
      static_assert(false, "sardine: type is not serializable");
    }
  }

  // Number of key/value pairs `v` contributes — its own fields plus, through
  // flatten, its children's (structs recurse, catch-all maps count entries).
  template <typename T>
  std::uint64_t field_count(const T& v) {
    std::uint64_t n = 0;
    template for (constexpr auto m : members_of<T>()) {
      if constexpr (!skip_ser(m)) {
        using M = [:std::meta::type_of(m):];
        if constexpr (has<flatten>(m) && reflectable_struct<M>) {
          n += field_count(v.[:m:]);
        } else if constexpr (has<flatten>(m) && map_like<M>) {
          n += std::uint64_t(std::ranges::distance(v.[:m:]));
        } else if constexpr (is_optional<M>::value && !has<emit_null>(m)) {
          if (v.[:m:].has_value()) ++n;
        } else {
          ++n;
        }
      }
    }
    return n;
  }

  // Emits the pairs of `v` without the map head (used by flatten and
  // internally tagged variants).
  template <typename T>
  void struct_body(const T& v) {
    template for (constexpr auto m : members_of<T>()) {
      if constexpr (!skip_ser(m)) {
        using M = [:std::meta::type_of(m):];
        if constexpr (has<flatten>(m) && reflectable_struct<M>) {
          struct_body(v.[:m:]);
        } else if constexpr (has<flatten>(m) && map_like<M>) {
          for (const auto& [k, mv] : v.[:m:]) {
            key(k);
            write(mv);
          }
        } else {
          bool present = true;
          if constexpr (is_optional<M>::value && !has<emit_null>(m))
            present = v.[:m:].has_value();
          if (present) {
            if constexpr (constexpr auto label = int_label(m); label.has_value()) {
              integer(*label);
            } else {
              text(member_key<T>(m));
            }
            write_member<m>(v.[:m:]);
          }
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

  // {"AltName": value} as a one-entry map; unit (monostate) alternatives
  // collapse to the text string "AltName".
  template <typename V>
  void variant_external(const V& v) {
    template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
      if (v.index() == I) {
        using A = std::variant_alternative_t<I, V>;
        if constexpr (std::same_as<A, std::monostate>) {
          text(alt_name<V, I>());
        } else {
          head(5, 1);
          text(alt_name<V, I>());
          write(std::get<I>(v));
        }
        return;
      }
    }
    null();  // valueless_by_exception
  }

  template <typename V>
  void variant_untagged(const V& v) {
    template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
      if (v.index() == I) {
        write(std::get<I>(v));
        return;
      }
    }
    null();
  }

  // {<tag>: "AltName", ...alt fields} — alternatives must all be structs.
  template <typename V>
  void variant_internal(const V& v, std::string_view tag_key) {
    template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
      if (v.index() == I) {
        using A = std::variant_alternative_t<I, V>;
        static_assert(reflectable_struct<A>,
                      "sardine::tag requires all variant alternatives to be structs");
        head(5, 1 + field_count(std::get<I>(v)));
        text(tag_key);
        text(alt_name<V, I>());
        struct_body(std::get<I>(v));
        return;
      }
    }
    null();
  }
};

// ---------------------------------------------------------------------------
// CBOR deserialization. Accepts definite and indefinite lengths and all
// three float widths; semantic tags (major 6) are skipped transparently.
// ---------------------------------------------------------------------------

struct cbor_reader {
  std::span<const std::uint8_t> in{};
  std::size_t pos = 0;
  int depth = 0;
  std::vector<std::string> path{};
  cbor_options opts{};
  static constexpr int max_depth = 256;

  [[noreturn]] void fail(const char* msg, errc code = errc::syntax) const {
    throw parse_error{msg, pos, code, joined_path(path)};
  }

  struct depth_guard {
    cbor_reader& p;
    explicit depth_guard(cbor_reader& p) : p(p) {
      if (++p.depth > max_depth) p.fail("nesting too deep", errc::depth_exceeded);
    }
    ~depth_guard() { --p.depth; }
  };

  std::uint8_t byte() {
    if (pos >= in.size()) fail("unexpected end of input", errc::truncated);
    return in[pos++];
  }
  std::uint64_t be(int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | byte();
    return v;
  }

  struct item {
    std::uint8_t major;   // 0..7
    std::uint8_t ai;      // additional info, before argument expansion
    std::uint64_t value;  // argument: int value, length, tag, float bits
    bool indefinite() const { return ai == 31; }
  };

  item raw_head() {
    std::uint8_t ib = byte();
    item h{std::uint8_t(ib >> 5), std::uint8_t(ib & 0x1f), ib & 0x1fu};
    if (h.ai < 24) return h;
    if (h.ai <= 27) {
      h.value = be(1 << (h.ai - 24));
      // RFC 8949 §3.3: a simple value in the two-byte form (major 7, ai 24)
      // must be ≥ 32 — the one-byte spellings own 0..23, and 24..31 are the
      // ai values themselves.
      if (h.major == 7 && h.ai == 24 && h.value < 32)
        fail("invalid two-byte simple value", errc::invalid_encoding);
      // Minimal-width heads: an argument that fit a shorter encoding is a
      // second spelling of the same item. Majors 0–6 only — for major 7 the
      // width IS the meaning (half/single/double float).
      if (opts.minimal_heads && h.major != 7) {
        static constexpr std::uint64_t floor_of[] = {24, 0x100, 0x10000,
                                                     0x100000000};
        if (h.value < floor_of[h.ai - 24])
          fail("non-minimal length encoding", errc::invalid_encoding);
      }
      return h;
    }
    if (h.ai == 31) {
      // ai 31 marks indefinite length, valid only for majors 2–5 (RFC 8949
      // §3.3). A break (major 7) is consumed by try_break before any head is
      // read, so one arriving here sits where a data item belongs.
      if (h.major == 7) fail("unexpected break", errc::invalid_encoding);
      if (h.major < 2 || h.major == 6)
        fail("indefinite length on an integer or tag", errc::invalid_encoding);
      if (opts.definite_only) fail("indefinite length", errc::invalid_encoding);
      h.value = 0;
      return h;
    }
    fail("reserved additional info", errc::invalid_encoding);
  }

  // Head of the next data item, with any semantic tags (major 6) skipped —
  // or refused, under no_tags.
  item head() {
    item h = raw_head();
    while (h.major == 6) {
      if (opts.no_tags) fail("semantic tag", errc::invalid_encoding);
      h = raw_head();
    }
    return h;
  }

  bool try_break() {
    if (pos >= in.size()) fail("unexpected end of input", errc::truncated);
    if (in[pos] == 0xff) { ++pos; return true; }
    return false;
  }

  void skip_bytes(std::uint64_t n) {
    if (n > in.size() - pos) fail("truncated string", errc::truncated);
    pos += std::size_t(n);
  }

  std::string chunk(std::uint64_t n) {
    if (n > in.size() - pos) fail("truncated string", errc::truncated);
    std::string s(reinterpret_cast<const char*>(in.data() + pos), std::size_t(n));
    pos += std::size_t(n);
    return s;
  }

  std::string text_body(item h) {
    if (!h.indefinite()) return chunk(h.value);
    std::string s;
    while (!try_break()) {
      item c = raw_head();  // chunks: definite text strings, no tags
      if (c.major != 3 || c.indefinite()) fail("bad text string chunk", errc::invalid_encoding);
      s += chunk(c.value);
    }
    return s;
  }

  std::string text() {
    item h = head();
    if (h.major != 3) fail("expected a text string", errc::type_mismatch);
    return text_body(h);
  }

  // Consumes null; `undefined` (0xf7) is accepted as null too, unless the
  // profile refuses substitutions.
  bool try_null() {
    std::size_t save = pos;
    if (pos < in.size()) {
      item h = head();
      if (h.major == 7 &&
          (h.ai == 22 || (h.ai == 23 && !opts.no_substitutions)))
        return true;
    }
    pos = save;
    return false;
  }

  template <typename T>
  T to_integer(item h) {
    if (h.major == 0) {
      if (!std::in_range<T>(h.value)) fail("integer out of range", errc::out_of_range);
      return T(h.value);
    }
    if (h.major == 1) {
      if (h.value > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        fail("integer out of range", errc::out_of_range);
      std::int64_t v = -1 - std::int64_t(h.value);
      if (!std::in_range<T>(v)) fail("integer out of range", errc::out_of_range);
      return T(v);
    }
    fail("expected an integer", errc::type_mismatch);
  }

  static double from_half(std::uint16_t h) {  // RFC 8949 appendix D
    unsigned exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    double v = exp == 0  ? std::ldexp(mant, -24)
             : exp != 31 ? std::ldexp(mant + 1024, int(exp) - 25)
             : mant == 0 ? std::numeric_limits<double>::infinity()
                         : std::numeric_limits<double>::quiet_NaN();
    return (h & 0x8000) ? -v : v;
  }

  double to_float(item h) {
    if (h.major == 7) {
      if (h.ai == 25) {
        if (opts.no_substitutions)
          fail("half-precision float", errc::invalid_encoding);
        return from_half(std::uint16_t(h.value));
      }
      if (h.ai == 26) return std::bit_cast<float>(std::uint32_t(h.value));
      if (h.ai == 27) return std::bit_cast<double>(h.value);
    }
    if (!opts.no_substitutions) {
      if (h.major == 0) return double(h.value);     // ints promote to float
      if (h.major == 1) return -1.0 - double(h.value);
    }
    fail("expected a number", errc::type_mismatch);
  }

  // Consume any well-formed data item without storing it (for unknown keys).
  void skip_value() {
    depth_guard g(*this);
    item h = head();
    switch (h.major) {
      case 0: case 1: return;  // the argument is the whole item
      case 2: case 3:
        if (!h.indefinite()) { skip_bytes(h.value); return; }
        while (!try_break()) {
          item c = raw_head();
          if (c.major != h.major || c.indefinite()) fail("bad string chunk", errc::invalid_encoding);
          skip_bytes(c.value);
        }
        return;
      case 4:
        if (h.indefinite()) { while (!try_break()) skip_value(); return; }
        for (std::uint64_t i = 0; i < h.value; ++i) skip_value();
        return;
      case 5:
        if (h.indefinite()) {
          while (!try_break()) { skip_value(); skip_value(); }
          return;
        }
        for (std::uint64_t i = 0; i < h.value; ++i) { skip_value(); skip_value(); }
        return;
      default:  // major 7; simple values and floats are fully in the head
        if (h.indefinite()) fail("unexpected break", errc::invalid_encoding);
        return;
    }
  }
};

template <typename T>
void read_value(cbor_reader& p, T& out);

// --- CBOR variant readers ---------------------------------------------------

template <typename V>
void read_variant_external(cbor_reader& p, V& out) {
  cbor_reader::depth_guard g(p);
  auto h = p.head();
  if (h.major == 3) {  // unit form: "AltName" selects a default-constructed alt
    std::string name = p.text_body(h);
    bool matched = false;
    template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
      if (!matched && name == alt_name<V, I>()) {
        matched = true;
        out.template emplace<I>();
      }
    }
    if (!matched) p.fail("unknown variant name", errc::unknown_variant);
    return;
  }
  if (h.major != 5 || (!h.indefinite() && h.value != 1))
    p.fail("expected a one-entry variant map", errc::type_mismatch);
  std::string name = p.text();
  bool matched = false;
  template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
    if (!matched && name == alt_name<V, I>()) {
      matched = true;
      std::variant_alternative_t<I, V> tmp{};
      read_value(p, tmp);
      out.template emplace<I>(std::move(tmp));
    }
  }
  if (!matched) p.fail("unknown variant name", errc::unknown_variant);
  if (h.indefinite() && !p.try_break()) p.fail("expected a one-entry variant map", errc::type_mismatch);
}

template <typename V>
void read_variant_internal(cbor_reader& p, V& out, std::string_view tag_key) {
  std::size_t start = p.pos;
  // First pass: scan the map for the tag key.
  std::string name;
  bool found = false;
  {
    cbor_reader::depth_guard g(p);
    auto h = p.head();
    if (h.major != 5) p.fail("expected an object", errc::type_mismatch);
    auto entry = [&] {
      std::string k = p.text();
      if (!found && k == tag_key) {
        name = p.text();
        found = true;
      } else {
        p.skip_value();
      }
    };
    if (h.indefinite()) { while (!p.try_break()) entry(); }
    else for (std::uint64_t i = 0; i < h.value; ++i) entry();
  }
  if (!found) p.fail("missing variant tag", errc::missing_field);
  // Second pass: re-parse the map as the selected alternative; the tag
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
  if (!matched) p.fail("unknown variant tag value", errc::unknown_variant);
}

// --- CBOR struct reader -----------------------------------------------------

template <typename T>
void read_struct(cbor_reader& p, T& out) {
  std::array<bool, members_of<T>().size()> seen{};
  cbor_reader::depth_guard g(p);
  auto h = p.head();
  if (h.major != 5) p.fail("expected an object", errc::type_mismatch);
  auto entry = [&] {
    auto kh = p.head();
    if (kh.major == 0 || kh.major == 1) {
      // Native integer key: COSE-style labels, matched against int_key fields.
      std::int64_t label = p.to_integer<std::int64_t>(kh);
      path_guard where(p, std::to_string(label));
      if (!try_read_int_key(p, out, label, seen.data())) {
        if constexpr (has<deny_unknown_fields>(^^T))
          p.fail("unknown field", errc::unknown_field);
        else p.skip_value();
      }
      return;
    }
    if (kh.major != 3) p.fail("expected a map key", errc::type_mismatch);
    std::string key = p.text_body(kh);
    path_guard where(p, key);
    if (!try_read_key(p, out, key, seen.data())) {
      if constexpr (has<deny_unknown_fields>(^^T)) p.fail("unknown field", errc::unknown_field);
      else p.skip_value();
    }
  };
  if (h.indefinite()) { while (!p.try_break()) entry(); }
  else for (std::uint64_t i = 0; i < h.value; ++i) entry();
  check_required<T>(p, seen.data());
}

template <typename T>
void read_value(cbor_reader& p, T& out) {
  if constexpr (std::same_as<T, bool>) {
    auto h = p.head();
    if (h.major == 7 && h.ai == 20) out = false;
    else if (h.major == 7 && h.ai == 21) out = true;
    else p.fail("expected true or false", errc::type_mismatch);
  } else if constexpr (std::is_enum_v<T>) {
    std::size_t at = p.pos;
    auto h = p.head();
    if (h.major == 3) {
      std::string name = p.text_body(h);
      template for (constexpr auto e : enumerators_of<T>()) {
        if (name == json_name<T>(e)) {
          out = [:e:];
          return;
        }
      }
      p.pos = at;
      p.fail("unknown enumerator", errc::unknown_enum);
    } else if constexpr (has<enum_from_number>(^^T)) {
      out = static_cast<T>(p.to_integer<std::underlying_type_t<T>>(h));
    } else {
      p.fail("expected an enumerator name", errc::type_mismatch);
    }
  } else if constexpr (std::integral<T>) {
    out = p.to_integer<T>(p.head());
  } else if constexpr (std::floating_point<T>) {
    out = static_cast<T>(p.to_float(p.head()));
  } else if constexpr (std::same_as<T, std::string>) {
    out = p.text();
  } else if constexpr (std::same_as<T, value>) {
    static_assert(false, "sardine: value is JSON-only (for now)");
  } else if constexpr (std::same_as<T, cbor_raw>) {
    // Validate one item (under the active profile) without interpreting it;
    // keep the verbatim bytes.
    std::size_t start = p.pos;
    p.skip_value();
    out.bytes.assign(p.in.begin() + std::ptrdiff_t(start),
                     p.in.begin() + std::ptrdiff_t(p.pos));
  } else if constexpr (is_optional<T>::value) {
    if (p.try_null()) {
      out.reset();
    } else {
      out.emplace();
      read_value(p, *out);
    }
  } else if constexpr (std::same_as<T, std::monostate>) {
    if (!p.try_null()) p.fail("expected null", errc::type_mismatch);
  } else if constexpr (is_variant<T>::value) {
    read_variant_external(p, out);
  } else if constexpr (map_like<T>) {
    cbor_reader::depth_guard g(p);
    auto h = p.head();
    if (h.major != 5) p.fail("expected a map", errc::type_mismatch);
    out.clear();
    auto entry = [&] {
      using K = typename T::key_type;
      if constexpr (string_like<K>) {
        std::string key = p.text();
        path_guard where(p, key);
        read_value(p, out[K(std::move(key))]);
      } else {  // integer key: native, or text for JSON-converted documents
        auto kh = p.head();
        K k{};
        if (kh.major == 3 && !p.opts.no_substitutions) {
          std::string s = p.text_body(kh);
          auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), k);
          if (ec != std::errc{} || end != s.data() + s.size())
            p.fail("expected an integer map key", errc::type_mismatch);
        } else {
          k = p.to_integer<K>(kh);
        }
        path_guard where(p, std::to_string(k));
        read_value(p, out[k]);
      }
    };
    if (h.indefinite()) { while (!p.try_break()) entry(); }
    else for (std::uint64_t i = 0; i < h.value; ++i) entry();
  } else if constexpr (resizable_bytes<T>) {
    auto h = p.head();
    out.clear();
    if (h.major == 2) {
      auto take = [&](std::uint64_t n) {
        if (n > p.in.size() - p.pos) p.fail("truncated string", errc::truncated);
        out.insert(out.end(), p.in.begin() + std::ptrdiff_t(p.pos),
                   p.in.begin() + std::ptrdiff_t(p.pos + n));
        p.pos += std::size_t(n);
      };
      if (!h.indefinite()) {
        take(h.value);
      } else {
        while (!p.try_break()) {
          auto c = p.raw_head();
          if (c.major != 2 || c.indefinite())
            p.fail("bad byte string chunk", errc::invalid_encoding);
          take(c.value);
        }
      }
    } else if (h.major == 4 && !p.opts.no_substitutions) {
      // The pre-bytes encoding (and what a JSON-converted document holds):
      // an array of small integers.
      cbor_reader::depth_guard g(p);
      auto element = [&] { out.push_back(p.to_integer<std::uint8_t>(p.head())); };
      if (h.indefinite()) { while (!p.try_break()) element(); }
      else for (std::uint64_t i = 0; i < h.value; ++i) element();
    } else {
      p.fail("expected a byte string", errc::type_mismatch);
    }
  } else if constexpr (sequence_like<T>) {
    cbor_reader::depth_guard g(p);
    auto h = p.head();
    if (h.major != 4) p.fail("expected an array", errc::type_mismatch);
    out.clear();
    std::size_t index = 0;
    auto element = [&] {
      path_guard where(p, std::to_string(index++));
      read_value(p, out.emplace_back());
    };
    if (h.indefinite()) { while (!p.try_break()) element(); }
    else for (std::uint64_t i = 0; i < h.value; ++i) element();
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
    } else if constexpr (std::same_as<T, value>) {
      json_writer jw;
      jw.write(v);
      out += jw.out;
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

// ---------------------------------------------------------------------------
// Schema generation ("meta class"): describe a type's SERIALIZED form as a
// JSON Schema document. The description honors the same annotations the
// writer does (rename / rename_all / skip / required / deny_unknown_fields /
// flatten), so schema<T>() is always true of to_json<T>()'s output. An
// endpoint can hand its schema to any peer -- the WSDL/IDL role, generated
// from the type instead of maintained beside it.
// ---------------------------------------------------------------------------

struct schema_writer {
  std::string out;

  void lit(std::string_view s) { out += s; }
  void key(std::string_view k) { escape_into(out, k); out += ':'; }

  template <typename T>
  void type_schema() {
    if constexpr (std::same_as<T, bool>) {
      lit(R"({"type":"boolean"})");
    } else if constexpr (std::is_enum_v<T>) {
      lit(R"({"enum":[)");
      bool first = true;
      template for (constexpr auto e : enumerators_of<T>()) {
        if (!std::exchange(first, false)) out += ',';
        escape_into(out, json_name<T>(e));
      }
      lit("]}");
    } else if constexpr (std::integral<T>) {
      lit(R"({"type":"integer"})");
    } else if constexpr (std::floating_point<T>) {
      lit(R"({"type":"number"})");
    } else if constexpr (string_like<T>) {
      lit(R"({"type":"string"})");
    } else if constexpr (std::same_as<T, value>) {
      lit("true");  // the boolean schema: any JSON value
    } else if constexpr (is_optional<T>::value) {
      lit(R"({"anyOf":[)");
      type_schema<typename T::value_type>();
      lit(R"(,{"type":"null"}]})");
    } else if constexpr (std::same_as<T, std::monostate>) {
      lit(R"({"type":"null"})");
    } else if constexpr (is_variant<T>::value) {
      variant_schema<T>();
    } else if constexpr (map_like<T>) {
      lit(R"({"type":"object","additionalProperties":)");
      type_schema<typename T::mapped_type>();
      lit("}");
    } else if constexpr (sequence_like<T>) {
      lit(R"({"type":"array","items":)");
      type_schema<std::ranges::range_value_t<T>>();
      lit("}");
    } else if constexpr (reflectable_struct<T>) {
      struct_schema<T>();
    } else {
      static_assert(false, "sardine: type has no schema");
    }
  }

  // Externally tagged (the type-level default): a unit (monostate) alternative
  // is the bare string "AltName"; others are {"AltName": <value>}.
  template <typename V>
  void variant_schema() {
    lit(R"({"oneOf":[)");
    bool first = true;
    template for (constexpr auto I : indices<std::variant_size_v<V>>()) {
      if (!std::exchange(first, false)) out += ',';
      using A = std::variant_alternative_t<I, V>;
      if constexpr (std::same_as<A, std::monostate>) {
        lit(R"({"const":)");
        escape_into(out, alt_name<V, I>());
        lit("}");
      } else {
        lit(R"({"type":"object","properties":{)");
        key(alt_name<V, I>());
        type_schema<A>();
        lit(R"(},"required":[)");
        escape_into(out, alt_name<V, I>());
        lit(R"(],"additionalProperties":false})");
      }
    }
    lit("]}");
  }

  template <typename T>
  void struct_schema() {
    lit(R"({"type":"object","title":)");
    escape_into(out, type_name<T>());
    lit(R"(,"properties":{)");
    bool first = true;
    properties_of<T>(first);
    lit(R"(},"required":[)");
    bool rfirst = true;
    required_of<T>(rfirst);
    lit(R"(],"additionalProperties":)");
    // Fidelity with the parser: unknown keys error only under
    // deny_unknown_fields; a flattened catch-all map also accepts anything.
    lit(has<deny_unknown_fields>(^^T) ? "false" : "true");
    lit("}");
  }

  // Emit properties, hoisting flattened structs like the writer does; a
  // flattened map is the catch-all and contributes no fixed properties.
  template <typename T>
  void properties_of(bool& first) {
    template for (constexpr auto m : members_of<T>()) {
      if constexpr (!skip_ser(m)) {
        using M = [:std::meta::type_of(m):];
        if constexpr (has<flatten>(m) && reflectable_struct<M>) {
          properties_of<M>(first);
        } else if constexpr (has<flatten>(m) && map_like<M>) {
          // catch-all: covered by additionalProperties
        } else {
          if (!std::exchange(first, false)) out += ',';
          key(member_key<T>(m));
          type_schema<M>();
        }
      }
    }
  }

  // required{} is per-level and not tracked through flatten (see README
  // deviations) -- the schema states exactly what the parser enforces.
  template <typename T>
  void required_of(bool& first) {
    template for (constexpr auto m : members_of<T>()) {
      if constexpr (!skip_de(m) && has<required>(m)) {
        if (!std::exchange(first, false)) out += ',';
        escape_into(out, member_key<T>(m));
      }
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
  detail::parser p{.in = json};
  T value{};
  try {
    detail::read_value(p, value);
    p.skip_ws();
    if (p.pos != json.size())
      return std::unexpected(
          error{"trailing characters after value", p.pos, errc::trailing, {}});
  } catch (const detail::parse_error& e) {
    return std::unexpected(error{e.message, e.offset, e.code, e.path});
  }
  return value;
}

// Deserialize ONTO an existing object: fields the document names are
// overwritten (an explicit null resets an optional), fields it omits keep
// the value they had — nested structs merge recursively, containers are
// replaced whole. The overlay/patch primitive: defaults or an earlier
// document first, this one on top.
template <typename T>
std::expected<void, error> from_json_into(std::string_view json, T& inout) {
  detail::parser p{.in = json};
  try {
    detail::read_value(p, inout);
    p.skip_ws();
    if (p.pos != json.size())
      return std::unexpected(
          error{"trailing characters after value", p.pos, errc::trailing, {}});
  } catch (const detail::parse_error& e) {
    return std::unexpected(error{e.message, e.offset, e.code, e.path});
  }
  return {};
}

// CBOR (RFC 8949) with the same data model and annotations as to_json:
// structs are maps with text keys, enums and variant tags are text strings.
// Integer map keys stay integers (JSON must stringify them).
template <typename T>
std::vector<std::uint8_t> to_cbor(const T& value) {
  detail::cbor_writer w;
  w.write(value);
  return std::move(w.out);
}

template <typename T>
std::expected<T, error> from_cbor(std::span<const std::uint8_t> cbor,
                                  cbor_options opts = {}) {
  detail::cbor_reader p{.in = cbor, .opts = opts};
  T value{};
  try {
    detail::read_value(p, value);
    if (p.pos != cbor.size())
      return std::unexpected(
          error{"trailing bytes after value", p.pos, errc::trailing, {}});
  } catch (const detail::parse_error& e) {
    return std::unexpected(error{e.message, e.offset, e.code, e.path});
  }
  return value;
}

// Decode one data item from the FRONT of `cbor`, reporting how many bytes it
// occupied. For values embedded mid-structure — a COSE key inside WebAuthn's
// attested credential data sits between fixed fields and optional extensions,
// and only the item itself says where it ends.
template <typename T>
std::expected<T, error> from_cbor_prefix(std::span<const std::uint8_t> cbor,
                                         std::size_t& consumed,
                                         cbor_options opts = {}) {
  detail::cbor_reader p{.in = cbor, .opts = opts};
  T value{};
  try {
    detail::read_value(p, value);
  } catch (const detail::parse_error& e) {
    return std::unexpected(error{e.message, e.offset, e.code, e.path});
  }
  consumed = p.pos;
  return value;
}

// JSON Schema of T's serialized form ("meta class"): what to_json<T> emits
// and from_json<T> accepts, annotations honored. Validates anywhere JSON
// Schema does; serves as the machine-readable protocol description an
// endpoint can hand to its peers.
template <typename T>
std::string schema() {
  detail::schema_writer w;
  w.template type_schema<T>();
  return std::move(w.out);
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
