SELECT DATE_TRUNC('month', o.order_date) AS month,
       p.category,
       COUNT(DISTINCT o.order_id) AS total_orders,
       SUM(oi.quantity) AS total_qty,
       SUM(oi.quantity * oi.unit_price) AS total_revenue,
       AVG(oi.quantity * oi.unit_price) AS avg_line_value
FROM orders o
INNER JOIN order_items oi ON o.order_id = oi.order_id
INNER JOIN products p ON oi.product_id = p.product_id
GROUP BY DATE_TRUNC('month', o.order_date), p.category
HAVING COUNT(DISTINCT o.order_id) >= 10
ORDER BY month DESC, total_revenue DESC;
