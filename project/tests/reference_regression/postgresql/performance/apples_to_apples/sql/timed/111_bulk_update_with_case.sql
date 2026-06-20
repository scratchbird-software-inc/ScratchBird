UPDATE order_items
SET discount_pct = CASE
    WHEN quantity >= 50 THEN 20.0
    WHEN quantity >= 20 THEN 15.0
    WHEN quantity >= 10 THEN 10.0
    ELSE 5.0
END
WHERE discount_pct < 5.0 OR discount_pct IS NULL;
