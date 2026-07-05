#!/usr/bin/env python3
"""
TPC-C Benchmark Workload

The industry standard OLTP benchmark with 5 transaction types:
- New-Order (45%)
- Payment (43%)
- Order-Status (4%)
- Delivery (4%)
- Stock-Level (4%)
"""

from dataclasses import dataclass
from typing import List, Dict

@dataclass
class TPCCTransaction:
    name: str
    weight: int  # Percentage
    description: str
    sql_statements: List[str]

class TPCCWorkload:
    """TPC-C benchmark transactions."""

    @staticmethod
    def get_new_order() -> TPCCTransaction:
        return TPCCTransaction(
            name="New-Order",
            weight=45,
            description="Place a new order with multiple items",
            sql_statements=[
                "SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse WHERE w_id = :w_id AND c_w_id = w_id AND c_d_id = :d_id AND c_id = :c_id",
                "SELECT d_next_o_id, d_tax FROM district WHERE d_id = :d_id AND d_w_id = :w_id",
                "UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = :d_id AND d_w_id = :w_id",
                "INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) VALUES (:o_id, :d_id, :w_id, :c_id, NOW(), :ol_cnt, :all_local)",
                "INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES (:o_id, :d_id, :w_id)",
                # Loop for each item
                "SELECT i_price, i_name, i_data FROM item WHERE i_id = :i_id",
                "SELECT s_quantity, s_data, s_dist_01 FROM stock WHERE s_i_id = :i_id AND s_w_id = :w_id",
                "UPDATE stock SET s_quantity = s_quantity - :qty WHERE s_i_id = :i_id AND s_w_id = :w_id",
                "INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES (:o_id, :d_id, :w_id, :ol_number, :i_id, :supply_w_id, :qty, :amount, :dist_info)",
            ]
        )

    @staticmethod
    def get_payment() -> TPCCTransaction:
        return TPCCTransaction(
            name="Payment",
            weight=43,
            description="Process a payment",
            sql_statements=[
                "UPDATE warehouse SET w_ytd = w_ytd + :amount WHERE w_id = :w_id",
                "SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name FROM warehouse WHERE w_id = :w_id",
                "UPDATE district SET d_ytd = d_ytd + :amount WHERE d_id = :d_id AND d_w_id = :w_id",
                "SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name FROM district WHERE d_id = :d_id AND d_w_id = :w_id",
                "SELECT c_id FROM customer WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_last = :last ORDER BY c_first",
                "SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_since FROM customer WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_id = :c_id",
                "UPDATE customer SET c_balance = c_balance - :amount, c_ytd_payment = c_ytd_payment + 1 WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_id = :c_id",
                "INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data) VALUES (:c_id, :d_id, :w_id, :d_id, :w_id, NOW(), :amount, :data)",
            ]
        )

    @staticmethod
    def get_order_status() -> TPCCTransaction:
        return TPCCTransaction(
            name="Order-Status",
            weight=4,
            description="Query order status",
            sql_statements=[
                "SELECT c_id FROM customer WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_last = :last ORDER BY c_first",
                "SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_id = :c_id",
                "SELECT o_id, o_entry_d, o_carrier_id FROM orders WHERE o_w_id = :w_id AND o_d_id = :d_id AND o_c_id = :c_id ORDER BY o_id DESC LIMIT 1",
                "SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_w_id = :w_id AND ol_d_id = :d_id AND ol_o_id = :o_id",
            ]
        )

    @staticmethod
    def get_delivery() -> TPCCTransaction:
        return TPCCTransaction(
            name="Delivery",
            weight=4,
            description="Deliver orders",
            sql_statements=[
                # Loop for each district
                "SELECT no_o_id FROM new_order WHERE no_d_id = :d_id AND no_w_id = :w_id ORDER BY no_o_id LIMIT 1",
                "DELETE FROM new_order WHERE no_d_id = :d_id AND no_w_id = :w_id AND no_o_id = :o_id",
                "SELECT o_c_id FROM orders WHERE o_id = :o_id AND o_d_id = :d_id AND o_w_id = :w_id",
                "UPDATE orders SET o_carrier_id = :carrier_id WHERE o_id = :o_id AND o_d_id = :d_id AND o_w_id = :w_id",
                "UPDATE order_line SET ol_delivery_d = NOW() WHERE ol_o_id = :o_id AND ol_d_id = :d_id AND ol_w_id = :w_id",
                "SELECT SUM(ol_amount) FROM order_line WHERE ol_o_id = :o_id AND ol_d_id = :d_id AND ol_w_id = :w_id",
                "UPDATE customer SET c_balance = c_balance + :total_amount WHERE c_id = :c_id AND c_d_id = :d_id AND c_w_id = :w_id",
            ]
        )

    @staticmethod
    def get_stock_level() -> TPCCTransaction:
        return TPCCTransaction(
            name="Stock-Level",
            weight=4,
            description="Check stock level",
            sql_statements=[
                "SELECT d_next_o_id FROM district WHERE d_id = :d_id AND d_w_id = :w_id",
                "SELECT COUNT(DISTINCT s_i_id) FROM order_line, stock WHERE ol_w_id = :w_id AND ol_d_id = :d_id AND ol_o_id BETWEEN :o_id_low AND :o_id_high AND s_w_id = :w_id AND s_i_id = ol_i_id AND s_quantity < :threshold",
            ]
        )

    @staticmethod
    def get_all_transactions() -> List[TPCCTransaction]:
        return [
            TPCCWorkload.get_new_order(),
            TPCCWorkload.get_payment(),
            TPCCWorkload.get_order_status(),
            TPCCWorkload.get_delivery(),
            TPCCWorkload.get_stock_level(),
        ]


def get_schema_sql() -> str:
    """Get TPC-C schema creation SQL."""
    return """
    -- Warehouse
    CREATE TABLE warehouse (
        w_id INT PRIMARY KEY,
        w_name VARCHAR(10),
        w_street_1 VARCHAR(20),
        w_street_2 VARCHAR(20),
        w_city VARCHAR(20),
        w_state CHAR(2),
        w_zip CHAR(9),
        w_tax DECIMAL(4,4),
        w_ytd DECIMAL(12,2)
    );

    -- District
    CREATE TABLE district (
        d_id INT,
        d_w_id INT,
        d_name VARCHAR(10),
        d_street_1 VARCHAR(20),
        d_street_2 VARCHAR(20),
        d_city VARCHAR(20),
        d_state CHAR(2),
        d_zip CHAR(9),
        d_tax DECIMAL(4,4),
        d_ytd DECIMAL(12,2),
        d_next_o_id INT,
        PRIMARY KEY (d_id, d_w_id)
    );

    -- Customer
    CREATE TABLE customer (
        c_id INT,
        c_d_id INT,
        c_w_id INT,
        c_first VARCHAR(16),
        c_middle CHAR(2),
        c_last VARCHAR(16),
        c_street_1 VARCHAR(20),
        c_street_2 VARCHAR(20),
        c_city VARCHAR(20),
        c_state CHAR(2),
        c_zip CHAR(9),
        c_phone CHAR(16),
        c_since TIMESTAMP,
        c_credit CHAR(2),
        c_credit_lim DECIMAL(12,2),
        c_discount DECIMAL(4,4),
        c_balance DECIMAL(12,2),
        c_ytd_payment DECIMAL(12,2),
        c_payment_cnt INT,
        c_delivery_cnt INT,
        c_data VARCHAR(500),
        PRIMARY KEY (c_id, c_d_id, c_w_id)
    );

    -- Orders
    CREATE TABLE orders (
        o_id INT,
        o_d_id INT,
        o_w_id INT,
        o_c_id INT,
        o_entry_d TIMESTAMP,
        o_carrier_id INT,
        o_ol_cnt INT,
        o_all_local INT,
        PRIMARY KEY (o_id, o_d_id, o_w_id)
    );

    -- Order Line
    CREATE TABLE order_line (
        ol_o_id INT,
        ol_d_id INT,
        ol_w_id INT,
        ol_number INT,
        ol_i_id INT,
        ol_supply_w_id INT,
        ol_delivery_d TIMESTAMP,
        ol_quantity INT,
        ol_amount DECIMAL(6,2),
        ol_dist_info CHAR(24),
        PRIMARY KEY (ol_o_id, ol_d_id, ol_w_id, ol_number)
    );

    -- New Order
    CREATE TABLE new_order (
        no_o_id INT,
        no_d_id INT,
        no_w_id INT,
        PRIMARY KEY (no_o_id, no_d_id, no_w_id)
    );

    -- Item
    CREATE TABLE item (
        i_id INT PRIMARY KEY,
        i_im_id INT,
        i_name VARCHAR(24),
        i_price DECIMAL(5,2),
        i_data VARCHAR(50)
    );

    -- Stock
    CREATE TABLE stock (
        s_i_id INT,
        s_w_id INT,
        s_quantity INT,
        s_dist_01 CHAR(24),
        s_dist_02 CHAR(24),
        s_dist_03 CHAR(24),
        s_dist_04 CHAR(24),
        s_dist_05 CHAR(24),
        s_dist_06 CHAR(24),
        s_dist_07 CHAR(24),
        s_dist_08 CHAR(24),
        s_dist_09 CHAR(24),
        s_dist_10 CHAR(24),
        s_ytd INT,
        s_order_cnt INT,
        s_remote_cnt INT,
        s_data VARCHAR(50),
        PRIMARY KEY (s_i_id, s_w_id)
    );

    -- History
    CREATE TABLE history (
        h_c_id INT,
        h_c_d_id INT,
        h_c_w_id INT,
        h_d_id INT,
        h_w_id INT,
        h_date TIMESTAMP,
        h_amount DECIMAL(6,2),
        h_data VARCHAR(24)
    );
    """


if __name__ == '__main__':
    transactions = TPCCWorkload.get_all_transactions()
    print(f"TPC-C Transactions:")
    for tx in transactions:
        print(f"  - {tx.name} ({tx.weight}%): {len(tx.sql_statements)} SQL statements")
