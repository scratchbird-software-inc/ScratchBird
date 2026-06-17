-- 32_queries.sql
-- SELECT examples exercising the confirmed SBsql query surface:
--   projections, WHERE, JOIN types, GROUP BY/HAVING,
--   ORDER BY/LIMIT/OFFSET, DISTINCT,
--   window functions, CTE, recursive CTE,
--   set operations (UNION / INTERSECT / EXCEPT),
--   and subqueries.

-- ---------------------------------------------------------------------------
-- 1. Simple projection with WHERE
-- ---------------------------------------------------------------------------
select product_id, sku, name, price
from app.catalog.products
where in_stock = true
order by price desc;

-- ---------------------------------------------------------------------------
-- 2. INNER JOIN: orders with customer name
-- ---------------------------------------------------------------------------
select o.order_id,
       c.full_name     as customer_name,
       o.status,
       o.total
from app.sales.orders o
inner join app.sales.customers c
    on c.customer_id = o.customer_id
where o.status <> 'cancelled'
order by o.order_id;

-- ---------------------------------------------------------------------------
-- 3. LEFT JOIN: customers with their order count (includes customers
--    who have placed no orders)
-- ---------------------------------------------------------------------------
select c.customer_id,
       c.full_name,
       count(o.order_id) as order_count
from app.sales.customers c
left join app.sales.orders o
    on o.customer_id = c.customer_id
group by c.customer_id, c.full_name
order by order_count desc, c.customer_id;

-- ---------------------------------------------------------------------------
-- 4. RIGHT JOIN: orders with optional product details via order_items
-- ---------------------------------------------------------------------------
select oi.order_id,
       oi.line_no,
       p.name      as product_name,
       oi.quantity,
       oi.unit_price
from app.catalog.products p
right join app.sales.order_items oi
    on oi.product_id = p.product_id
order by oi.order_id, oi.line_no;

-- ---------------------------------------------------------------------------
-- 5. FULL JOIN: all customers and all orders (unmatched rows from either side)
-- ---------------------------------------------------------------------------
select c.customer_id,
       c.full_name,
       o.order_id,
       o.status
from app.sales.customers c
full join app.sales.orders o
    on o.customer_id = c.customer_id
order by c.customer_id nulls last, o.order_id nulls last;

-- ---------------------------------------------------------------------------
-- 6. CROSS JOIN: each customer paired with each product (small tables)
-- ---------------------------------------------------------------------------
select c.full_name as customer_name,
       p.sku       as product_sku
from app.sales.customers c
cross join app.catalog.products p
where p.price < 15.00
order by c.customer_id, p.product_id;

-- ---------------------------------------------------------------------------
-- 7. GROUP BY / HAVING: customers with more than one order
-- ---------------------------------------------------------------------------
select c.customer_id,
       c.full_name,
       count(o.order_id)  as order_count,
       sum(o.total)       as lifetime_total
from app.sales.customers c
join app.sales.orders o
    on o.customer_id = c.customer_id
group by c.customer_id, c.full_name
having count(o.order_id) > 1
order by lifetime_total desc;

-- ---------------------------------------------------------------------------
-- 8. ORDER BY / LIMIT / OFFSET
-- ---------------------------------------------------------------------------
select product_id, name, price
from app.catalog.products
order by price desc
limit 3
offset 1;

-- ---------------------------------------------------------------------------
-- 9. DISTINCT: distinct statuses in the orders table
-- ---------------------------------------------------------------------------
select distinct status
from app.sales.orders
order by status;

-- ---------------------------------------------------------------------------
-- 10. Window functions: row_number, rank, running total
-- ---------------------------------------------------------------------------
select
    o.order_id,
    o.customer_id,
    o.total,
    row_number() over (
        partition by o.customer_id
        order by o.order_ts, o.order_id
    ) as customer_order_seq,
    rank() over (
        order by o.total desc
    ) as total_rank,
    sum(o.total) over (
        partition by o.customer_id
        order by o.order_ts, o.order_id
        rows between unbounded preceding and current row
    ) as running_customer_total
from app.sales.orders o
where o.status <> 'cancelled'
order by o.customer_id, customer_order_seq;

-- ---------------------------------------------------------------------------
-- 11. CTE: top-spending customers
-- ---------------------------------------------------------------------------
with customer_spend as (
    select
        c.customer_id,
        c.full_name,
        sum(o.total) as total_spend
    from app.sales.customers c
    join app.sales.orders o
        on o.customer_id = c.customer_id
    where o.status in ('paid', 'shipped')
    group by c.customer_id, c.full_name
)
select customer_id, full_name, total_spend
from customer_spend
where total_spend > 50
order by total_spend desc;

-- ---------------------------------------------------------------------------
-- 12. Recursive CTE: number series 1..5 (demonstrates RECURSIVE syntax)
-- ---------------------------------------------------------------------------
with recursive counter(n) as (
    select 1
    union all
    select n + 1
    from counter
    where n < 5
)
select n as series_value
from counter
order by n;

-- ---------------------------------------------------------------------------
-- 13. UNION: combine product names and customer names into one list
-- ---------------------------------------------------------------------------
select name   as label, 'product'  as kind from app.catalog.products
union
select full_name, 'customer' from app.sales.customers
order by label;

-- ---------------------------------------------------------------------------
-- 14. INTERSECT: sensor IDs that appear in both of two time windows
-- (illustrates INTERSECT with the same table, different predicates)
-- ---------------------------------------------------------------------------
select sensor_id
from ts.sensor_reading
where reading_ts < cast('2026-06-01 01:00:00+00' as timestamptz)
intersect
select sensor_id
from ts.sensor_reading
where reading_ts >= cast('2026-06-01 01:00:00+00' as timestamptz);

-- ---------------------------------------------------------------------------
-- 15. EXCEPT: products that have never appeared in any order item
-- ---------------------------------------------------------------------------
select product_id, name
from app.catalog.products
except
select p.product_id, p.name
from app.catalog.products p
join app.sales.order_items oi
    on oi.product_id = p.product_id
order by product_id;

-- ---------------------------------------------------------------------------
-- 16. Correlated subquery: orders whose total exceeds the customer average
-- ---------------------------------------------------------------------------
select o.order_id, o.customer_id, o.total
from app.sales.orders o
where o.total > (
    select avg(o2.total)
    from app.sales.orders o2
    where o2.customer_id = o.customer_id
)
order by o.customer_id, o.total desc;
