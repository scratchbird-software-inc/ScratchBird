-- 21_procedures.sql
-- Stored procedures for the example ScratchBird database.
-- Demonstrates: IF/ELSE, WHILE loop, FOR SELECT cursor, exception handling,
--   selectable procedure (RETURNS + SUSPEND), and OUT-variable pattern.
-- Source authority: Language_Reference/syntax_reference/procedure.md,
--   procedural_sql_blocks.md, procedural_sql_control_flow.md,
--   procedural_sql_cursors.md, procedural_sql_exceptions.md.

SET TERM ^;

-- ---------------------------------------------------------------------------
-- 1. app.recompute_order_total(p_order_id bigint)
--    Recalculates app.sales.orders.total from order_items, then updates the
--    orders row.  Demonstrates IF/ELSE, exception handling, and SELECT INTO.
-- ---------------------------------------------------------------------------
create procedure app.recompute_order_total(p_order_id bigint)
as
declare variable v_computed app.money default 0;
declare variable v_exists   int       default 0;
begin
  -- Verify the order exists before touching it.
  select count(*)
    from app.sales.orders
   where order_id = p_order_id
    into v_exists;

  if v_exists = 0 then
  begin
    signal sqlstate '45000'
      set message_text = 'recompute_order_total: order not found';
  end

  -- Sum line items.
  select coalesce(sum(cast(quantity as numeric(14,2)) * unit_price), 0)
    from app.sales.order_items
   where order_id = p_order_id
    into v_computed;

  -- Update the order total.
  begin
    update app.sales.orders
       set total = v_computed
     where order_id = p_order_id;

  exception
    when sqlstate '23000' do
    begin
      signal sqlstate '45000'
        set message_text = 'recompute_order_total: constraint violation updating total';
    end
  end
end^

-- ---------------------------------------------------------------------------
-- 2. app.award_loyalty_points(p_min_order_total app.money, p_points bigint)
--    Awards loyalty points to every customer whose most recent order exceeds
--    p_min_order_total.  Demonstrates FOR SELECT loop.
-- ---------------------------------------------------------------------------
create procedure app.award_loyalty_points(
  p_min_order_total app.money,
  p_points          bigint
)
as
declare variable v_customer_id bigint default 0;
declare variable v_order_total app.money default 0;
declare variable v_count       bigint default 0;
begin
  for
    select o.customer_id,
           o.total
      from app.sales.orders o
     where o.total >= p_min_order_total
       and o.status = 'shipped'
      into v_customer_id, v_order_total
  do
  begin
    update app.sales.customers
       set loyalty_points = loyalty_points + p_points
     where customer_id = v_customer_id;

    v_count = v_count + 1;
  end

  -- Intentionally no return value; caller can query the updated table.
end^

-- ---------------------------------------------------------------------------
-- 3. app.list_customer_orders(p_customer_id bigint)
--    Selectable procedure: yields one row per order for the given customer,
--    including the computed line total via app.order_line_total().
--    Demonstrates RETURNS descriptor, SUSPEND, and WHILE-style processing
--    using explicit cursor + LOOP.
-- ---------------------------------------------------------------------------
create procedure app.list_customer_orders(p_customer_id bigint)
returns (
  order_id   bigint,
  status     text,
  order_ts   timestamptz,
  line_total app.money
)
as
declare variable v_order_id bigint   default 0;
declare variable v_status   text     default '';
declare variable v_order_ts timestamptz default current_timestamp;
declare cursor c_orders for
  select o.order_id,
         o.status,
         o.order_ts
    from app.sales.orders o
   where o.customer_id = p_customer_id
   order by o.order_ts desc;
begin
  open c_orders;

  loop
    fetch c_orders into v_order_id, v_status, v_order_ts;
    if row_not_found() then
      leave;
    end if;

    order_id   = v_order_id;
    status     = v_status;
    order_ts   = v_order_ts;
    line_total = app.order_line_total(v_order_id);

    suspend;
  end loop;

  close c_orders;
end^

SET TERM ;^
