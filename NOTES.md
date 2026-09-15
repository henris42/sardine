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
`undefined` as null, duplicate map keys (last wins), non-UTF-8 text, and
skips semantic tags. Each lenience has an off switch (`minimal_heads`,
`definite_only`, `no_tags`, `no_substitutions`, `reject_duplicate_keys`,
`validate_utf8`), and `sardine::cbor_strict` turns them all off. Both
profiles refuse input that RFC 8949 §3.3 calls not well-formed (ai 31 on an
integer or tag head, two-byte simple values below 32, stray breaks, bad
chunk types) — that is not a profile choice.

`cbor_strict` is the **sardine canonical profile**, not RFC 8949 §4.2
deterministic encoding: map keys stay in declaration/container order (§4.2
wants bytewise-sorted keys) and floats keep their static width (§4.2 wants
shortest-float). It guarantees "exactly one encoding verifies" between
sardine endpoints; a non-sardine verifier of the same wire must implement
this profile, or a future `sorted_keys` / deterministic-writer pair closes
the gap for COSE-style interop. Freeze whichever the wire picks in fixtures.

**`from_cbor_prefix(span, consumed, opts)`** decodes one item off the front
of a buffer and reports its length — for CBOR embedded mid-structure
(WebAuthn attested credential data, COSE keys followed by extensions).

**`[[=sardine::int_key(N)]]`** gives a struct field an integer map key
(negative allowed) — COSE/CTAP-shaped protocols (RFC 9052 labels). CBOR
writes and matches the label natively; JSON spells it as a decimal string
key. The decimal text spelling also matches in lenient CBOR (JSON-converted
documents); under `no_substitutions` — and therefore under `cbor_strict` —
a text key never matches an int_key member, so `{3: a}` and `{"3": a}` are
two different documents on the strict wire.

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
- **Block-scope structs need program-unique names.** GCC trunk mangles a
  reflection template argument (`^^local_struct::member`) without the
  enclosing function scope, so two local structs with the same name and a
  same-named member collide in one comdat group — an ICE
  (`symtab_node::verify failed`) when both are serialized in one TU. Local
  DTOs work fine; just don't call two of them `ask`.

## Security profile

Which knobs to set, by what the input is:

- **Hostile input at scale** (public HTTP bodies, anything unauthenticated):
  the JSON defaults already enforce what RFC 8259 requires of the document —
  UTF-8 (`validate_utf8`) and the §6 number grammar (`strict_numbers`) — and
  both readers take a `limits{}` budget: `max_depth` (default 256; lower it
  for public endpoints — each level costs several stack frames),
  `max_string_bytes`, `max_elements` per container (a definite CBOR length
  beyond it is refused before any element is read), and `max_total_items`
  for the whole document, skipped values included. Every budget fails with
  `errc::limit_exceeded`. Add `deny_unknown_fields` decoder-wide so the
  strict-unknown-key property is a call-site fact rather than a per-type
  annotation audit; `[[=sardine::allow_unknown_fields{}]]` opts the rare
  pass-through type back out (per-type deny still wins).
- **Signed or hashed wires**: `cbor_strict` (minimal heads, definite
  lengths, no tags, no substitutions, duplicate keys rejected, UTF-8
  enforced, int_key text spellings refused). For JSON set
  `reject_duplicate_keys`; the defaults cover numbers and UTF-8. Do not put
  `sardine::value` on a signed wire — it preserves duplicate keys by design.
- **Logged errors**: `redact_paths` replaces document key text in
  `error.path` with `<key>` while keeping indices, matched member names, and
  int_key labels. Even unredacted, path segments cap at 64 bytes and the
  joined path at 1024 (`…`-marked, cut at UTF-8 boundaries), and
  `error.message` is always a static string — it never contains input bytes.
- **Deliberately lenient defaults**: duplicate keys (last wins, matching
  most JSON parsers), CBOR's substitutions/tags/indefinite lengths, and
  non-UTF-8 CBOR text stay accepted by default because real peers emit them
  (WebAuthn authenticators, JSON-converted documents, hand-written config).
  Strictness is a wire contract, so it is opt-in per call site.

**Variant mode complexity**: `untagged` tries alternatives by parse-and-
rewind — k alternatives nested d deep can re-parse a subtree up to k^d
times. Every retry burns `max_total_items` budget, so the amplification is
bounded (and reported as `limit_exceeded`, not `unknown_variant`), but on
public endpoints prefer externally or internally tagged forms. Internally
tagged variants scan the object twice — a constant factor, also budgeted.

**Exception boundary**: `parse_error` never escapes the public API; `from_*`
return `std::expected`. The exception is allocation failure —
`std::bad_alloc` propagates.

## Errors

`sardine::error` carries a human message (static text, never input bytes),
the byte offset, an `errc` code to branch on (`unknown_field`,
`missing_field`, `type_mismatch`, `invalid_encoding`, `duplicate_field`,
`limit_exceeded`, …), and the dotted path of the failure inside the
document (`"profile.valid_secs"`, `"items.0.name"`). Paths are joined at
throw time, so they survive unwinding; they are bounded, and redactable,
as described under Security profile.

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
defaults, `std::tuple`. Duplicate keys are last-wins by default and rejected
under `reject_duplicate_keys`; `sardine::value` always preserves duplicates.
