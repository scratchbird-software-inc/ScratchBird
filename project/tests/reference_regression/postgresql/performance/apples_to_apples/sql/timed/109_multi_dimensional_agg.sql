SELECT EXTRACT(YEAR FROM o.order_date) AS order_year,
       EXTRACT(MONTH FROM o.order_date) AS order_month,
       c.country_code,
       p.category,
       COUNT(DISTINCT o.order_id) AS orders,
       SUM(oi.quantity) AS units,
       SUM(oi.quantity * oi.unit_price) AS revenue,
       AVG(oi.quantity * oi.unit_price) AS avg_line
FROM orders o
INNER JOIN customers c ON o.customer_id = c.customer_id
INNER JOIN order_items oi ON o.order_id = oi.order_id
INNER JOIN products p ON oi.product_id = p.product_id
GROUP BY EXTRACT(YEAR FROM o.order_date),
         EXTRACT(MONTH FROM o.order_date),
         c.country_code,
         p.category
ORDER BY order_year, order_month, revenue DESC;
