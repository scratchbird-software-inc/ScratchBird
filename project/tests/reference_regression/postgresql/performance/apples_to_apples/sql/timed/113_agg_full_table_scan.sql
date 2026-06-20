SELECT p.category,
       COUNT(*) AS item_count,
       SUM(oi.quantity) AS total_qty,
       AVG(oi.unit_price) AS avg_price,
       MIN(oi.unit_price) AS min_price,
       MAX(oi.unit_price) AS max_price,
       STDDEV(oi.unit_price) AS price_stddev
FROM order_items oi
INNER JOIN products p ON oi.product_id = p.product_id
GROUP BY p.category
HAVING COUNT(*) > 1000
ORDER BY item_count DESC;
