INSERT INTO bulk_insert_test (id, data, metric_value)
SELECT seq AS id,
       'Data_' || CAST(seq AS VARCHAR(20)) AS data,
       (seq * 1.5) AS metric_value
FROM (
    SELECT ROW_NUMBER() OVER () AS seq
    FROM orders o
    CROSS JOIN order_items oi
    LIMIT :bulk_insert_rows
) sub;
