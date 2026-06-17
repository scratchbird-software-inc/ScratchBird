-- 15_views.sql
-- Normal view and materialized view over the sales schema.
-- CREATE MATERIALIZED VIEW and REFRESH MATERIALIZED VIEW are confirmed
-- in syntax_reference/view.md.

-- ---------------------------------------------------------------------------
-- Normal view: app.sales.order_summary
-- Joins orders, customers, and order_items; aggregates line count and total.
-- ---------------------------------------------------------------------------
create view app.sales.order_summary as
select
    o.order_id,
    o.status,
    o.order_ts,
    o.total                                       as order_total,
    c.customer_id,
    c.full_name                                   as customer_name,
    c.email                                       as customer_email,
    count(oi.line_no)                             as line_count,
    sum(cast(oi.quantity as numeric(14,2))
        * oi.unit_price)                          as computed_total
from app.sales.orders o
join app.sales.customers c
    on c.customer_id = o.customer_id
left join app.sales.order_items oi
    on oi.order_id = o.order_id
group by
    o.order_id,
    o.status,
    o.order_ts,
    o.total,
    c.customer_id,
    c.full_name,
    c.email;

comment on view app.sales.order_summary is 'Per-order summary with customer and line-item aggregates';

-- ---------------------------------------------------------------------------
-- Materialized view: app.sales.daily_revenue
-- Aggregates paid-order totals by calendar day.
-- ---------------------------------------------------------------------------
create materialized view app.sales.daily_revenue as
select
    cast(o.order_ts as date)  as revenue_date,
    count(*)                  as order_count,
    sum(o.total)              as daily_total
from app.sales.orders o
where o.status in ('paid', 'shipped')
group by cast(o.order_ts as date);

comment on materialized view app.sales.daily_revenue
    is 'Daily revenue rollup for paid and shipped orders';

-- Refresh the materialized view to populate it with committed data.
refresh materialized view app.sales.daily_revenue;
