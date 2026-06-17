-- 37_casts.sql
-- Positive cast coverage for the ScratchBird type conversion surface.
-- Every statement in this file SHOULD SUCCEED.  A diagnostic is a real finding.
--
-- Canonical type spellings are sourced from:
--   Language_Reference/data_types/numeric_types.md
--   Language_Reference/data_types/temporal_types.md
--   Language_Reference/data_types/text_collation_and_charset.md
--   Language_Reference/data_types/binary_uuid_and_protected_values.md
--   Language_Reference/data_types/document_graph_vector_and_multimodel_types.md
--   Language_Reference/data_types/domains_casts_and_coercion.md
--   Language_Reference/data_types/conversion_matrix.md
--
-- Grammar:
--   cast(expression as data_type)
--   try_cast(expression as data_type)
--
-- Organization mirrors the conversion_matrix.md section order.

-- ===========================================================================
-- SECTION 1: NULL ADOPTION
-- null adopts nullable target descriptor (Core Matrix row 1)
-- ===========================================================================

select cast(null as int32)           as null_int32;
select cast(null as text)            as null_text;
select cast(null as uuid)            as null_uuid;
select cast(null as decimal(14, 2))  as null_decimal;
select cast(null as timestamptz)     as null_timestamptz;
select cast(null as boolean)         as null_boolean;
select cast(null as bytea)           as null_bytea;

-- ===========================================================================
-- SECTION 2: SIGNED INTEGER WIDENING
-- signed integer -> wider signed integer (implicit assignment, explicit cast)
-- int8 -> int16 -> int32 -> int64 -> int128
-- ===========================================================================

-- int8 -> int16
select cast(cast(100 as int8) as int16)   as int8_to_int16;

-- int8 -> int32
select cast(cast(-100 as int8) as int32)  as int8_to_int32;

-- int16 -> int32
select cast(cast(32767 as int16) as int32)  as int16_to_int32;

-- int16 -> int64
select cast(cast(-32768 as int16) as int64) as int16_to_int64;

-- int32 -> int64
select cast(cast(2147483647 as int32) as int64) as int32_to_int64;

-- int32 -> int128
select cast(cast(-2147483648 as int32) as int128) as int32_to_int128;

-- int64 -> int128
select cast(cast(9223372036854775807 as int64) as int128) as int64_to_int128;

-- literal suffix binding to explicit widths
select cast(42I8   as int16)  as i8_literal_to_int16;
select cast(1000I16 as int32) as i16_literal_to_int32;
select cast(1000I32 as int64) as i32_literal_to_int64;
select cast(1000I64 as int128) as i64_literal_to_int128;

-- ===========================================================================
-- SECTION 3: UNSIGNED INTEGER WIDENING
-- unsigned integer -> wider unsigned integer
-- uint8 -> uint16 -> uint32 -> uint64 -> uint128
-- ===========================================================================

select cast(cast(255 as uint8) as uint16)   as uint8_to_uint16;
select cast(cast(65535 as uint16) as uint32) as uint16_to_uint32;
select cast(cast(0 as uint16) as uint64)    as uint16_to_uint64;
select cast(cast(4294967295 as uint32) as uint64)  as uint32_to_uint64;
select cast(cast(18446744073709551615 as uint64) as uint128) as uint64_to_uint128;

-- canonical max uint128 via text cast (verified in numeric_types.md)
select cast('340282366920938463463374607431768211455' as uint128) as max_uint128;

-- unsigned literal suffix binding
select cast(255U8   as uint16)  as u8_literal_to_uint16;
select cast(1000U16 as uint32)  as u16_literal_to_uint32;
select cast(1000U32 as uint64)  as u32_literal_to_uint64;

-- ===========================================================================
-- SECTION 4: SIGNED <-> UNSIGNED (IN-RANGE VALUES)
-- signed -> unsigned (no negatives, in-range): explicit cast
-- unsigned -> signed (in-range): explicit cast
-- ===========================================================================

-- signed -> unsigned (positive, in range)
select cast(cast(127 as int8) as uint8)     as int8_to_uint8_inrange;
select cast(cast(32767 as int16) as uint16) as int16_to_uint16_inrange;
select cast(cast(100 as int32) as uint32)   as int32_to_uint32_inrange;
select cast(cast(9223372036854775807 as int64) as uint64) as int64_to_uint64_inrange;

-- unsigned -> signed (in range)
select cast(cast(200 as uint8) as int16)    as uint8_to_int16_inrange;
select cast(cast(255 as uint8) as int16)    as uint8_255_to_int16;
select cast(cast(65535 as uint16) as int32) as uint16_to_int32_inrange;
select cast(cast(4294967295 as uint32) as int64) as uint32_to_int64_inrange;

-- ===========================================================================
-- SECTION 5: EXACT INTEGER -> DECIMAL (EXACT WIDENING)
-- exact integer -> decimal(p,s) where precision admits the value
-- ===========================================================================

select cast(42 as decimal(10, 0))      as int_to_decimal_p10;
select cast(42 as decimal(10, 2))      as int_to_decimal_p10s2;
select cast(-99 as decimal(5, 0))      as neg_int_to_decimal;
select cast(1000 as numeric(14, 2))    as int_to_numeric14_2;
select cast(cast(9999 as int32) as decimal(10, 2)) as int32_to_decimal;
select cast(cast(0 as int64) as decimal(18, 4))    as zero_int64_to_decimal;

-- ===========================================================================
-- SECTION 6: EXACT NUMERIC -> APPROXIMATE REAL
-- exact numeric -> real / double precision (explicit cast, may be lossy)
-- ===========================================================================

select cast(cast(123 as int32) as real)              as int_to_real;
select cast(cast(123 as int64) as double precision)  as int64_to_double;
select cast(cast(3.14 as decimal(10, 2)) as real)    as decimal_to_real;
select cast(cast(3.14 as decimal(10, 2)) as double precision) as decimal_to_double;
select cast(999 as real)                             as literal_to_real;
select cast(999 as double precision)                 as literal_to_double;

-- ===========================================================================
-- SECTION 7: APPROXIMATE REAL -> EXACT NUMERIC (EXACT REPRESENTABLE VALUES)
-- explicit cast; NaN / infinity / out-of-range REFUSED (see refusals file)
-- Only exact representable finite values here.
-- ===========================================================================

-- Cast a real that is exactly representable as an integer
select cast(cast(10.0 as real) as int32)           as real_exact_10_to_int32;
select cast(cast(0.0 as double precision) as int64) as double_zero_to_int64;

-- Cast a double that is exactly representable as a decimal
select cast(cast(1.5 as double precision) as decimal(10, 1)) as double_to_decimal_exact;

-- ===========================================================================
-- SECTION 8: NUMERIC -> TEXT
-- rendering is descriptor-owned; explicit cast required
-- ===========================================================================

select cast(42 as text)                      as int_to_text;
select cast(-99 as text)                     as neg_int_to_text;
select cast(3.14 as text)                    as decimal_to_text;
select cast(cast(3.14 as real) as text)      as real_to_text;
select cast(cast(3.14 as double precision) as text) as double_to_text;
select cast(cast(100 as int8) as text)       as int8_to_text;
select cast(cast(255 as uint8) as text)      as uint8_to_text;
select cast(cast(9223372036854775807 as int64) as text) as int64_to_text;

-- ===========================================================================
-- SECTION 9: TEXT -> NUMERIC
-- invalid syntax is diagnostic (see refusals); valid syntax succeeds
-- ===========================================================================

select cast('42' as int32)                   as text_to_int32;
select cast('-99' as int64)                  as text_to_int64;
select cast('255' as uint8)                  as text_to_uint8;
select cast('3.14' as decimal(10, 2))        as text_to_decimal;
select cast('3.14' as double precision)      as text_to_double;
select cast('0' as int8)                     as text_to_int8;
select cast('18446744073709551615' as uint64) as text_to_uint64_max;
select cast('100.00' as numeric(14, 2))      as text_to_numeric;

-- ===========================================================================
-- SECTION 10: BOOLEAN <-> TEXT
-- ===========================================================================

-- boolean -> text
select cast(true as text)   as true_to_text;
select cast(false as text)  as false_to_text;

-- text -> boolean (accepted canonical spellings)
select cast('true' as boolean)   as text_true_to_bool;
select cast('false' as boolean)  as text_false_to_bool;

-- try_cast: boolean <-> text
select try_cast(true as text)    as try_true_to_text;
select try_cast('false' as boolean) as try_text_to_bool;

-- ===========================================================================
-- SECTION 11: TEXT -> TEMPORAL
-- text literal requires explicit cast; valid literal syntax succeeds
-- ===========================================================================

-- text -> date
select cast('2026-06-08' as date)              as text_to_date;
select cast('2000-01-01' as date)              as text_to_date_y2k;

-- text -> time
select cast('14:30:00' as time)                as text_to_time;
select cast('00:00:00.000000' as time(6))      as text_to_time_p6;

-- text -> timestamp
select cast('2026-06-08 14:30:00' as timestamp)        as text_to_timestamp;
select cast('2026-06-08 14:30:00.123456' as timestamp(6)) as text_to_timestamp_p6;

-- text -> timestamptz
select cast('2026-06-08 14:30:00-04:00' as timestamptz) as text_to_timestamptz;
select cast('2026-06-08T14:30:00Z' as timestamptz)      as text_to_timestamptz_iso;

-- text -> interval
select cast('1 year 2 months' as interval)        as text_to_interval_ym;
select cast('3 days 04:30:00' as interval)        as text_to_interval_ds;

-- ===========================================================================
-- SECTION 12: TEMPORAL -> TEXT
-- rendering uses timezone/profile policy; explicit cast required
-- ===========================================================================

select cast(cast('2026-06-08' as date) as text)          as date_to_text;
select cast(cast('14:30:00' as time) as text)            as time_to_text;
select cast(cast('2026-06-08 14:30:00' as timestamp) as text) as timestamp_to_text;
select cast(cast('2026-06-08 14:30:00-04:00' as timestamptz) as text) as timestamptz_to_text;
select cast(cast('3 days 04:30:00' as interval) as text) as interval_to_text;

-- ===========================================================================
-- SECTION 13: TEMPORAL -> TEMPORAL (SUPPORTED EXPLICIT DIRECTIONS)
-- Ref: conversion_matrix.md Temporal Conversions table
-- ===========================================================================

-- date -> timestamp: policy-dependent; explicit cast form admitted
-- (Default time-of-day policy must be explicit per the matrix)
select cast(cast('2026-06-08' as date) as timestamp)     as date_to_timestamp;

-- timestamp -> timestamptz: requires timezone rule; explicit cast
select cast(cast('2026-06-08 14:30:00' as timestamp) as timestamptz) as timestamp_to_timestamptz;

-- timestamptz -> date (time loss; but date extraction is a supported explicit cast direction)
-- NOTE: The matrix lists "timestamp to date" as refusing time loss unless explicit cast policy
-- admits it.  Cast is explicit here, which satisfies "explicit cast policy."
select cast(cast('2026-06-08 14:30:00+00:00' as timestamptz) as date) as timestamptz_to_date;

-- ===========================================================================
-- SECTION 14: TEXT <-> BINARY (WHERE PLAIN CAST IS DOCUMENTED)
-- conversion_matrix.md: "text to binary: Requires encoding or decode rule."
-- binary_uuid_and_protected_values.md: "Hex text converted to binary: Requires
-- explicit decode or cast function."
-- The docs do not document a bare cast(text as bytea) path without an encoding
-- rule.  Named function decode/encode forms are the admitted surface.
-- OMITTED: bare cast('...' as bytea) — no encoding-rule form is demonstrated
-- in the reference without a decode function.  encode/decode are named
-- conversion functions (not cast expressions).  Would be expressed as:
--   select encode(cast('hello' as bytea), 'escape');  -- named function, not cast
-- Included below only where the reference confirms a plain cast is admitted.
-- ===========================================================================

-- OMITTED: text -> binary plain cast — docs require encoding/decode rule.
--   The reference shows "explicit encoding or decode function" as the required
--   detail, not a bare cast.  Named function form is out of scope for this
--   cast-expression harness.

-- OMITTED: binary -> text plain cast — docs require charset or encoding rule.
--   Same rationale.

-- ===========================================================================
-- SECTION 15: TEXT <-> UUID
-- ===========================================================================

-- text -> uuid
select cast('018f0000-0000-7000-8000-000000000001' as uuid) as text_to_uuid;
select cast('00000000-0000-0000-0000-000000000000' as uuid) as text_to_uuid_zero;

-- uuid -> text
select cast(cast('018f0000-0000-7000-8000-000000000001' as uuid) as text) as uuid_to_text;

-- try_cast text -> uuid (valid)
select try_cast('018f0000-0000-7000-8000-000000000002' as uuid) as try_text_to_uuid_valid;

-- uuid literal syntax (from binary_uuid_and_protected_values.md)
select uuid '018f0000-0000-7000-8000-000000000001'             as uuid_literal;

-- ===========================================================================
-- SECTION 16: UUID <-> BINARY(16)
-- ===========================================================================

-- uuid -> binary(16): explicit cast required
select cast(cast('018f0000-0000-7000-8000-000000000001' as uuid) as binary(16)) as uuid_to_binary16;

-- binary(16) -> uuid: explicit cast required; validates UUID descriptor policy
-- OMITTED: We cannot construct a valid 16-byte binary(16) literal from text
-- without an encoding/decode function, and the reference does not show a bare
-- literal form for binary(16).  This direction is confirmed supported but
-- requires a named decode function to produce the binary(16) argument.
-- OMITTED: binary(16) -> uuid — depends on binary literal or decode function
--   which is a named conversion function outside the cast expression surface.

-- ===========================================================================
-- SECTION 17: DOCUMENT SCALAR <-> SCALAR
-- ===========================================================================

-- scalar -> document: explicit cast creates a document scalar value
select cast(42 as document)            as int_to_document;
select cast('hello' as document)       as text_to_document;
select cast(true as document)          as bool_to_document;
select cast(3.14 as document)          as decimal_to_document;

-- document path extraction -> scalar: requires path extraction operator then cast
-- The matrix says "document scalar to scalar: Requires path extraction and
-- scalar descriptor compatibility."
-- Using ->>'key' path extraction operator yielding text, then cast to scalar:
select cast(attributes->>'description' as text) as doc_path_to_text
from app.catalog.products
where attributes is not null
limit 1;

-- Extract a numeric path and cast to decimal
select cast(attributes->>'price_override' as decimal(14, 2)) as doc_path_to_decimal
from app.catalog.products
where attributes is not null
limit 1;

-- document_graph_vector_and_multimodel_types.md example:
select cast(payload->>'amount' as decimal(18, 2)) as doc_payload_amount
from audit.change_log
where detail is not null
limit 1;

-- ===========================================================================
-- SECTION 18: DOMAIN CASTS
-- Cast base carrier value to domain: validates carrier + null policy + checks.
-- Domains from 11_domains.sql:
--   app.email_addr  — carrier: text, not null, check position('@' in value) > 0
--   app.positive_qty — carrier: bigint, not null, check value > 0
--   app.money       — carrier: numeric(14,2)
-- ===========================================================================

-- text -> app.email_addr (valid address)
select cast('user@example.com' as app.email_addr)     as text_to_email_addr;
select cast('admin@scratchbird.io' as app.email_addr) as text_to_email_addr2;

-- bigint -> app.positive_qty (positive value)
select cast(1 as app.positive_qty)      as int_to_positive_qty_min;
select cast(100 as app.positive_qty)    as int_to_positive_qty_100;
select cast(9223372036854775807 as app.positive_qty) as int_to_positive_qty_max;

-- numeric -> app.money (valid decimal)
select cast(0.00 as app.money)          as zero_to_money;
select cast(99.99 as app.money)         as decimal_to_money;
select cast(12345678901234.99 as app.money) as large_decimal_to_money;

-- domain -> base carrier: domain erasure (policy-dependent; explicit cast)
select cast(cast('user@example.com' as app.email_addr) as text)        as email_addr_to_text;
select cast(cast(5 as app.positive_qty) as bigint)                      as positive_qty_to_bigint;
select cast(cast(9.99 as app.money) as numeric(14, 2))                  as money_to_numeric;

-- try_cast with domain (valid values)
select try_cast('test@example.com' as app.email_addr)  as try_email_valid;
select try_cast(42 as app.positive_qty)                as try_qty_valid;
select try_cast(1.00 as app.money)                     as try_money_valid;

-- ===========================================================================
-- SECTION 19: TRY_CAST — ADDITIONAL SUCCESS CASES
-- try_cast uses the same validation path as cast; returns value on success.
-- ===========================================================================

select try_cast('123' as int32)                          as try_text_to_int32;
select try_cast('2026-06-08' as date)                    as try_text_to_date;
select try_cast('3.14' as decimal(10, 2))                as try_text_to_decimal;
select try_cast('2026-06-08 14:30:00-04:00' as timestamptz) as try_text_to_timestamptz;
select try_cast(42 as text)                              as try_int_to_text;
select try_cast(null as int64)                           as try_null_to_int64;
select try_cast(true as text)                            as try_bool_to_text;

-- ===========================================================================
-- SECTION 20: IMPLICIT ASSIGNMENT SAFE-WIDENING (exercised via table read)
-- Reads from existing tables to confirm implicit descriptor widening.
-- ===========================================================================

-- int64 (bigint) column read as int128 via explicit cast
select cast(product_id as int128) as product_id_as_int128
from app.catalog.products
limit 1;

-- double precision column -> decimal (exact representable, explicit cast)
select cast(value as decimal(18, 6)) as sensor_value_as_decimal
from ts.sensor_reading
limit 1;

-- timestamptz column -> text
select cast(created_at as text) as created_at_text
from app.catalog.products
limit 1;

-- uuid column -> text
select cast(external_ref as text) as external_ref_text
from app.sales.customers
where external_ref is not null
limit 1;

-- date column -> timestamp (explicit)
select cast(signup_date as timestamp) as signup_as_timestamp
from app.sales.customers
limit 1;

-- ===========================================================================
-- SECTION 21: ARRAY ELEMENT CONVERSION (supported explicit direction)
-- conversion_matrix.md: "array element -> another element descriptor: explicit cast;
-- every element conversion must be admitted."
-- ===========================================================================

-- OMITTED: array<T> literal construction — the reference does not document
-- a portable literal syntax for array construction (e.g. ARRAY[...]) in the
-- data_types pages reviewed.  The conversion rule is confirmed supported but
-- the safe literal form for constructing an array<int32> to cast element-wise
-- to array<int64> is not grounded in the reviewed pages.

-- ===========================================================================
-- SECTION 22: VECTOR ELEMENT TYPE CHANGE
-- conversion_matrix.md: "vector element type change: Explicit cast; dimension
-- must match; quantization/lossiness must be explicit."
-- ===========================================================================

-- OMITTED: vector element type cast — the reviewed pages confirm the rule but
-- do not provide literal syntax for vector construction or casting between
-- vector element types (real32 -> int8 etc.).  The document_graph_vector page
-- shows 'vector search ... using :query_vector' but no cast literal form.

-- ===========================================================================
-- END OF 37_casts.sql
-- ===========================================================================
