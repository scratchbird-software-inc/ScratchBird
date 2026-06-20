SELECT c.customer_id, c.first_name, c.last_name,
       o.order_id, o.order_date, o.total_amount,
       oi.quantity, oi.unit_price,
       p.product_id, p.name AS product_name, p.category
FROM customers c
INNER JOIN orders o ON c.customer_id = o.customer_id
INNER JOIN order_items oi ON o.order_id = oi.order_id
INNER JOIN products p ON oi.product_id = p.product_id
WHERE o.order_date >= TIMESTAMP '2024-06-01';
