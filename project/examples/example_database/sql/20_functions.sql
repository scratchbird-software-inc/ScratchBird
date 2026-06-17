-- 20_functions.sql
-- User-defined scalar functions for the example ScratchBird database.
-- Demonstrates: SET TERM terminator change, parameters, local variables, RETURN.
-- Source authority: Language_Reference/syntax_reference/function.md,
--   procedural_sql_blocks.md, procedural_sql_control_flow.md.

SET TERM ^;

-- ---------------------------------------------------------------------------
-- 1. order_line_total(p_order_id bigint) returns app.money
--    Sums unit_price * quantity across order_items for one order.
--    Returns 0 when no line items exist.
-- ---------------------------------------------------------------------------
create function app.order_line_total(p_order_id bigint)
returns app.money
stable
called on null input
as
declare variable v_total app.money default 0;
begin
  select coalesce(sum(cast(quantity as numeric(14,2)) * unit_price), 0)
    from app.sales.order_items
   where order_id = p_order_id
    into v_total;

  return v_total;
end^

-- ---------------------------------------------------------------------------
-- 2. format_customer_label(p_full_name text, p_email app.email_addr)
--    returns text
--    Combines name and email into a display label like "Alice <alice@x.com>".
--    Returns email alone when name is null or empty.
-- ---------------------------------------------------------------------------
create function app.format_customer_label(
  p_full_name text,
  p_email     app.email_addr
)
returns text
deterministic
called on null input
as
declare variable v_label text default '';
begin
  if p_full_name is not null and char_length(p_full_name) > 0 then
  begin
    v_label = trim(p_full_name) || ' <' || p_email || '>';
  end
  else
  begin
    v_label = p_email;
  end

  return v_label;
end^

-- ---------------------------------------------------------------------------
-- 3. days_since_signup(p_signup_date date) returns bigint
--    Returns the number of complete days from signup_date to today.
-- ---------------------------------------------------------------------------
create function app.days_since_signup(p_signup_date date)
returns bigint
stable
returns null on null input
as
begin
  return cast(current_date - p_signup_date as bigint);
end^

SET TERM ;^
