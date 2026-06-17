-- 35_builtin_functions.sql
-- Exercises documented SBsql built-in function packages through SELECT statements.
-- Source authority: Language_Reference/functional_reference/ (sb_core.md,
--   sb_temporal.md, sb_uuid.md, sb_vector.md, sb_json.md).
-- Function names, signatures, and examples verified against the functional
-- reference pages and conformance IDs cited therein.

-- ===========================================================================
-- A. String / text functions  (sb.core)
-- ===========================================================================

select
    -- Case conversion
    upper('ScratchBird Demo')                           as upper_ex,
    lower('ScratchBird Demo')                           as lower_ex,

    -- Trimming
    trim('  hello  ')                                   as trim_ex,
    ltrim('  hello  ')                                  as ltrim_ex,
    rtrim('  hello  ')                                  as rtrim_ex,
    btrim('##hello##', '#')                             as btrim_ex,

    -- Padding
    lpad('42', 6, '0')                                  as lpad_ex,
    rpad('ok', 5, '.')                                  as rpad_ex,

    -- Length
    char_length('ScratchBird')                          as char_length_ex,
    length('ScratchBird')                               as length_ex,

    -- Substring / position
    substring('ScratchBird' from 1 for 7)               as substr_ex,
    position('Bird' in 'ScratchBird')                   as position_ex,

    -- Replace and concatenation
    replace('foo_bar_baz', '_', '-')                    as replace_ex,
    concat('Hello', ' ', 'World')                       as concat_ex,

    -- Overlay (replace substring at position)
    overlay('ScratchBird' placing 'Cat' from 1 for 7)   as overlay_ex,

    -- ASCII / NULLIF / COALESCE
    ascii('A')                                          as ascii_ex,
    nullif(0, 0)                                        as nullif_ex,
    coalesce(null, null, 'fallback')                    as coalesce_ex;

-- ===========================================================================
-- B. Numeric / math functions  (sb.core)
-- ===========================================================================

select
    abs(-42)                         as abs_ex,
    sign(-99)                        as sign_ex,
    round(3.14159, 2)                as round_ex,
    trunc(3.99)                      as trunc_ex,
    floor(3.9)                       as floor_ex,
    ceil(3.1)                        as ceil_ex,
    mod(17, 5)                       as mod_ex,
    power(2, 10)                     as power_ex,
    sqrt(144)                        as sqrt_ex,
    ln(2.718281828)                  as ln_ex,
    log(10, 100)                     as log_base10_ex,
    log10(1000)                      as log10_ex,
    exp(1)                           as exp_ex,
    greatest(3, 7, 2, 9, 1)         as greatest_ex,
    least(3, 7, 2, 9, 1)            as least_ex;

-- ===========================================================================
-- C. Date / time / temporal functions  (sb.temporal)
-- ===========================================================================

select
    current_date                                              as today,
    current_time                                              as now_time,
    current_timestamp                                         as now_ts,
    localtime                                                 as local_time,
    localtimestamp                                            as local_ts,
    clock_timestamp()                                         as clock_ts,

    -- Part extraction
    date_part('year',  current_timestamp)                     as yr,
    date_part('month', current_timestamp)                     as mo,
    date_part('day',   current_timestamp)                     as dy,
    dow(current_date)                                         as day_of_week,
    doy(current_date)                                         as day_of_year,
    isodow(current_date)                                      as iso_dow,

    -- Truncation and construction
    date_trunc('month', current_timestamp)                    as month_start,
    make_date(2025, 6, 1)                                     as made_date,
    last_day(current_date)                                    as last_of_month,

    -- Arithmetic helpers
    add_months(current_date, 3)                               as plus_3_months,
    age(current_timestamp, cast('2020-01-01' as timestamptz)) as age_str,
    age_in_days(current_timestamp)                            as days_since_epoch,
    epoch(current_timestamp)                                  as unix_secs,
    from_unixtime(1704067200)                                 as from_unix,

    -- Day name
    day_name(current_date)                                    as weekday_name;

-- Temporal arithmetic on actual table data.
select c.customer_id,
       c.full_name,
       c.signup_date,
       app.days_since_signup(c.signup_date)                   as days_member,
       add_months(c.signup_date, 12)                          as first_anniversary
from app.sales.customers c
order by c.signup_date;

-- ===========================================================================
-- D. Aggregate functions  (sb.core)
-- ===========================================================================

select
    count(*)                            as total_orders,
    count(distinct customer_id)         as unique_customers,
    sum(total)                          as revenue,
    avg(total)                          as avg_order,
    min(total)                          as min_order,
    max(total)                          as max_order
from app.sales.orders
where status <> 'cancelled';

-- ===========================================================================
-- E. UUID functions  (sb.uuid)
-- ===========================================================================

select
    uuid_generate_v4()                                  as new_uuid_v4,
    uuid_nil()                                          as nil_uuid,
    uuid_generate_v1()                                  as new_uuid_v1,
    uuid_generate_v3(
      cast('6ba7b810-9dad-11d1-80b4-00c04fd430c8' as uuid),
      'example.com'
    )                                                   as det_uuid_v3,
    uuid_generate_v5(
      cast('6ba7b810-9dad-11d1-80b4-00c04fd430c8' as uuid),
      'example.com'
    )                                                   as det_uuid_v5;

-- ===========================================================================
-- F. JSON / document functions  (sb.json)
-- ===========================================================================

select
    json_value(
      cast('{"name":"Ada","score":99}' as document),
      '$.name'
    )                                                   as json_name,

    json_exists(
      cast('{"active":true}' as document),
      '$.active'
    )                                                   as field_exists,

    json_query(
      cast('{"items":[1,2,3]}' as document),
      '$.items'
    )                                                   as items_array,

    json_typeof(
      cast('{"x":1}' as document)
    )                                                   as json_type,

    json_build_object('product_id', 42, 'in_stock', true) as built_obj,

    json_build_array(1, 2, 'three', null)               as built_arr,

    json_array_length(cast('[10,20,30]' as document))   as arr_len;

-- JSON aggregate: build a JSON array of skus per in_stock status.
select in_stock,
       json_agg(sku order by sku) as sku_list
from app.catalog.products
group by in_stock;

-- ===========================================================================
-- G. Vector distance functions  (sb.vector)
-- ===========================================================================

select
    cosine_distance(
      vector('[1.0,0.0,0.0]'),
      vector('[0.0,1.0,0.0]')
    )                                   as cosine_dist,

    hamming_distance(
      vector('[1,0,1]'),
      vector('[1,1,0]')
    )                                   as hamming_dist;
