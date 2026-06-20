SELECT COUNT(DISTINCT c.customer_id) AS unique_customers,
       COUNT(DISTINCT o.order_id) AS unique_orders,
       COUNT(DISTINCT p.product_id) AS unique_products,
       COUNT(DISTINCT c.country_code) AS countries,
       COUNT(DISTINCT p.category) AS categories
FROM orders o
INNER JOIN customers c ON o.customer_id = c.customer_id
INNER JOIN order_items oi ON o.order_id = oi.order_id
INNER JOIN products p ON oi.product_id = p.product_id;
