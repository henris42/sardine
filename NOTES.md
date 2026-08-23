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
| `Debug`, `{:?}` / `{:#?}` | `sardine::dbg(v)` with `{}` / `{:#}` |
| schemars `schema_for!` | `sardine::schema<T>()` — JSON Schema of the serialized form |

Types: bool, integers, floats, `std::string`, `optional`, `variant`,
`monostate`, sequences, maps (string or integer keys), nested structs, enums.
Parser handles `\uXXXX` escapes incl. surrogate pairs; nesting-depth guard.
Field names resolve at compile time and live in static storage
(`std::define_static_string`) — runtime never computes names.

## GCC 16.1 notes

- `-freflection` is required; `-std=c++26` alone doesn't enable reflection.
- Annotations must be structural: no `string_view`/`const char*` members —
  names are stored by value in a `char[64]`.
- `[[=rename{"x"}]]` doesn't parse; use `[[=rename("x")]]` or `[[=(rename{"x"})]]`.
- `nonstatic_data_members_of` returns a heap `std::vector` — pipe through
  `std::define_static_array` before `template for`.
- GCC spells the annotation query `annotations_of_with_type(info, info)`.

## Deviations from Serde

- Missing fields default silently unless `required` (Serde errors unless
  `default`).
- Variant "names" are the alternative's type name.
- `required` isn't tracked through `flatten`.
- Internally tagged parsing re-scans for the tag — don't combine with
  `deny_unknown_fields` on the alternative.

Not implemented: adjacently tagged variants, `serialize_with`, custom
defaults, `std::tuple`, duplicate-key detection.
