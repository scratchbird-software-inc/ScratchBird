SELECT c.customer_id,
       c.first_name,
       o.order_id,
       o.total_amount,
       RANK() OVER (PARTITION BY c.customer_id ORDER BY o.total_amount DESC) AS amount_rank,
       COALESCE(SUM(o.total_amount) OVER (PARTITION BY c.customer_id), 0) AS customer_total
FROM customers c
INNER JOIN orders o ON c.customer_id = o.customer_id
WHERE o.order_date >= TIMESTAMP '2024-01-01';
