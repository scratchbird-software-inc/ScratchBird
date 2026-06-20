UPDATE orders
SET total_amount = total_amount * 0.95
WHERE customer_id IN (
    SELECT customer_id
    FROM customers
    WHERE account_balance > 10000
);
