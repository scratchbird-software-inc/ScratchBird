-- 14_indexes.sql
-- Explicit indexes on the example tables.
-- Index families used: btree (default), unique_btree, full_text, vector_hnsw.
-- Sources: syntax_reference/index.md family table.

-- B-tree index: orders by customer_id (supports FK lookups and joins)
create index app.sales.orders_customer_id_idx
    on app.sales.orders (customer_id);

-- B-tree index: products by name (supports name prefix search)
create index app.catalog.products_name_idx
    on app.catalog.products (name);

-- B-tree index: customers by signup_date descending (recent-first reporting)
create index app.sales.customers_signup_date_idx
    on app.sales.customers (signup_date desc);

-- B-tree index: order_items by product_id (supports FK lookups)
create index app.sales.order_items_product_id_idx
    on app.sales.order_items (product_id);

-- Unique index: customers by email (reinforces the UNIQUE constraint)
create unique index app.sales.customers_email_uidx
    on app.sales.customers (email);

-- B-tree index: sensor readings by sensor and time window (time-series queries)
create index ts.sensor_reading_sensor_ts_idx
    on ts.sensor_reading (sensor_id, reading_ts desc);

-- B-tree index: audit change_log by table_name and event_ts
create index audit.change_log_table_ts_idx
    on audit.change_log (table_name, event_ts desc);

-- Full-text index on products.name (family: full_text)
create index app.catalog.products_name_fts_idx
    on app.catalog.products (name)
    using full_text;

-- Vector ANN index on type_demo.col_vector (family: vector_hnsw)
-- Requires an approximate-rerank proof before final row delivery.
create index app.catalog.type_demo_vector_idx
    on app.catalog.type_demo (col_vector)
    using vector_hnsw;
