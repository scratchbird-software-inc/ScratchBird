SELECT c1.customer_id AS customer1_id,
       c1.first_name AS customer1_name,
       c2.customer_id AS customer2_id,
       c2.first_name AS customer2_name,
       c1.country_code
FROM customers c1
INNER JOIN customers c2 ON c1.country_code = c2.country_code
    AND c1.customer_id < c2.customer_id
WHERE c1.registration_date >= DATE '2024-01-01'
LIMIT 10000;
