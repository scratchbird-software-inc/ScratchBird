-- 30_insert.sql
-- Populate example data.
-- Demonstrates:
--   single-row VALUES, multi-row VALUES,
--   INSERT ... SELECT, INSERT ... RETURNING.

begin transaction;

-- ---------------------------------------------------------------------------
-- Products (single-row inserts; NEXT VALUE FOR sequences not used here because
-- product_id is supplied explicitly to keep IDs predictable across runs)
-- ---------------------------------------------------------------------------
insert into app.catalog.products (product_id, sku, name, description, price, in_stock)
values (1, 'WIDGET-A', 'Widget Alpha',
        'Standard aluminium widget', 49.99, true);

insert into app.catalog.products (product_id, sku, name, description, price, in_stock)
values (2, 'WIDGET-B', 'Widget Beta',
        'Heavy-duty steel widget', 79.99, true);

insert into app.catalog.products (product_id, sku, name, description, price, in_stock)
values (3, 'GADGET-X', 'Gadget X',
        'Compact electronic gadget', 129.00, true);

insert into app.catalog.products (product_id, sku, name, description, price, in_stock)
values (4, 'GADGET-Y', 'Gadget Y',
        'Wireless gadget with charging case', 199.00, false);

insert into app.catalog.products (product_id, sku, name, description, price, in_stock)
values (5, 'CABLE-USB', 'USB Cable 2m',
        '2-metre braided USB cable', 9.99, true);

insert into app.catalog.products (product_id, sku, name, description, price, in_stock)
values (6, 'STAND-ADJ', 'Adjustable Stand',
        'Height-adjustable monitor stand', 59.50, true);

-- ---------------------------------------------------------------------------
-- Customers (multi-row VALUES insert)
-- ---------------------------------------------------------------------------
insert into app.sales.customers
    (customer_id, email, full_name, signup_date, loyalty_points)
values
    (next value for app.customer_id_seq, 'alice@example.com', 'Alice Nguyen',
     cast('2024-01-15' as date), 120),
    (next value for app.customer_id_seq, 'bob@example.org',   'Bob Schmidt',
     cast('2024-03-22' as date), 0),
    (next value for app.customer_id_seq, 'carol@example.net', 'Carol Okafor',
     cast('2024-06-01' as date), 500),
    (next value for app.customer_id_seq, 'dave@example.com',  'Dave Larsson',
     cast('2025-01-10' as date), 75),
    (next value for app.customer_id_seq, 'eve@example.io',    'Eve Tanaka',
     cast('2025-04-30' as date), 0);

-- ---------------------------------------------------------------------------
-- Orders (single-row inserts; RETURNING to capture generated timestamp)
-- ---------------------------------------------------------------------------
insert into app.sales.orders (order_id, customer_id, status, total)
values (next value for app.order_id_seq, 1, 'paid', 139.97)
returning order_id, order_ts;

insert into app.sales.orders (order_id, customer_id, status, total)
values (next value for app.order_id_seq, 1, 'shipped', 59.50)
returning order_id, order_ts;

insert into app.sales.orders (order_id, customer_id, status, total)
values (next value for app.order_id_seq, 2, 'open', 9.99)
returning order_id, order_ts;

insert into app.sales.orders (order_id, customer_id, status, total)
values (next value for app.order_id_seq, 3, 'paid', 199.00)
returning order_id, order_ts;

insert into app.sales.orders (order_id, customer_id, status, total)
values (next value for app.order_id_seq, 3, 'cancelled', 0)
returning order_id, order_ts;

insert into app.sales.orders (order_id, customer_id, status, total)
values (next value for app.order_id_seq, 4, 'open', 229.49)
returning order_id, order_ts;

-- ---------------------------------------------------------------------------
-- Order items (multi-row VALUES)
-- Order 1: WIDGET-A x2, CABLE-USB x1
-- Order 2: STAND-ADJ x1
-- Order 3: CABLE-USB x1
-- Order 4: GADGET-Y x1
-- Order 6: WIDGET-B x1, GADGET-X x1, CABLE-USB x1
-- ---------------------------------------------------------------------------
insert into app.sales.order_items (order_id, line_no, product_id, quantity, unit_price)
values
    (1, 1, 1, 2, 49.99),
    (1, 2, 5, 1,  9.99),
    (2, 1, 6, 1, 59.50),
    (3, 1, 5, 1,  9.99),
    (4, 1, 4, 1, 199.00),
    (6, 1, 2, 1, 79.99),
    (6, 2, 3, 1, 129.00),
    (6, 3, 5, 1,   9.99),
    (6, 4, 1, 1,  49.99);

-- Pad with a few extra lines to reach 12 total
insert into app.sales.order_items (order_id, line_no, product_id, quantity, unit_price)
values
    (1, 3, 6, 0 + 1, 59.50),
    (2, 2, 5, 1,      9.99),
    (6, 5, 6, 1,     59.50);

-- ---------------------------------------------------------------------------
-- Audit rows (INSERT ... SELECT from a constant expression rowset)
-- ---------------------------------------------------------------------------
insert into audit.change_log (event_id, table_name, action, actor)
select
    n,
    'app.sales.orders',
    'INSERT',
    'example_script'
from (
    values (101), (102), (103), (104), (105), (106)
) as t(n);

-- ---------------------------------------------------------------------------
-- Sensor readings (multi-row VALUES)
-- ---------------------------------------------------------------------------
insert into ts.sensor_reading (sensor_id, reading_ts, value)
values
    ('sensor-01', cast('2026-06-01 00:00:00+00' as timestamptz), 22.4),
    ('sensor-01', cast('2026-06-01 01:00:00+00' as timestamptz), 22.7),
    ('sensor-01', cast('2026-06-01 02:00:00+00' as timestamptz), 23.1),
    ('sensor-02', cast('2026-06-01 00:00:00+00' as timestamptz), 18.0),
    ('sensor-02', cast('2026-06-01 01:00:00+00' as timestamptz), 18.3),
    ('sensor-02', cast('2026-06-01 02:00:00+00' as timestamptz), 17.9);

commit;
