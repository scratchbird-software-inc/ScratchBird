SELECT country_stats.country_code,
       country_stats.customer_count,
       country_stats.total_revenue,
       (SELECT AVG(country_revenue) FROM (
            SELECT SUM(total_amount) AS country_revenue
            FROM orders o2
            INNER JOIN customers c2 ON o2.customer_id = c2.customer_id
            GROUP BY c2.country_code
        ) sub2) AS global_avg,
       country_stats.total_revenue /
        (SELECT AVG(country_revenue) FROM (
            SELECT SUM(total_amount) AS country_revenue
            FROM orders o3
            INNER JOIN customers c3 ON o3.customer_id = c3.customer_id
            GROUP BY c3.country_code
        ) sub3) AS revenue_ratio
FROM (
    SELECT c.country_code,
           COUNT(DISTINCT c.customer_id) AS customer_count,
           SUM(o.total_amount) AS total_revenue
    FROM customers c
    INNER JOIN orders o ON c.customer_id = o.customer_id
    GROUP BY c.country_code
) country_stats
ORDER BY country_stats.total_revenue DESC;
