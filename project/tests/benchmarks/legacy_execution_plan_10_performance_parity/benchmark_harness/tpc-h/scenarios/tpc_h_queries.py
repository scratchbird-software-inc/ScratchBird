#!/usr/bin/env python3
"""Bounded TPC-H analytical query profile.

The profile keeps the canonical 22 query slots and names so benchmark output
cannot silently skip analytical coverage. Queries are bounded variants designed
to execute on every supported reference connection in this harness.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List


@dataclass
class TPC_HQuery:
    num: int
    name: str
    description: str
    sql: str


class TPCHQueries:
    """TPC-H analytical benchmark profile queries."""

    @staticmethod
    def get_all_queries() -> List[TPC_HQuery]:
        return [
            TPC_HQuery(
                1,
                "Pricing Summary Report",
                "Lineitem grouped pricing summary",
                """SELECT l_returnflag, l_linestatus, SUM(l_quantity) AS sum_qty,
                          SUM(l_extendedprice) AS sum_base_price, COUNT(*) AS count_order
                   FROM lineitem
                   GROUP BY l_returnflag, l_linestatus
                   ORDER BY l_returnflag, l_linestatus""",
            ),
            TPC_HQuery(
                2,
                "Minimum Cost Supplier",
                "Part/supplier cost lookup",
                """SELECT p_partkey, MIN(ps_supplycost) AS min_supplycost
                   FROM part, partsupp
                   WHERE p_partkey = ps_partkey
                   GROUP BY p_partkey
                   ORDER BY p_partkey""",
            ),
            TPC_HQuery(
                3,
                "Shipping Priority",
                "Customer/order/lineitem revenue by order",
                """SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue
                   FROM customer, orders, lineitem
                   WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey
                   GROUP BY l_orderkey
                   ORDER BY revenue DESC""",
            ),
            TPC_HQuery(
                4,
                "Order Priority Checking",
                "Order priority aggregation",
                """SELECT o_orderpriority, COUNT(*) AS order_count
                   FROM orders
                   GROUP BY o_orderpriority
                   ORDER BY o_orderpriority""",
            ),
            TPC_HQuery(
                5,
                "Local Supplier Volume",
                "Nation revenue through supplier/customer joins",
                """SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue
                   FROM customer, orders, lineitem, supplier, nation
                   WHERE c_custkey = o_custkey
                     AND l_orderkey = o_orderkey
                     AND l_suppkey = s_suppkey
                     AND c_nationkey = n_nationkey
                   GROUP BY n_name
                   ORDER BY revenue DESC""",
            ),
            TPC_HQuery(
                6,
                "Forecasting Revenue Change",
                "Discounted lineitem revenue",
                """SELECT SUM(l_extendedprice * l_discount) AS revenue
                   FROM lineitem
                   WHERE l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24""",
            ),
            TPC_HQuery(
                7,
                "Volume Shipping",
                "Supplier/customer nation volume",
                """SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation,
                          SUM(l_extendedprice * (1 - l_discount)) AS revenue
                   FROM supplier, lineitem, orders, customer, nation n1, nation n2
                   WHERE s_suppkey = l_suppkey
                     AND o_orderkey = l_orderkey
                     AND c_custkey = o_custkey
                     AND s_nationkey = n1.n_nationkey
                     AND c_nationkey = n2.n_nationkey
                   GROUP BY n1.n_name, n2.n_name
                   ORDER BY supp_nation, cust_nation""",
            ),
            TPC_HQuery(
                8,
                "National Market Share",
                "Regional market revenue",
                """SELECT r_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue
                   FROM part, lineitem, supplier, nation, region
                   WHERE p_partkey = l_partkey
                     AND s_suppkey = l_suppkey
                     AND s_nationkey = n_nationkey
                     AND n_regionkey = r_regionkey
                   GROUP BY r_name
                   ORDER BY r_name""",
            ),
            TPC_HQuery(
                9,
                "Product Type Profit",
                "Part profit by nation",
                """SELECT n_name, SUM(l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity) AS profit
                   FROM part, supplier, lineitem, partsupp, nation
                   WHERE p_partkey = l_partkey
                     AND s_suppkey = l_suppkey
                     AND ps_suppkey = l_suppkey
                     AND ps_partkey = l_partkey
                     AND s_nationkey = n_nationkey
                   GROUP BY n_name
                   ORDER BY n_name""",
            ),
            TPC_HQuery(
                10,
                "Returned Item Reporting",
                "Customer revenue for returned items",
                """SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue
                   FROM customer, orders, lineitem
                   WHERE c_custkey = o_custkey
                     AND o_orderkey = l_orderkey
                     AND l_returnflag = 'R'
                   GROUP BY c_custkey, c_name
                   ORDER BY revenue DESC""",
            ),
            TPC_HQuery(
                11,
                "Important Stock Identification",
                "Partsupp value by part",
                """SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS stock_value
                   FROM partsupp
                   GROUP BY ps_partkey
                   ORDER BY stock_value DESC""",
            ),
            TPC_HQuery(
                12,
                "Shipping Modes and Order Priority",
                "Shipping mode priority counts",
                """SELECT l_shipmode,
                          SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END) AS high_line_count,
                          SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH' THEN 1 ELSE 0 END) AS low_line_count
                   FROM orders, lineitem
                   WHERE o_orderkey = l_orderkey
                   GROUP BY l_shipmode
                   ORDER BY l_shipmode""",
            ),
            TPC_HQuery(
                13,
                "Customer Distribution",
                "Orders per customer distribution",
                """SELECT c_custkey, COUNT(o_orderkey) AS order_count
                   FROM customer
                   LEFT JOIN orders ON c_custkey = o_custkey
                   GROUP BY c_custkey
                   ORDER BY c_custkey""",
            ),
            TPC_HQuery(
                14,
                "Promotion Effect",
                "Promotion revenue percentage",
                """SELECT SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END) AS promo_revenue,
                          SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
                   FROM lineitem, part
                   WHERE l_partkey = p_partkey""",
            ),
            TPC_HQuery(
                15,
                "Top Supplier",
                "Supplier revenue ranking",
                """SELECT s_suppkey, s_name, SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
                   FROM supplier, lineitem
                   WHERE s_suppkey = l_suppkey
                   GROUP BY s_suppkey, s_name
                   ORDER BY total_revenue DESC""",
            ),
            TPC_HQuery(
                16,
                "Parts Supplier Relationship",
                "Supplier count by part brand/type",
                """SELECT p_brand, p_type, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
                   FROM part, partsupp
                   WHERE p_partkey = ps_partkey
                   GROUP BY p_brand, p_type
                   ORDER BY supplier_cnt DESC""",
            ),
            TPC_HQuery(
                17,
                "Small Quantity Order Revenue",
                "Average quantity revenue filter",
                """SELECT SUM(l_extendedprice) AS revenue
                   FROM lineitem
                   WHERE l_quantity < (SELECT AVG(l_quantity) FROM lineitem)""",
            ),
            TPC_HQuery(
                18,
                "Large Volume Customer",
                "Customer order quantity aggregation",
                """SELECT c_name, o_orderkey, SUM(l_quantity) AS total_quantity
                   FROM customer, orders, lineitem
                   WHERE c_custkey = o_custkey AND o_orderkey = l_orderkey
                   GROUP BY c_name, o_orderkey
                   ORDER BY total_quantity DESC""",
            ),
            TPC_HQuery(
                19,
                "Discounted Revenue",
                "Brand/container revenue filter",
                """SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue
                   FROM lineitem, part
                   WHERE p_partkey = l_partkey
                     AND p_brand IN ('Brand#12', 'Brand#23', 'Brand#34')""",
            ),
            TPC_HQuery(
                20,
                "Potential Part Promotion",
                "Supplier availability by nation",
                """SELECT s_name, s_address
                   FROM supplier, nation
                   WHERE s_nationkey = n_nationkey
                     AND s_suppkey IN (SELECT ps_suppkey FROM partsupp WHERE ps_availqty > 0)
                   ORDER BY s_name""",
            ),
            TPC_HQuery(
                21,
                "Suppliers Who Kept Orders Waiting",
                "Supplier late-line count",
                """SELECT s_name, COUNT(*) AS numwait
                   FROM supplier, lineitem
                   WHERE s_suppkey = l_suppkey
                     AND l_receiptdate > l_commitdate
                   GROUP BY s_name
                   ORDER BY numwait DESC""",
            ),
            TPC_HQuery(
                22,
                "Global Sales Opportunity",
                "Customer account balance by phone prefix",
                """SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode,
                          COUNT(*) AS numcust,
                          SUM(c_acctbal) AS total_acctbal
                   FROM customer
                   GROUP BY SUBSTRING(c_phone FROM 1 FOR 2)
                   ORDER BY cntrycode""",
            ),
        ]


def get_schema_sql() -> str:
    return """
    CREATE TABLE region (r_regionkey INT PRIMARY KEY, r_name CHAR(25), r_comment VARCHAR(152));
    CREATE TABLE nation (n_nationkey INT PRIMARY KEY, n_name CHAR(25), n_regionkey INT, n_comment VARCHAR(152));
    CREATE TABLE part (p_partkey INT PRIMARY KEY, p_name VARCHAR(55), p_mfgr CHAR(25), p_brand CHAR(10), p_type VARCHAR(25), p_size INT, p_container CHAR(10), p_retailprice DECIMAL(15,2), p_comment VARCHAR(23));
    CREATE TABLE supplier (s_suppkey INT PRIMARY KEY, s_name CHAR(25), s_address VARCHAR(40), s_nationkey INT, s_phone CHAR(15), s_acctbal DECIMAL(15,2), s_comment VARCHAR(101));
    CREATE TABLE partsupp (ps_partkey INT, ps_suppkey INT, ps_availqty INT, ps_supplycost DECIMAL(15,2), ps_comment VARCHAR(199), PRIMARY KEY (ps_partkey, ps_suppkey));
    CREATE TABLE customer (c_custkey INT PRIMARY KEY, c_name VARCHAR(25), c_address VARCHAR(40), c_nationkey INT, c_phone CHAR(15), c_acctbal DECIMAL(15,2), c_mktsegment CHAR(10), c_comment VARCHAR(117));
    CREATE TABLE orders (o_orderkey INT PRIMARY KEY, o_custkey INT, o_orderstatus CHAR(1), o_totalprice DECIMAL(15,2), o_orderdate DATE, o_orderpriority CHAR(15), o_clerk CHAR(15), o_shippriority INT, o_comment VARCHAR(79));
    CREATE TABLE lineitem (l_orderkey INT, l_partkey INT, l_suppkey INT, l_linenumber INT, l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), l_tax DECIMAL(15,2), l_returnflag CHAR(1), l_linestatus CHAR(1), l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE, l_shipinstruct CHAR(25), l_shipmode CHAR(10), l_comment VARCHAR(44), PRIMARY KEY (l_orderkey, l_linenumber));
    """


def get_seed_sql() -> List[str]:
    return [
        "INSERT INTO region (r_regionkey, r_name, r_comment) VALUES (1, 'AMERICA', 'region')",
        "INSERT INTO nation (n_nationkey, n_name, n_regionkey, n_comment) VALUES (1, 'CANADA', 1, 'nation')",
        "INSERT INTO part (p_partkey, p_name, p_mfgr, p_brand, p_type, p_size, p_container, p_retailprice, p_comment) VALUES (1, 'part', 'MFGR#1', 'Brand#12', 'PROMO BURNISHED', 1, 'SM BOX', 100.00, 'part')",
        "INSERT INTO supplier (s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, s_comment) VALUES (1, 'supplier', 'address', 1, '15-000-000-0000', 1000.00, 'supplier')",
        "INSERT INTO partsupp (ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, ps_comment) VALUES (1, 1, 100, 10.00, 'partsupp')",
        "INSERT INTO customer (c_custkey, c_name, c_address, c_nationkey, c_phone, c_acctbal, c_mktsegment, c_comment) VALUES (1, 'customer', 'address', 1, '15-000-000-0000', 500.00, 'BUILDING', 'customer')",
        "INSERT INTO orders (o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment) VALUES (1, 1, 'O', 100.00, DATE '1996-01-01', '1-URGENT', 'Clerk#1', 0, 'order')",
        "INSERT INTO lineitem (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment) VALUES (1, 1, 1, 1, 10.00, 100.00, 0.06, 0.02, 'R', 'O', DATE '1996-01-01', DATE '1996-01-02', DATE '1996-01-03', 'DELIVER IN PERSON', 'SHIP', 'lineitem')",
    ]


if __name__ == "__main__":
    queries = TPCHQueries.get_all_queries()
    print(f"TPC-H Queries: {len(queries)}")
    for q in queries:
        print(f"  Q{q.num}: {q.name}")
