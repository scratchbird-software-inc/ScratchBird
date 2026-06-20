SELECT o.order_id, o.total_amount, c.country_code
FROM orders o
INNER JOIN customers c ON o.customer_id = c.customer_id
WHERE o.order_date >= TIMESTAMP '2023-01-01'
  AND o.total_amount > 1000
  AND c.country_code IN ('US', 'CA', 'MX');
