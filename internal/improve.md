# sardine — Improvement Notes

Source: read-only review of `henris42/sardine` at commit `4626a44`
(`include/sardine/sardine.hpp`, tests, NOTES.md). Not built (no GCC 16
in the review environment). Line numbers refer to that commit.

Context: in storax-ca this library decodes every byte that enters the
daemons — REST JSON from the web, the ML-DSA-sealed CBOR envelope between
core and cryptod, WebAuthn/CTAP2 CBOR, and every JSON column saltherring
stores. The bar is therefore "parser on hostile input" plus "exactly one
encoding verifies" for the signed wire. The code is in good shape for
that: no length-driven pre-allocation, bounds checks before every read,
a depth guard on both readers, `std::from_chars` everywhere, and a
strict CBOR profile that already exists. The notes below are what is
still missing.

Priorities: **P0** — a signed-wire or malformed-input acceptance issue;
**P1** — hardening needed for untrusted input at scale; **P2** — hygiene.

---

## D-1 · P0 · Duplicate map keys are accepted, including under `cbor_strict`

**Where**: `read_struct` JSON (1196–1214) and CBOR (1810–1840);
`try_read_key` (1116–1153); map readers (1256–1279, 1899–1922);
`value` objects (by design, 220).

**Problem**: a second occurrence of a key silently overwrites the first
in bound structs and maps; unknown duplicates are skipped. Two
implementations may disagree on which wins, so one signature can cover
two meanings — exactly what the strict profile exists to prevent
(RFC 8949 §5.6 says a strict decoder must reject). The `seen[]` array
already exists, so direct-member detection is nearly free.

**Fix**:
- Add `reject_duplicate_keys` to `cbor_options`; set it in
  `cbor_strict`. Add the same to a new `json_options` (see D-5).
- In `try_read_key` / `try_read_int_key`: if `seen && seen[I]` already,
  fail `errc::duplicate_field` (new code). Flattened structs pass
  `nullptr` today — give them their own `seen` array so duplicates inside
  a flattened level are caught too.
- For unknown keys that are skipped and for `map_like` targets, keep a
  small per-level set of keys (`std::vector<std::string>` with linear
  search is fine for typical map sizes; switch to a hash set above ~32)
  and fail on repeat. For `map_like`, `out.contains(k)` before insert
  suffices when the container is empty at entry (it is — `out.clear()`).
- `sardine::value` keeps duplicates (fidelity); document that `value`
  is not for signed wires.

**Test**: `{"a":1,"a":2}` → error under the option, last-wins without;
CBOR map with two identical text keys, two identical int keys, and one
int key spelled both natively and as text (`3` and `"3"` — see D-3);
duplicate inside a flattened struct; duplicate in `std::map` target.

---

## D-2 · P0 · Malformed CBOR heads decode as values

**Where**: `cbor_reader::raw_head` (1572–1595), `try_null` (1640–1650),
`read_value` bool/simple handling (1843–1848).

**Problem**:
- Additional-info 31 is only valid for majors 2, 3, 4, 5 (indefinite)
  and 7 (break). `raw_head` returns `value = 0` for any major, so
  `0x1f` decodes as integer 0, `0x3f` as −1, and `0xdf` as tag 0 (then
  loops to the next head). RFC 8949 §3.3: these are not well-formed.
- Major 7 with ai 24 and a value below 32 is not well-formed (§3.3);
  currently accepted and, since `minimal_heads` skips major 7, never
  refused.
- A `0xff` break outside an indefinite container reaches `read_value`
  and is reported as a type mismatch rather than `invalid_encoding`;
  fine for callers but wrong for a fuzz oracle — classify it.

**Fix**: in `raw_head`, when `ai == 31` and `major` is 0, 1, or 6,
fail `invalid_encoding`. When `major == 7 && ai == 24 && value < 32`,
fail. Add a well-formedness pass in `skip_value` for the same. Under
`minimal_heads`, also refuse major 7 ai 24 encoding a value that fits
ai < 24 (simple values 0–23 written with the one-byte form).

**Test**: the RFC 8949 Appendix A vectors plus a negative corpus:
`1f`, `3f`, `df 00`, `f8 1f`, `ff`, `9f ff` (empty indefinite array is
valid), `5f 00 ff` (bad chunk type), `7f 7f 61 61 ff ff` (nested
indefinite text chunks — invalid).

---

## D-3 · P0 · `int_key` matches its decimal text spelling regardless of profile

**Where**: `member_key` (380–385), `try_read_key` (1131), CBOR
`read_struct` text-key branch (1830–1836).

**Problem**: NOTES.md warns not to combine `int_key` with a strict
signed wire because the text spelling `"3"` matches label `3`. The
warning should be enforcement: under `no_substitutions` (and therefore
`cbor_strict`), a text key must never match an `int_key` member.
Combined with D-1, `{3: a, "3": b}` currently has two encodings for one
field.

**Fix**: in the CBOR text-key path, pass `p.opts.no_substitutions`
into `try_read_key` (or a separate `try_read_text_key` that skips
`int_key` members when the flag is set). JSON keeps matching the text
spelling — JSON has no integer keys.

**Test**: strict decode of a COSE-shaped struct with `"3": …` → error;
lenient decode still accepts; JSON unaffected.

---

## D-4 · P1 · No UTF-8 validation on input text

**Where**: JSON `parse_string` (819–864), CBOR `chunk`/`text_body`
(1614–1634), writer `escape_into` (448–470).

**Problem**: invalid UTF-8 in JSON strings and CBOR major-3 text passes
through into `std::string` members and back out on write. RFC 8949 §3.1
requires text strings to be valid UTF-8; RFC 8259 §8.1 requires UTF-8
for JSON. Overlong encodings and surrogate code points in raw bytes are
a classic differential between parsers, and stored invalid UTF-8 later
breaks Postgres `TEXT` inserts (saltherring stores sardine JSON in TEXT).
`\u0000` escapes produce embedded NUL, which is legal but worth a knob.

**Fix**: `validate_utf8` option in both readers (on in `cbor_strict`,
and on by default for JSON — off is the odd case). Reject overlongs,
surrogates D800–DFFF, and code points above 10FFFF. Optional
`reject_nul`. Consider `validate_utf8` in the writer as a debug
assertion only.

**Test**: table of invalid sequences (`C0 80`, `ED A0 80`, `F4 90 80 80`,
truncated lead bytes) in both formats; valid 4-byte sequences round-trip.

---

## D-5 · P1 · Decode limits and a `json_options` struct

**Where**: `parser::max_depth` (745, constexpr 256),
`cbor_reader::max_depth` (1540); `from_json` (2255) takes no options.

**Problem**: depth is the only limit and it is not configurable; JSON
has no options at all. For untrusted input a caller needs per-decode
budgets: maximum string/byte-string length, maximum container element
count, maximum total items, and a lower depth for public endpoints.
Each nesting level costs several stack frames
(`read_value → read_struct → try_read_key → read_member → read_value`),
so 256 is a reasonable default for internal wires and too generous for
a public HTTP body.

**Fix**:
```cpp
struct limits {
  int max_depth = 256;
  std::size_t max_string_bytes = 16u << 20;
  std::size_t max_elements = 1u << 20;   // per container
  std::size_t max_total_items = 4u << 20; // whole document
};
struct json_options {
  limits lim{};
  bool deny_unknown_fields = false;   // decoder-wide, see D-6
  bool reject_duplicate_keys = false; // D-1
  bool validate_utf8 = true;          // D-4
  bool strict_numbers = false;        // D-8
};
// cbor_options gains `limits lim{}` and the D-1/D-4 flags.
```
Provide `from_json<T>(text, const json_options&)` and keep the
no-options overloads. Add `errc::limit_exceeded`.

**Test**: each limit trips with the exact error and offset; a
`[[[[…]]]]` document of depth `max_depth+1` fails without stack growth
measurable by a `rlimit`-constrained test process.

---

## D-6 · P1 · Decoder-wide `deny_unknown_fields`

**Where**: `read_struct` (1207, 1826, 1834) — per-type annotation only.

**Problem**: storax-ca's stated property is "strict unknown-key
rejection on everything that enters". With annotation-only control, one
forgotten `[[=sardine::deny_unknown_fields{}]]` silently opens a type.
A decoder-wide switch makes the property a call-site fact
(`from_json<T>(body, api_strict)`) that a lint or test can check.

**Fix**: `json_options::deny_unknown_fields` and
`cbor_options::deny_unknown_fields`; the annotation ORs with the option.
Add the inverse annotation `[[=sardine::allow_unknown_fields{}]]` for
the rare pass-through type so the option can be on globally.

**Test**: unannotated struct rejects unknown keys under the option;
`allow_unknown_fields` type still accepts.

---

## D-7 · P1 · Backtracking cost of `untagged` and internally tagged variants

**Where**: `read_variant_untagged` (1027–1046),
`read_variant_internal` JSON (1048–1091) and CBOR (1768–1806).

**Problem**: untagged variants try each alternative by exception and
rewind; nested untagged variants multiply — k alternatives at depth d
re-parse the subtree up to k^d times. Internally tagged variants scan
the object twice. Both are fine on trusted documents and a CPU
amplifier on hostile ones.

**Fix**: count re-parses against `limits::max_total_items` (each
attempted alternative consumes budget), so the amplification is bounded
by the same knob as everything else. Document the complexity in
NOTES.md and recommend externally/internally tagged forms on public
endpoints. Under `cbor_strict`, consider refusing `untagged` at compile
time via a `static_assert` in a `strict_decodable<T>` concept that
storax-ca can apply to envelope types.

**Test**: benchmark test with three-deep untagged nesting and a document
that fails at the last alternative; assert the budget error fires
before wall-clock becomes quadratic.

---

## D-8 · P1 · JSON number grammar is looser than RFC 8259

**Where**: `number_token` (866–885), `read_number` (972–990),
`read_document` (958–971).

**Problem**: the tokenizer accepts any run of `[0-9.eE+-]`, then relies
on `std::from_chars`, which accepts leading zeros (`01`), a trailing dot
(`1.`), and `1.e5`. These are not JSON. Harmless for typed reads, but a
signed or hashed JSON body that another implementation refuses is a
differential.

**Fix**: `strict_numbers` (D-5) enforces the RFC grammar:
`-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?` before `from_chars`.
On by default; the lenient path stays for `from_json_into` on
config files if wanted.

**Test**: `01`, `1.`, `.5`, `+1`, `1.e5`, `-` → `invalid_number`;
`-0`, `1e-7`, `1E+2` accepted.

---

## D-9 · P1 · Name the strict profile precisely; offer RFC deterministic encoding

**Where**: `cbor_strict` (198), writer `head`/float encoding
(1315–1330, float path), NOTES.md CBOR section.

**Problem**: `cbor_strict` is "sardine-canonical": minimal heads,
definite lengths, no tags, no substitutions, but key order is
declaration order and floats are written at static width. RFC 8949
§4.2 deterministic encoding requires sorted keys and shortest-float.
For sardine-to-sardine wires (core↔cryptod) that is fine; for COSE
interop (WebAuthn is read-only, but future COSE signing or an external
verifier of the envelope is not) it is a mismatch that will surface as
"signature verifies here, not there".

**Fix**:
- Rename in docs: `cbor_strict` = "sardine canonical profile" and say
  explicitly what a verifier must implement.
- Add `cbor_options::sorted_keys` (decoder: fail if a map's keys are
  not in §4.2 bytewise-lexicographic order of their encodings) and
  `cbor_encode_options{ deterministic = true }` (writer: sort struct
  fields by encoded key, emit shortest float that round-trips, e.g. via
  half → single → double checks). Keep both off by default.
- The envelope in storax-ca should pick one and freeze it in the wire
  fixtures.

**Test**: RFC 8949 §4.2 examples; encode→decode with `sorted_keys` set
passes; a declaration-order document fails under `sorted_keys`.

---

## D-10 · P1 · Errors carry document keys and values

**Where**: `path_guard` (734–739), `check_required` (1178–1194),
`joined_path` (725–732), `parse_error.path`.

**Problem**: `error.path` is built from attacker-supplied key text, and
messages such as "unknown enumerator" are followed by the offending
value in callers that format the error. In a CA, decode errors are
logged; the path may contain secrets when a document is mis-shaped
(a key named like a token, or a value that landed in the key
position). Also unbounded: a 1 MB key becomes a 1 MB path segment.

**Fix**: cap each path segment (64 bytes, then `…`) and total path
length; add `json_options::redact_paths` that replaces key text with
`<key>` and keeps indices; document that `error.message` never contains
input bytes (true today — add a test that keeps it true).

**Test**: a 100 KB key produces a bounded path; `redact_paths` yields
`items.0.<key>`.

---

## D-11 · P2 · `sardine::value` numerics and CBOR support

**Where**: `read_document` (958–971), `value` (226–277).

**Problem**: integers beyond int64 silently become doubles (documented);
for evidence-preserving use (audit payloads, JWS claims) precision loss
is a fidelity bug. `value` is JSON-only, so CBOR pass-through needs
`cbor_raw`, which cannot be inspected.

**Fix**: add a `value::big_int` (string of digits) alternative or an
option `numbers_as_text`; implement `read_value(cbor_reader&, value&)`
with byte strings as a `bytes` alternative and integer keys as text.

**Test**: `18446744073709551615` and `-9223372036854775809` round-trip
through `value` textually.

---

## D-12 · P2 · Writer edge cases

**Where**: `number_into` (473–484), `escape_into` (448–470),
CBOR `integer` (1326–1333).

- Non-finite floats silently become JSON `null`; add
  `json_encode_options{ non_finite = null|error }` — a CA should never
  emit `null` for a number it did not intend.
- Optionally escape U+2028/U+2029 and `</` for documents embedded in
  HTML/JS contexts (`escape_html_safe`); off by default.
- `integer(T)` for `unsigned __int128`/`__int128` is not supported;
  `static_assert` a clear message rather than a template error.

---

## D-13 · P2 · Enum decoding under strict profiles

**Where**: JSON `read_value` enum branch (1216–1236), CBOR (1849–1866).

**Problem**: `enum_from_number` accepts any integer, including values
that are not enumerators; the writer also emits a bare number for an
un-named value. Fine for flags-style enums, a silent widening for
closed sets.

**Fix**: `[[=sardine::enum_closed{}]]` (or make it the default under
strict options) rejecting numbers outside the enumerator set; `errc::
unknown_enum`.

---

## D-14 · P2 · Test infrastructure: fuzz harnesses and differential oracles

**Where**: `tests/`, `Makefile`.

**Fix**:
- `fuzz/json_typed.cpp`, `fuzz/json_value.cpp`, `fuzz/cbor_typed.cpp`,
  `fuzz/cbor_strict.cpp`, `fuzz/cbor_prefix.cpp`, each instantiating a
  "kitchen-sink" struct (every annotation, nested variants, maps with
  int keys, `cbor_raw`, flattened structs). Build with AFL++
  `afl-g++-fast` (GCC plugin) and ASan/UBSan; seed with the RFC vectors
  and the repository's own test documents.
- Round-trip property: `from(to(x)) == x` for generated values; and for
  the strict profile, `to(from(bytes)) == bytes` for every accepted
  input (the "exactly one encoding" property, stated as a test).
- Differential: decode the fuzz corpus with a second CBOR
  implementation (e.g. `cbor-diag` or a Python `cbor2` script in CI)
  and assert both accept or both reject under equivalent strictness;
  same for JSON against a strict reference parser.
- Publish the invalid-input corpus from D-2/D-4/D-8 as
  `tests/corpus/invalid/` so storax-ca's T-fuzz-2 can reuse it.

---

## D-15 · P2 · Documentation

- A "Security profile" section in NOTES.md: which options to set for
  hostile input (D-5 defaults), for signed wires (D-1, D-3, D-4, D-9),
  and what is deliberately lenient in the default decoder and why.
- State the complexity of variant modes (D-7).
- State the exception boundary: `parse_error` never escapes the public
  API; `from_*` are `noexcept` in effect (consider marking the public
  entry points `noexcept` after making `std::bad_alloc` handling
  explicit — today an allocation failure propagates as an exception
  from a function returning `std::expected`).

---

## Mapping to storax-ca

| Note | Where it matters | storax-ca test id |
|---|---|---|
| D-1, D-2, D-3, D-9 | core↔cryptod envelope, any signed CBOR | T-sep-4 (envelope forgery), T-fuzz-3, wire fixtures |
| D-2, D-5 | WebAuthn / CTAP2 CBOR from authenticators | T-auth-2, T-fuzz-4 |
| D-4 | all inputs; Postgres TEXT storage via saltherring | T-fuzz-2, saltherring S-9/S-12 |
| D-5, D-6, D-8 | REST bodies, EST/ACME JSON | T-authz-3 (mass assignment), T-dos-2 (size caps), T-fuzz-6/7 |
| D-7 | any endpoint accepting `untagged` variants | T-dos-4 |
| D-10 | audit/log redaction | T-aud-2 |
| D-14 | fuzz program | security plan §7 |

Recommended order: D-2 (small, closes accepted-malformed cases), D-1 and
D-3 together (one PR, both touch `try_read_key`), then D-5 with D-4/D-6/
D-8 riding on the new options struct, then D-9 as its own design PR.