-- 31_update_delete_merge.sql
-- UPDATE (with RETURNING, and UPDATE ... FROM),
-- DELETE (with RETURNING),
-- MERGE, and UPSERT.
-- Each block is an independent unit; wrap in explicit transactions.

-- ---------------------------------------------------------------------------
-- UPDATE: mark order 3 as shipped; return the new status
-- ---------------------------------------------------------------------------
begin transaction;

update app.sales.orders
   set status = 'shipped'
 where order_id = 3
returning order_id, status, order_ts;

commit;

-- ---------------------------------------------------------------------------
-- UPDATE ... FROM: copy the customer full_name into a scratch column.
-- (Demonstrates the update_from_clause; the join is customer -> order.)
-- We add 10 loyalty points to customers who have at least one paid order.
-- ---------------------------------------------------------------------------
begin transaction;

update app.sales.customers c
   set loyalty_points = c.loyalty_points + 10
  from app.sales.orders o
 where o.customer_id = c.customer_id
   and o.status = 'paid'
returning c.customer_id, c.loyalty_points;

commit;

-- ---------------------------------------------------------------------------
-- DELETE: remove cancelled orders and return their IDs
-- ---------------------------------------------------------------------------
begin transaction;

delete from app.sales.orders
 where status = 'cancelled'
returning order_id, status;

commit;

-- ---------------------------------------------------------------------------
-- MERGE: synchronise a product price update from a staging rowset.
-- The source is an inline VALUES subquery.
-- ---------------------------------------------------------------------------
begin transaction;

merge into app.catalog.products p
using (
    values
        (1, cast(54.99 as numeric(14,2))),
        (7, cast(19.99 as numeric(14,2)))
) as src(product_id, new_price)
   on src.product_id = p.product_id
when matched then
    update set price = src.new_price
when not matched then
    do nothing
returning p.product_id, p.price;

commit;

-- ---------------------------------------------------------------------------
-- UPSERT: insert a customer or update loyalty_points if they already exist.
-- ---------------------------------------------------------------------------
begin transaction;

upsert into app.sales.customers
    (customer_id, email, full_name, signup_date, loyalty_points)
values
    (6, 'frank@example.com', 'Frank Osei',
     cast('2026-01-01' as date), 0)
on conflict (customer_id) do update set
    loyalty_points = app.sales.customers.loyalty_points + 50
returning customer_id, email, loyalty_points;

commit;
