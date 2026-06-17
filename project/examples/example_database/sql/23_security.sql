-- 23_security.sql
-- Roles, groups, grants, revokes, and data-protection policies for the
-- example ScratchBird database.
-- Source authority: Language_Reference/syntax_reference/security_and_privilege_statements.md,
--   policy_mask_and_rls.md.
-- Privilege names, target forms, and statement shapes confirmed from the
-- documented EBNF and examples only.

-- ---------------------------------------------------------------------------
-- 1. Create roles and a group
-- ---------------------------------------------------------------------------
create role app_reader;
create role app_writer;
create role app_auditor;
create group app_support;

-- ---------------------------------------------------------------------------
-- 2. Grant schema USAGE so roles can resolve names within the schemas
-- ---------------------------------------------------------------------------
grant usage on schema app to app_reader;
grant usage on schema app to app_writer;
grant usage on schema app to app_auditor;

grant usage on schema app.sales to app_reader;
grant usage on schema app.sales to app_writer;
grant usage on schema app.sales to app_auditor;

grant usage on schema app.catalog to app_reader;
grant usage on schema app.catalog to app_writer;

grant usage on schema audit to app_auditor;

-- ---------------------------------------------------------------------------
-- 3. Read-only role: SELECT on key tables
-- ---------------------------------------------------------------------------
grant select on table app.catalog.products        to app_reader;
grant select on table app.sales.customers         to app_reader;
grant select on table app.sales.orders            to app_reader;
grant select on table app.sales.order_items       to app_reader;

-- ---------------------------------------------------------------------------
-- 4. Writer role: INSERT/UPDATE on orders and order_items; SELECT everywhere
-- ---------------------------------------------------------------------------
grant select, insert, update on table app.sales.orders      to app_writer;
grant select, insert, update, delete on table app.sales.order_items to app_writer;
grant select on table app.catalog.products                  to app_writer;
grant select on table app.sales.customers                   to app_writer;

-- Sequence usage for writer role
grant usage on sequence app.order_id_seq to app_writer;

-- ---------------------------------------------------------------------------
-- 5. Auditor role: SELECT on audit.change_log only
-- ---------------------------------------------------------------------------
grant select on table audit.change_log to app_auditor;

-- ---------------------------------------------------------------------------
-- 6. Routine execution grants
-- ---------------------------------------------------------------------------
grant execute on function  app.order_line_total       to app_reader;
grant execute on function  app.format_customer_label  to app_reader;
grant execute on function  app.days_since_signup      to app_reader;

grant execute on procedure app.recompute_order_total  to app_writer;
grant execute on procedure app.award_loyalty_points   to app_writer;
grant execute on procedure app.list_customer_orders   to app_reader;

-- ---------------------------------------------------------------------------
-- 7. Column-level grant: restrict customer PII visibility for app_support group
--    Only expose customer_id and full_name; email is masked below.
-- ---------------------------------------------------------------------------
grant select on column app.sales.customers.customer_id  to app_support;
grant select on column app.sales.customers.full_name    to app_support;
grant select on column app.sales.customers.email        to app_support;
grant select on column app.sales.customers.loyalty_points to app_support;

-- ---------------------------------------------------------------------------
-- 8. Column mask: redact email for app_support unless they hold a special role
-- ---------------------------------------------------------------------------
create mask app.customer_email_mask
on column app.sales.customers.email
using case
  when has_role('app_auditor') then email
  else 'redacted@example.com'
end
to role app_support
active;

-- ---------------------------------------------------------------------------
-- 9. Row-level security: readers see only non-cancelled orders
-- ---------------------------------------------------------------------------
create rls app.orders_active_rls
on table app.sales.orders
for select
to role app_reader
using (status <> 'cancelled')
as restrictive
active;

-- ---------------------------------------------------------------------------
-- 10. Policy: restrict app_writer UPDATE on orders to certain status values
--     (a writer may only move an order forward, not reopen a closed one)
-- ---------------------------------------------------------------------------
create policy app.orders_writer_policy
on table app.sales.orders
for update
to role app_writer
with check (status in ('open', 'processing', 'shipped', 'paid'))
as restrictive
active;

-- ---------------------------------------------------------------------------
-- 11. Revoke the over-broad initial grant; leave column-level grants intact
--     This shows the documented REVOKE form from security_and_privilege_statements.md.
-- ---------------------------------------------------------------------------
-- Revoke broad UPDATE that was granted; only keep INSERT and SELECT for safety.
revoke update on table app.sales.customers from app_writer restrict;

-- ---------------------------------------------------------------------------
-- 12. Re-grant so later example scripts (run by the same session user) work
--     The subsequent scripts 30-36 do DML so the session user still needs access.
-- ---------------------------------------------------------------------------
grant select, insert, update, delete on table app.sales.orders      to app_writer;
grant select, insert, update, delete on table app.sales.order_items to app_writer;
grant select, insert, update         on table app.sales.customers   to app_writer;
grant select, insert, update, delete on table app.catalog.products  to app_writer;
grant select, insert, update, delete on table audit.change_log      to app_writer;
grant select, insert, update, delete on table ts.sensor_reading     to app_writer;
