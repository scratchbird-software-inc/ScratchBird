-- 33_transactions.sql
-- Demonstrates the SBsql transaction control surface:
--   BEGIN TRANSACTION with isolation levels (READ COMMITTED, SNAPSHOT,
--   REPEATABLE READ, SERIALIZABLE), READ ONLY, READ WRITE,
--   SAVEPOINT, ROLLBACK TO SAVEPOINT, RELEASE SAVEPOINT,
--   COMMIT, and ROLLBACK.
-- Isolation levels are from syntax_reference/transaction_control.md.

-- ---------------------------------------------------------------------------
-- 1. READ COMMITTED transaction (DML committed normally)
-- ---------------------------------------------------------------------------
begin transaction
    isolation level read committed
    read write;

update app.sales.orders
   set status = 'paid'
 where order_id = 3
   and status = 'open';

insert into audit.change_log (event_id, table_name, action, actor)
values (200, 'app.sales.orders', 'UPDATE', 'txn_demo');

commit;

-- ---------------------------------------------------------------------------
-- 2. SNAPSHOT isolation: stable read view across multiple statements
-- ---------------------------------------------------------------------------
begin transaction
    isolation level snapshot
    read only;

select status, count(*) as n
from app.sales.orders
group by status
order by status;

select sensor_id, min(value) as min_val, max(value) as max_val
from ts.sensor_reading
group by sensor_id;

commit;

-- ---------------------------------------------------------------------------
-- 3. REPEATABLE READ: write transaction with a savepoint and partial rollback
-- ---------------------------------------------------------------------------
begin transaction
    isolation level repeatable read
    read write;

savepoint before_loyalty_update;

update app.sales.customers
   set loyalty_points = loyalty_points + 100
 where customer_id = 1;

-- Simulate discovering we should not apply the bonus; roll back to savepoint
rollback to savepoint before_loyalty_update;

-- Continue with a different, narrower update
update app.sales.customers
   set loyalty_points = loyalty_points + 10
 where customer_id = 1;

release savepoint before_loyalty_update;

insert into audit.change_log (event_id, table_name, action, actor)
values (201, 'app.sales.customers', 'UPDATE', 'loyalty_svc');

commit;

-- ---------------------------------------------------------------------------
-- 4. SERIALIZABLE: strongest isolation; rolled back to demonstrate rollback
-- ---------------------------------------------------------------------------
begin transaction
    isolation level serializable
    read write;

insert into audit.change_log (event_id, table_name, action, actor)
values (202, 'app.sales.orders', 'DELETE', 'cleanup_job');

-- Intentionally roll back to show rollback semantics
rollback;

-- ---------------------------------------------------------------------------
-- 5. Savepoint with nested partial rollback across multiple savepoints
-- ---------------------------------------------------------------------------
begin transaction;

savepoint sp_first;

insert into audit.change_log (event_id, table_name, action, actor)
values (210, 'ts.sensor_reading', 'INSERT', 'ingestion');

savepoint sp_second;

insert into audit.change_log (event_id, table_name, action, actor)
values (211, 'ts.sensor_reading', 'INSERT', 'ingestion');

-- Roll back only to sp_second; event 210 is preserved
rollback to sp_second;

insert into audit.change_log (event_id, table_name, action, actor)
values (212, 'ts.sensor_reading', 'INSERT', 'ingestion');

commit;
