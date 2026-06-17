-- cast_refusals.sql
-- Negative cast coverage for the ScratchBird type conversion surface.
--
-- Every statement in this file is expected to FAIL with a conversion
-- diagnostic.  A statement that SUCCEEDS is the finding (silent lossy or
-- unsafe conversion that the engine should have refused).
--
-- Source authority: Language_Reference/data_types/conversion_matrix.md
-- (Lossy Conversions table, Core Matrix "no ordinary cast" rows, Implicit
-- Assignment "should not silently" list, Diagnostics table, and sub-matrices
-- for Text/Binary/UUID, Temporal, Document/Collection, Vector.)
--
-- One cast statement per line/statement so error counts map to statements.
--
-- NOTE: Protected-material release refusals (cast from secret_ref or
-- protected_blob_ref to text/binary) are out of scope here because no
-- protected-material objects are created in the example schema.  The
-- conversion_matrix.md "Protected Material" table documents that these are
-- denied by default; the proof is deferred to a security surface test.

-- ===========================================================================
-- SECTION 1: DECIMAL WITH FRACTIONAL PART -> INTEGER
-- Lossy Conversions table: "Decimal with fractional part to integer — Refuse."
-- ===========================================================================

select cast(cast(3.14 as decimal(10, 2)) as int32)  as frac_decimal_to_int32;
select cast(cast(9.99 as decimal(10, 2)) as int64)  as frac_decimal_to_int64;
select cast(cast(-1.5 as decimal(10, 1)) as int8)   as frac_decimal_to_int8;
select cast(cast(0.1 as decimal(5, 1)) as uint32)   as frac_decimal_to_uint32;
select cast(cast(100.001 as decimal(10, 3)) as int128) as frac_decimal_to_int128;

-- text literal with fraction -> integer (must parse as decimal then refuse)
select cast('3.14' as int32)   as text_frac_to_int32;
select cast('9.9' as int64)    as text_frac_to_int64;

-- ===========================================================================
-- SECTION 2: DECIMAL TO LOWER PRECISION / SCALE (DATA LOSS)
-- Lossy Conversions table: "Decimal to lower precision/scale — Refuse unless
-- explicit cast policy admits rounding."
-- ===========================================================================

-- decimal(10,4) -> decimal(10,2): scale loss on fractional digits
select cast(cast(1.2345 as decimal(10, 4)) as decimal(10, 2)) as decimal_scale_loss;

-- decimal(18,2) -> decimal(5,2): precision loss (value too large for p=5)
select cast(cast(12345678901234.99 as decimal(18, 2)) as decimal(5, 2)) as decimal_prec_loss;

-- decimal(14,4) -> decimal(14,2): rounding/truncation of last 2 fractional digits
select cast(cast(1.9999 as decimal(14, 4)) as decimal(14, 2)) as decimal_frac_trunc;

-- ===========================================================================
-- SECTION 3: APPROXIMATE REAL -> EXACT NUMERIC (NON-EXACT VALUES)
-- Lossy Conversions table: "Approximate real to exact numeric — Refuse for
-- non-exact values, NaN, infinity, or out-of-range values."
-- ===========================================================================

-- NaN to integer
select cast(cast('NaN' as double precision) as int32)          as nan_to_int32;

-- NaN to decimal
select cast(cast('NaN' as real) as decimal(10, 2))             as nan_to_decimal;

-- Infinity to exact integer
select cast(cast('Infinity' as double precision) as int64)     as inf_to_int64;

-- Negative infinity to exact integer
select cast(cast('-Infinity' as double precision) as int32)    as neg_inf_to_int32;

-- Infinity to decimal
select cast(cast('Infinity' as double precision) as decimal(18, 4)) as inf_to_decimal;

-- Non-exact real that cannot be represented exactly in decimal
-- (e.g. 1.1 in binary64 is not exactly 1.1 in decimal)
select cast(cast(1.1 as double precision) as decimal(38, 38))  as nonexact_double_to_decimal;

-- Out-of-range double -> int32 (value exceeds int32 max 2147483647)
select cast(cast(3000000000.0 as double precision) as int32)   as double_outofrange_to_int32;

-- ===========================================================================
-- SECTION 4: TIMESTAMP WITH TIMEZONE -> TIMESTAMP (TIMEZONE LOSS)
-- Lossy Conversions table: "timestamptz to timestamp — Refuse unless
-- timezone-loss policy is explicit."
-- Core Matrix: timestamptz -> timestamp: requires explicit timezone-loss rule.
-- ===========================================================================

select cast(cast('2026-06-08 14:30:00-04:00' as timestamptz) as timestamp) as timestamptz_to_timestamp_tz_loss;

-- ===========================================================================
-- SECTION 5: TIMESTAMP -> LOWER PRECISION (PRECISION LOSS)
-- Lossy Conversions table: "Timestamp to lower precision — Refuse unless
-- explicit cast policy admits precision loss."
-- temporal_types.md: "Assignment to lower precision — Refused unless an
-- explicit rounding/truncation policy or function is used."
-- ===========================================================================

select cast(cast('2026-06-08 14:30:00.123456' as timestamp(6)) as timestamp(0)) as timestamp_prec_loss;
select cast(cast('14:30:00.999999' as time(6)) as time(0))                      as time_prec_loss;

-- ===========================================================================
-- SECTION 6: TEXT -> SHORTER TEXT (TRUNCATION)
-- Lossy Conversions table: "Text to shorter text — Refuse unless explicit
-- truncation function is used."
-- text_collation_and_charset.md: "Silent truncation is not admitted by default."
-- ===========================================================================

-- A 10-character string -> varchar(5): over-length
select cast('HelloWorld' as varchar(5))   as text_truncation_10_to_5;

-- A 6-character string -> char(3): character length exceeded
select cast('abcdef' as char(3))          as text_truncation_6_to_char3;

-- Long text -> varchar(1): extreme truncation
select cast('This is a long string' as varchar(1)) as text_truncation_long_to_1;

-- ===========================================================================
-- SECTION 7: SIGNED NEGATIVE -> UNSIGNED
-- Core Matrix: "signed integer -> unsigned integer: Negative or out-of-range
-- values are refused."
-- numeric_types.md: "Negative input refuses unsigned assignment."
-- ===========================================================================

select cast(-1 as uint8)    as neg_to_uint8;
select cast(-1 as uint16)   as neg_to_uint16;
select cast(-1 as uint32)   as neg_to_uint32;
select cast(-1 as uint64)   as neg_to_uint64;
select cast(-1 as uint128)  as neg_to_uint128;
select cast(cast(-128 as int8) as uint8)  as neg_int8_to_uint8;

-- ===========================================================================
-- SECTION 8: OUT-OF-RANGE INTEGER -> NARROWER INTEGER
-- Core Matrix: "signed integer -> wider signed integer: Refused if value
-- cannot fit."
-- Diagnostics: "Out-of-range value — Conversion diagnostic."
-- ===========================================================================

-- int32 max + 1 -> int16 (exceeds int16 max 32767)
select cast(cast(32768 as int32) as int16)       as int32_too_big_for_int16;

-- int64 max -> int32 (exceeds int32 max 2147483647)
select cast(cast(9223372036854775807 as int64) as int32) as int64_too_big_for_int32;

-- int128 value exceeding int64 range -> int64
select cast(cast(9223372036854775808 as int128) as int64) as int128_too_big_for_int64;

-- uint64 max -> uint32 (exceeds uint32 max 4294967295)
select cast(cast(18446744073709551615 as uint64) as uint32) as uint64_too_big_for_uint32;

-- unsigned value exceeding signed range: uint64 > int64 max -> int64
select cast(cast(18446744073709551615 as uint64) as int64)  as uint64_max_to_int64_overflow;

-- ===========================================================================
-- SECTION 9: INVALID TEXT -> NUMERIC
-- Diagnostics: "Invalid text-to-number cast — Conversion diagnostic."
-- ===========================================================================

select cast('abc' as int32)           as invalid_text_to_int32;
select cast('12.34x' as decimal(10,2)) as invalid_text_to_decimal;
select cast('' as int64)              as empty_text_to_int64;
select cast('1e999' as int32)         as overflow_exp_text_to_int32;
select cast('--5' as int32)           as double_neg_to_int32;
select cast('NaN' as decimal(10, 2))  as nan_text_to_decimal;
select cast('true' as int32)          as bool_text_to_int32;

-- ===========================================================================
-- SECTION 10: INVALID TEXT -> UUID
-- binary_uuid_and_protected_values.md: "UUID text invalid — Conversion
-- diagnostic."
-- ===========================================================================

select cast('not-a-uuid' as uuid)                   as invalid_text_to_uuid;
select cast('00000000000000000000000000000000' as uuid) as no_hyphens_to_uuid;
select cast('' as uuid)                             as empty_text_to_uuid;
select cast('gggggggg-0000-0000-0000-000000000000' as uuid) as invalid_hex_to_uuid;
select cast('018f0000-0000-7000-8000-0000000000' as uuid)   as short_uuid_to_uuid;

-- ===========================================================================
-- SECTION 11: INVALID TEXT -> TEMPORAL
-- temporal_types.md: "Invalid date or time field — Conversion diagnostic."
-- conversion_matrix.md: "text to temporal: Requires valid literal syntax."
-- ===========================================================================

-- Invalid date fields
select cast('2026-13-01' as date)     as invalid_month_to_date;
select cast('2026-00-15' as date)     as zero_month_to_date;
select cast('2026-06-32' as date)     as invalid_day_to_date;
select cast('not-a-date' as date)     as text_to_date_invalid;

-- Invalid time fields
select cast('25:00:00' as time)       as invalid_hour_to_time;
select cast('14:60:00' as time)       as invalid_minute_to_time;
select cast('abc' as time)            as text_to_time_invalid;

-- Invalid timestamp
select cast('2026-06-32 14:30:00' as timestamp)  as invalid_ts_day;
select cast('not-a-timestamp' as timestamp)      as text_to_ts_invalid;

-- Invalid timestamptz
select cast('2026-06-08 14:30:00 NOTAZONE' as timestamptz) as invalid_tz_to_timestamptz;

-- ===========================================================================
-- SECTION 12: BINARY WITH INVALID ENCODING -> TEXT
-- Lossy Conversions table: "Binary to text with invalid encoded bytes — Refuse."
-- binary_uuid_and_protected_values.md: "Invalid binary literal encoding —
-- Parse or conversion diagnostic."
-- ===========================================================================

-- OMITTED: We cannot construct a bytea literal with known-invalid UTF-8 bytes
-- using only cast expression syntax without a decode/encode named function.
-- The refusal rule is confirmed in the Lossy Conversions table.
-- The proof requires a binary literal with bytes that are invalid for the
-- target charset, which depends on named function syntax (e.g. decode with
-- a hex string containing invalid sequences), not a bare cast expression.

-- ===========================================================================
-- SECTION 13: DOMAIN CONSTRAINT VIOLATIONS
-- domains_casts_and_coercion.md: "Domain check fails — Refuse assignment."
-- Diagnostics: "Domain validation failure — Domain diagnostic."
-- ===========================================================================

-- app.email_addr: text without '@' violates check (position('@' in value) > 0)
select cast('notanemail' as app.email_addr)    as bad_email_no_at;
select cast('' as app.email_addr)              as bad_email_empty;

-- app.positive_qty: zero violates check (value > 0)
select cast(0 as app.positive_qty)             as zero_not_positive;

-- app.positive_qty: negative value violates check (value > 0)
select cast(-1 as app.positive_qty)            as neg_not_positive;

-- app.email_addr: null violates not null policy
select cast(null as app.email_addr)            as null_to_email_addr;

-- app.positive_qty: null violates not null policy
select cast(null as app.positive_qty)          as null_to_positive_qty;

-- ===========================================================================
-- SECTION 14: PROTECTED MATERIAL REFUSALS (OUT OF SCOPE)
-- NOTE: Protected-release refusal (cast from secret_ref / protected_blob_ref
-- to text or binary) is out of scope for this file.  No protected-material
-- objects are instantiated in the example schema.  The conversion_matrix.md
-- "Protected Material" table and the "no ordinary cast" Core Matrix row
-- document that these are denied regardless of cast syntax.  A dedicated
-- security surface test harness should exercise those paths against a schema
-- that includes protected-material table columns or function results.
-- ===========================================================================

-- ===========================================================================
-- SECTION 15: VECTOR DIMENSION MISMATCH
-- conversion_matrix.md: "Vector dimension change — Refused unless a named
-- operation defines padding, projection, or embedding conversion."
-- document_graph_vector_and_multimodel_types.md: "Vector dimension mismatch —
-- Bind diagnostic."
-- ===========================================================================

-- OMITTED: Cannot construct two vector literals of differing dimension using
-- only cast expression syntax without a named vector constructor.  The refusal
-- rule is confirmed; the proof depends on named operation syntax not shown in
-- the reviewed data_types pages.

-- ===========================================================================
-- END OF cast_refusals.sql
-- ===========================================================================
