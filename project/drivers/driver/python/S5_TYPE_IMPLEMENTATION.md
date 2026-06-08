# S5 Type Implementation (DLB-PYTHON-006)

Scope: `lanes/active/drivers/python` lane only.

## Changes

- Extended type codec parity in `src/scratchbird/types.py` for `TIMETZ`:
  - `encode_param(...)` now treats offset-aware `datetime.time` (`tzinfo` set) as `OID_TIMETZ` and emits a deterministic 12-byte binary payload (`<qi`: micros since midnight + zone seconds west of UTC).
  - `_decode_binary_value(...)` now supports `OID_TIMETZ` via `_decode_timetz(...)`.
  - `_decode_timetz(...)` handles:
    - 12-byte payloads with zone displacement.
    - Backward-compatible 8-byte payloads (UTC fallback when zone bytes are absent).
  - Text decode now routes through `_decode_text_typed_value(...)`, with typed parsing for scalar and temporal families (`bool`/`int`/`float`/`numeric`/`date`/`time`/`timetz`/`timestamp`/`timestamptz`/`uuid`).
  - Typed text decode now follows JDBC fail-fast semantics for invalid non-boolean typed payloads (parse errors are surfaced), while boolean decode remains token-based (`true`/`t` -> `True`, others -> `False`).
  - Temporal typed-text parsing now enforces JDBC family semantics:
    - `time` rejects timezone-offset payloads.
    - `timestamp` rejects timezone-offset payloads.
    - `timetz` requires an explicit timezone offset.
  - Binary temporal decode now enforces JDBC family semantics:
    - `timestamp` binary decode materializes naive datetimes.
    - `timestamptz` binary decode materializes UTC-aware datetimes.
  - Temporal text normalization now accepts trailing `Z` and normalizes it to UTC offset form.
  - `type_name(...)` now maps `OID_TIMETZ` to `"timetz"`.
- Added JDBC-style `BYTEA` decode parity in `src/scratchbird/types.py`:
  - Binary `OID_BYTEA` decode now detects and decodes escaped/hex textual payloads (`\\x`/`0x`/plain hex/octal escapes), while preserving raw binary payloads when text markers are absent.
  - Text decode for `OID_BYTEA` now follows the same decode path.
  - `bytea[]` typed-array conversion now materializes `bytes` elements via the same decoding rules.
- Added JDBC-style typed array parity in `src/scratchbird/types.py`:
  - Non-vector Python list/tuple payloads now infer stable array OIDs (for bool/bytea/int/float/text/date/time/timetz/timestamp/timestamptz/numeric/uuid families) instead of always using OID `0`.
  - Vector auto-detection is now restricted to float-only sequences so mixed numeric collections (`int` + `float`) follow JDBC-style typed-array OID inference instead of being forced to `vector`.
  - `_decode_binary_value(...)` now recognizes typed array OIDs and decodes them through array-literal parsing plus scalar-type conversion.
  - Array literal parsing now handles quoted and escaped string elements while preserving nested-array behavior.
  - Unquoted array tokens now remain text tokens (except `NULL`) so `text[]` decode preserves source lexical forms instead of coercing to Python bool/number values.
  - Boolean array element conversion now treats both short (`t`/`f`) and long (`true`/`false`) textual tokens with JDBC-consistent truthiness.
  - Temporal array element conversion now enforces JDBC timezone-family rules (`time` rejects offsets, `timestamp` rejects offsets, `timetz` requires offsets).
  - Typed array decode now materializes element families deterministically (`date`, `timetz`, `timestamptz`, `numeric`, `uuid`, etc.) instead of returning string-only payloads.
  - `type_name(...)` now includes array OID names (`text[]`, `boolean[]`, `timetz[]`, etc.).
- Added JDBC-style unknown-binary scalar parity in `src/scratchbird/types.py`:
  - `_decode_unknown_binary(...)` now decodes 1-byte payloads as signed int8 values (matching JDBC `byte` semantics) instead of unsigned byte values.
- Added JDBC-style unknown-text integer bounds parity in `src/scratchbird/types.py`:
  - `_parse_unknown_text(...)` now limits integer auto-coercion to signed int64 range; out-of-range integer text remains text (matching JDBC `Long.parseLong` behavior).
- Added JDBC-style temporal range-bound coercion parity in `src/scratchbird/types.py`:
  - `_encode_range_bound(...)` now accepts string temporal bounds for `daterange`/`tsrange`/`tstzrange` and coerces them deterministically to UTC-backed binary payloads.
- Added explicit wrapper-family parity in `src/scratchbird/types.py` and `src/scratchbird/__init__.py`:
  - Added lightweight wrapper types: `Blob`, `Clob`, `RowId`, `Ref`, `SqlXml`.
  - `encode_param(...)` now routes wrappers deterministically:
    - `Blob`/`RowId` -> `OID_BYTEA`
    - `Clob`/`Ref` -> `OID_TEXT`
    - `SqlXml` -> `OID_XML`
  - Package exports now include these wrappers for lane-visible API usage.
- Added JDBC-style fallback object encode parity in `src/scratchbird/types.py`:
  - `encode_param(...)` now encodes Python `Enum` values as text using enum member names.
  - Previously unsupported object values now fall back to `str(value)` text encoding (`OID_TEXT`) instead of hard-failing.
- Added deterministic tests in `tests/test_types.py`:
  - `test_encode_blob_wrapper_uses_bytea_oid`
  - `test_encode_clob_wrapper_uses_text_oid`
  - `test_encode_rowid_wrapper_uses_bytea_oid`
  - `test_encode_ref_wrapper_uses_text_oid`
  - `test_encode_sqlxml_wrapper_uses_xml_oid`
  - `test_encode_enum_parameter_uses_text_name`
  - `test_encode_custom_object_falls_back_to_text`
  - `test_decode_bytea_binary_prefixed_hex_payload`
  - `test_decode_bytea_binary_escaped_octal_payload`
  - `test_decode_bytea_text_payload`
  - `test_encode_timetz_uses_binary_layout_and_zone_seconds_west`
  - `test_decode_timetz_binary_payload_roundtrip`
  - `test_decode_timetz_binary_payload_supports_legacy_8byte_form`
  - `test_decode_timestamp_binary_payload_to_naive_datetime`
  - `test_decode_timestamptz_binary_payload_to_aware_utc_datetime`
  - `test_decode_timetz_text_payload_to_offset_time`
  - `test_decode_date_text_payload_to_date`
  - `test_decode_numeric_scalar_text_payloads`
  - `test_decode_bool_scalar_text_non_true_tokens_map_to_false`
  - `test_decode_typed_text_payloads_raise_on_invalid_parse`
  - `test_decode_time_text_payload_to_time`
  - `test_decode_time_text_payload_with_offset_raises`
  - `test_decode_timestamp_text_payload_to_naive_datetime`
  - `test_decode_timestamp_text_payload_with_offset_raises`
  - `test_decode_timestamptz_text_payload_to_aware_datetime`
  - `test_decode_timestamptz_text_payload_with_z_suffix_to_aware_datetime`
  - `test_decode_timetz_text_payload_without_offset_raises`
  - `test_decode_uuid_text_payload_to_uuid`
  - `test_type_name_includes_timetz`
  - `test_encode_string_array_infers_text_array_oid`
  - `test_encode_bool_array_infers_boolean_array_oid`
  - `test_encode_mixed_numeric_array_widens_to_float8_array_oid`
  - `test_decode_int4_array_literal_payload`
  - `test_decode_bool_array_literal_accepts_t_f_tokens`
  - `test_decode_text_array_literal_with_quotes_and_nested_arrays`
  - `test_decode_text_array_literal_preserves_unquoted_scalar_tokens`
  - `test_type_name_includes_array_names`
  - `test_encode_timetz_array_infers_timetz_array_oid`
  - `test_decode_date_array_to_date_values`
  - `test_decode_time_array_with_offset_payload_raises`
  - `test_decode_timetz_array_without_offset_payload_raises`
  - `test_decode_timestamp_array_with_offset_payload_raises`
  - `test_decode_numeric_array_to_decimal_values`
  - `test_decode_uuid_array_to_uuid_values`
  - `test_decode_timestamptz_array_to_aware_datetimes`
  - `test_decode_bytea_array_to_bytes_values`
  - `test_decode_unknown_binary_single_byte_is_signed`
  - `test_decode_unknown_text_integer_outside_int64_returns_text`
  - `test_encode_daterange_with_string_bounds_roundtrip`
  - `test_encode_tstzrange_with_string_bounds_roundtrip`
  - `test_encode_tsrange_with_string_bounds_roundtrip`
- Added deterministic always-on runtime type contract coverage in `tests/test_runtime_contract_gate.py`:
  - `test_runtime_gate_type_decode_without_env` validates runtime decode semantics for `TIMESTAMPTZ`, `NUMERIC`, and `BYTEA` without `SCRATCHBIRD_TEST_DSN`.

## Tests Run

1. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_types.py`
- Result: PASS (`59 passed`)

2. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_runtime_contract_gate.py`
- Result: PASS (`3 passed`)

3. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests`
- Result: PASS (`214 passed, 27 skipped, 1 warning`)

## TYPE Status Recommendation

- Recommendation: `IMPLEMENTED`
- Reason:
  - Deterministic type parity now explicitly includes `TIMETZ` encode/decode behavior (binary and text) aligned with JDBC lane expectations for zone-aware time payloads, plus typed text decode for scalar and temporal families (`bool`/`int`/`float`/`numeric`/`date`/`time`/`timestamp`/`timestamptz`/`uuid`).
  - Deterministic type parity now also includes typed array OID inference/decode behavior with quoted-string/nested-array literal parsing and typed element conversion coverage.
  - Deterministic decode parity now preserves untyped `text[]` token lexemes, interprets boolean array `t`/`f` tokens with JDBC-consistent semantics, and decodes signed 1-byte unknown-binary values in line with JDBC behavior.
  - Temporal text decode and temporal range-bound encode now include `Z`-suffix and string-bound coercion behavior for UTC-stable parity.
  - Mixed numeric collection encode now aligns with JDBC array inference (`float8[]`) while float-only vectors retain explicit `vector` encode behavior.
  - Typed text decode now also aligns with JDBC fail-fast parse semantics for invalid non-boolean typed payloads.
  - Temporal typed-text decode now enforces JDBC timezone rules for `time`/`timestamp`/`timetz` families.
  - Temporal typed-array conversion now enforces the same JDBC timezone rules for `time[]`/`timestamp[]`/`timetz[]` families.
  - Unknown-text integer inference now matches JDBC int64 limits; out-of-range integer text is preserved as text.
  - Binary `timestamp` vs `timestamptz` decode now aligns with JDBC timezone materialization behavior.
  - `BYTEA` decode behavior now aligns with JDBC escape/hex decoding semantics across binary, text, and array decode paths.
  - Wrapper-equivalent families now include explicit encode routing for `blob`/`clob`/`rowid`/`ref`/`sqlxml` wrappers with deterministic lane tests.
  - Parameter encode parity now includes enum-name and custom-object string fallback behavior aligned with JDBC’s text fallback path.
  - Existing scalar/json/range/composite/vector paths remain covered by deterministic lane tests, and runtime decode semantics now have always-on contract assertions without environment gating.
