-- 22_triggers.sql
-- Triggers for the example ScratchBird database.
-- Demonstrates: BEFORE INSERT row trigger, AFTER INSERT/UPDATE/DELETE row
--   triggers writing to audit.change_log, a statement-level trigger, and an
--   event trigger on DDL.
-- Source authority: Language_Reference/syntax_reference/trigger.md,
--   procedural_sql_triggers_and_events.md, procedural_sql_blocks.md.
-- Transition values: new.* / old.* as documented in trigger.md.
-- audit.change_log schema: event_id bigint, event_ts timestamptz,
--   table_name text, action text, actor text, detail document.

SET TERM ^;

-- ---------------------------------------------------------------------------
-- 1. BEFORE INSERT on app.sales.orders
--    Ensures order_ts is set when caller omits it.
-- ---------------------------------------------------------------------------
create trigger app.sales.orders_bi
before insert on table app.sales.orders
for each row
when (new.order_ts is null)
as
begin
  new.order_ts = current_timestamp;
end^

-- ---------------------------------------------------------------------------
-- 2. AFTER INSERT on app.sales.orders
--    Logs each new order row to audit.change_log.
-- ---------------------------------------------------------------------------
create trigger app.sales.orders_ai
after insert on table app.sales.orders
for each row
as
declare variable v_event_id bigint default 0;
begin
  select coalesce(max(event_id), 0) + 1
    from audit.change_log
    into v_event_id;

  insert into audit.change_log (event_id, event_ts, table_name, action, actor, detail)
  values (
    v_event_id,
    current_timestamp,
    'app.sales.orders',
    'INSERT',
    current_user,
    cast('{"order_id":' || cast(new.order_id as text)
         || ',"status":"' || new.status || '"}' as document)
  );
end^

-- ---------------------------------------------------------------------------
-- 3. AFTER UPDATE on app.sales.orders
--    Logs status changes; fires only when status actually changes.
-- ---------------------------------------------------------------------------
create trigger app.sales.orders_au
after update of status on table app.sales.orders
for each row
when (old.status is distinct from new.status)
as
declare variable v_event_id bigint default 0;
begin
  select coalesce(max(event_id), 0) + 1
    from audit.change_log
    into v_event_id;

  insert into audit.change_log (event_id, event_ts, table_name, action, actor, detail)
  values (
    v_event_id,
    current_timestamp,
    'app.sales.orders',
    'UPDATE',
    current_user,
    cast('{"order_id":' || cast(new.order_id as text)
         || ',"old_status":"' || old.status
         || '","new_status":"' || new.status || '"}' as document)
  );
end^

-- ---------------------------------------------------------------------------
-- 4. AFTER DELETE on app.sales.order_items
--    Logs each removed line item to audit.change_log.
-- ---------------------------------------------------------------------------
create trigger app.sales.order_items_ad
after delete on table app.sales.order_items
for each row
as
declare variable v_event_id bigint default 0;
begin
  select coalesce(max(event_id), 0) + 1
    from audit.change_log
    into v_event_id;

  insert into audit.change_log (event_id, event_ts, table_name, action, actor, detail)
  values (
    v_event_id,
    current_timestamp,
    'app.sales.order_items',
    'DELETE',
    current_user,
    cast('{"order_id":' || cast(old.order_id as text)
         || ',"line_no":' || cast(old.line_no as text)
         || ',"product_id":' || cast(old.product_id as text) || '}' as document)
  );
end^

-- ---------------------------------------------------------------------------
-- 5. AFTER DELETE statement-level trigger on app.sales.orders
--    Logs a single summary audit row per bulk-delete statement.
-- ---------------------------------------------------------------------------
create trigger app.sales.orders_ad_stmt
after delete on table app.sales.orders
for each statement
as
declare variable v_event_id bigint default 0;
begin
  select coalesce(max(event_id), 0) + 1
    from audit.change_log
    into v_event_id;

  insert into audit.change_log (event_id, event_ts, table_name, action, actor, detail)
  values (
    v_event_id,
    current_timestamp,
    'app.sales.orders',
    'DELETE_STMT',
    current_user,
    cast('{"note":"statement-level delete audit"}' as document)
  );
end^

-- ---------------------------------------------------------------------------
-- 6. EVENT TRIGGER on ddl_command_end
--    Records CREATE/ALTER/DROP TABLE DDL activity to audit.change_log.
--    Uses the engine-provided event.* descriptor (tag, object_name,
--    principal_uuid) as documented in trigger.md event-trigger section.
-- ---------------------------------------------------------------------------
create event trigger audit.ddl_table_changes
on ddl_command_end
when event.tag in ('CREATE TABLE', 'ALTER TABLE', 'DROP TABLE')
as
declare variable v_event_id bigint default 0;
begin
  select coalesce(max(event_id), 0) + 1
    from audit.change_log
    into v_event_id;

  insert into audit.change_log (event_id, event_ts, table_name, action, actor, detail)
  values (
    v_event_id,
    current_timestamp,
    coalesce(event.object_name, '(unknown)'),
    event.tag,
    cast(event.principal_uuid as text),
    cast('{"ddl_event":true}' as document)
  );
end^

SET TERM ;^
