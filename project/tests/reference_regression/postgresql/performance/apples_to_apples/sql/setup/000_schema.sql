DROP SCHEMA IF EXISTS :"schema_name" CASCADE;
CREATE SCHEMA :"schema_name";
SET search_path TO :"schema_name";

CREATE TABLE customers (
    customer_id BIGINT PRIMARY KEY,
    first_name VARCHAR(50),
    last_name VARCHAR(50),
    email VARCHAR(100) UNIQUE,
    phone VARCHAR(20),
    registration_date DATE,
    country_code VARCHAR(2),
    account_balance NUMERIC(12, 2)
);

CREATE TABLE products (
    product_id BIGINT PRIMARY KEY,
    product_code VARCHAR(20) UNIQUE,
    name VARCHAR(200),
    category VARCHAR(50),
    price NUMERIC(10, 2),
    cost NUMERIC(10, 2),
    stock_quantity INTEGER,
    is_active INTEGER
);

CREATE TABLE orders (
    order_id BIGINT PRIMARY KEY,
    customer_id BIGINT,
    order_date TIMESTAMP,
    status VARCHAR(20),
    total_amount NUMERIC(12, 2),
    shipping_cost NUMERIC(8, 2),
    discount_amount NUMERIC(10, 2)
);

CREATE TABLE order_items (
    item_id BIGINT PRIMARY KEY,
    order_id BIGINT,
    product_id BIGINT,
    quantity INTEGER,
    unit_price NUMERIC(10, 2),
    discount_pct NUMERIC(5, 2)
);

CREATE TABLE bulk_insert_test (
    id BIGINT PRIMARY KEY,
    data VARCHAR(100),
    metric_value NUMERIC(10, 2)
);
