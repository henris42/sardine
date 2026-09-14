# Notes

## Serde parity

| Serde | sardine |
|---|---|
| `rename = "x"` | `[[=sardine::rename("x")]]` |
| `rename_all = "camelCase"` | `[[=sardine::rename_all("camelCase")]]` |
| `skip` / `skip_serializing` / `skip_deserializing` | same names, `{}` suffix |
| `flatten` | `[[=sardine::flatten{}]]` — struct inlining and map catch-all |
| `deny_unknown_fields` | `[[=sardine::deny_unknown_fields{}]]` |
| required fields | `[[=sardine::required{}]]` (opt-in; absent fields keep defaults) |
| enums w/ payload | `std::variant` — externally tagged by default |
| `tag = "type"` | `[[=sardine::tag("type")]]` |
| `untagged` | `[[=sardine::untagged{}]]` |
| `Option<T>` | `std::optional<T>` ↔ null |
| fieldless enums | enum class ↔ `"enumerator_name"` |
| `to_string_pretty` | `sardine::to_json_pretty(v, indent = 2)` |
| `serde_cbor` / `ciborium` | `sardine::to_cbor(v)` / `from_cbor<T>` — same annotations |
| `Debug`, `{:?}` / `{:#?}` | `sardine::dbg(v)` with `{}` / `{:#}` |
| schemars `schema_for!` | `sardine::schema<T>()` — JSON Schema of the serialized form |

Types: bool, integers, floats, `std::string`, `optional`, `variant`,
`monostate`, sequences, maps (string or integer keys), nested structs, enums.
Parser handles `\uXXXX` escapes incl. surrogate pairs; nesting-depth guard.
Field names resolve at compile time and live in static storage
(`std::define_static_string`) — runtime never computes names.

## CBOR (RFC 8949)

`to_cbor`/`from_cbor<T>` share the JSON data model and annotations: structs
are maps with text keys, enums and variant tags are text strings. Format
differences: integer map keys stay integers, nan/inf encode natively (JSON
degrades them to null), floats encode at their static width (`float` → 4
bytes, `double` → 8; no shortest-float search). The encoder emits definite
lengths and minimal-width heads (unsized input ranges fall back to indefinite
arrays); map/field order follows declaration/container order — deterministic,
but not RFC 8949 §4.2 *sorted* order, on purpose (declaration order is the
protocol author's order).

**Byte strings**: any sequence of exactly `std::uint8_t` (`vector`, `array`,
`span`) is a CBOR byte string (major 2), both directions — serde_bytes
without the wrapper, because C++ can dispatch on the element type. The JSON
pair still writes an array of numbers (JSON has no bytes). *Breaking change*
against the first CBOR release, which wrote u8 sequences as integer arrays;
the lenient decoder still reads that spelling.

**Decode profiles** (`cbor_options`, passed to `from_cbor`/`from_cbor_prefix`):
the default decoder is liberal — it accepts indefinite-length
strings/arrays/maps, non-minimal argument encodings, half-precision floats,
integers where a float is expected, text-encoded integer map keys,
`undefined` as null, and skips semantic tags. Each lenience has an off
switch (`minimal_heads`, `definite_only`, `no_tags`, `no_substitutions`), and
`sardine::cbor_strict` turns them all off — for wires where a message is
signed and exactly one encoding of it may verify.

**`from_cbor_prefix(span, consumed, opts)`** decodes one item off the front
of a buffer and reports its length — for CBOR embedded mid-structure
(WebAuthn attested credential data, COSE keys followed by extensions).

**`[[=sardine::int_key(N)]]`** gives a struct field an integer map key
(negative allowed) — COSE/CTAP-shaped protocols (RFC 9052 labels). CBOR
writes and matches the label natively; JSON spells it as a decimal string
key. The decimal text spelling also matches in CBOR (JSON-converted
documents), so don't combine int_key with a signed strict wire that must
refuse the text spelling.

**`sardine::cbor_raw`** holds one verbatim, validated-but-uninterpreted CBOR
item (WebAuthn `attStmt`, extensions carried through unread). CBOR-only; an
empty one writes null.

## GCC 16.1 notes

- `-freflection` is required; `-std=c++26` alone doesn't enable reflection.
- Annotations must be structural: no `string_view`/`const char*` members —
  names are stored by value in a `char[64]`.
- `[[=rename{"x"}]]` doesn't parse; use `[[=rename("x")]]` or `[[=(rename{"x"})]]`.
- `nonstatic_data_members_of` returns a heap `std::vector` — pipe through
  `std::define_static_array` before `template for`.
- GCC spells the annotation query `annotations_of_with_type(info, info)`.

## Errors

`sardine::error` carries a human message, the byte offset, an `errc` code to
branch on (`unknown_field`, `missing_field`, `type_mismatch`,
`invalid_encoding`, …), and the dotted path of the failure inside the
document (`"profile.valid_secs"`, `"items.0.name"`). Paths are joined at
throw time, so they survive unwinding.

## The generic document: `sardine::value`

Typed structs are the front door; `sardine::value` is the escape hatch for
documents whose shape is the data's, not the program's. `from_json<value>`
parses anything; it nests as a struct member (typed envelope, generic
payload). Fidelity semantics: objects keep insertion order AND duplicate keys
(`find` returns the first match), and an integer too large for int64 degrades
to double instead of failing. JSON-only for now.

`from_json_into(text, obj)` deserializes ONTO an existing object — named
fields overwrite (explicit null resets an optional), omitted fields keep
their values, nested structs merge recursively, containers replace whole.
The overlay/patch primitive.

## Deviations from Serde

- Missing fields default silently unless `required` (Serde errors unless
  `default`).
- **Disengaged `std::optional` struct members are OMITTED from output** (no
  `"k":null`) unless the field carries `[[=sardine::emit_null{}]]`. Serde
  emits null unless told otherwise; document round-tripping wants omission.
- **Enums read from strings only** — a numeric value where an enum belongs is
  a `type_mismatch` unless the enum is annotated
  `[[=sardine::enum_from_number{}]]`. (Writing an un-named enumerator value
  still emits the number.)
- Variant "names" are the alternative's type name.
- `required` isn't tracked through `flatten`.
- Internally tagged parsing re-scans for the tag — don't combine with
  `deny_unknown_fields` on the alternative.

Not implemented: adjacently tagged variants, `serialize_with`, custom
defaults, `std::tuple`, duplicate-key detection in bound structs (last one
wins; `sardine::value` preserves duplicates).
