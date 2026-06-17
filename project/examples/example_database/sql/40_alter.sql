-- 40_alter.sql
-- ALTER TABLE, COMMENT ON, RENAME, and ALTER SEQUENCE / DOMAIN / VIEW.
-- Each statement targets objects created in the 10–15 files.

-- ---------------------------------------------------------------------------
-- ALTER TABLE: add a column, change a default, drop a column
-- ---------------------------------------------------------------------------

-- Add an optional notes column to orders
alter table app.sales.orders
    add column notes text;

-- Change the default status to 'open' (already the default; demonstrates syntax)
alter table app.sales.orders
    alter column status set default 'open';

-- Add a constraint: notes length must not exceed 1000 characters when present
alter table app.sales.orders
    add constraint orders_notes_length
    check (notes is null or char_length(notes) <= 1000);

-- Add a processed_at timestamp column to audit.change_log (nullable)
alter table audit.change_log
    add column processed_at timestamptz;

-- Drop the processed_at column (demonstrates DROP COLUMN)
alter table audit.change_log
    drop column processed_at;

-- ---------------------------------------------------------------------------
-- RENAME TABLE / RENAME COLUMN
-- ---------------------------------------------------------------------------

-- Rename a column in customers (reversible; renamed back below)
alter table app.sales.customers
    rename column external_ref to ext_system_ref;

-- Rename it back so the rest of the script stays consistent
alter table app.sales.customers
    rename column ext_system_ref to external_ref;

-- ---------------------------------------------------------------------------
-- COMMENT ON: update comments after schema evolution
-- ---------------------------------------------------------------------------

comment on table app.sales.orders is
    'Customer orders (amended: notes column added)';

comment on column app.sales.orders.notes is
    'Optional free-text notes on the order; max 1000 characters';

comment on table audit.change_log is
    'Append-only audit event log';

-- ---------------------------------------------------------------------------
-- ALTER SEQUENCE: change cache and add a restart
-- ---------------------------------------------------------------------------

alter sequence app.order_id_seq
    set cache 50
    set no cycle;

-- Restart example: only safe in a development/example context
alter sequence app.customer_id_seq
    restart with 100;

-- ---------------------------------------------------------------------------
-- ALTER DOMAIN: add a constraint to app.money (non-negative amounts)
-- ---------------------------------------------------------------------------

alter domain app.money
    add constraint money_nonnegative check (value >= 0);

-- ---------------------------------------------------------------------------
-- ALTER VIEW: set security invoker on the order_summary view
-- Confirmed in syntax_reference/view.md ALTER VIEW surface.
-- ---------------------------------------------------------------------------

alter view app.sales.order_summary
    set security invoker;

-- ---------------------------------------------------------------------------
-- RECREATE: replace the daily_revenue materialized view with a wider query
-- ---------------------------------------------------------------------------

recreate materialized view app.sales.daily_revenue as
select
    cast(o.order_ts as date)        as revenue_date,
    count(*)                        as order_count,
    sum(o.total)                    as daily_total,
    min(o.total)                    as min_order,
    max(o.total)                    as max_order
from app.sales.orders o
where o.status in ('paid', 'shipped')
group by cast(o.order_ts as date);

refresh materialized view app.sales.daily_revenue;
