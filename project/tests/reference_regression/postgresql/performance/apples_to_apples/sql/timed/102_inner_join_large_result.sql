SELECT oi.*, o.order_date, o.status, c.first_name, c.last_name
FROM order_items oi
INNER JOIN orders o ON oi.order_id = o.order_id
INNER JOIN customers c ON o.customer_id = c.customer_id;
