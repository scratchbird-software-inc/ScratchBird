CREATE INDEX idx_stress_customers_country_customer ON customers (country_code, customer_id);
CREATE INDEX idx_stress_customers_registration ON customers (registration_date);
CREATE INDEX idx_stress_customers_balance ON customers (account_balance);
CREATE INDEX idx_stress_orders_customer_date ON orders (customer_id, order_date);
CREATE INDEX idx_stress_orders_order_date ON orders (order_date);
CREATE INDEX idx_stress_orders_total_amount ON orders (total_amount);
CREATE INDEX idx_stress_order_items_order_id ON order_items (order_id);
CREATE INDEX idx_stress_order_items_product_id ON order_items (product_id);
CREATE INDEX idx_stress_products_category ON products (category);

ANALYZE customers;
ANALYZE products;
ANALYZE orders;
ANALYZE order_items;
ANALYZE bulk_insert_test;
