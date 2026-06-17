-- 90_teardown.sql
-- Drop all objects created by files 10–40, in reverse dependency order.
-- Security objects, triggers, and routines first (they depend on the tables),
-- then views and materialized views, then tables (child before parent),
-- then sequences, domains, principals, and finally schemas.

-- ---------------------------------------------------------------------------
-- Security objects (created in 23_security.sql)
-- ---------------------------------------------------------------------------
drop policy app.orders_writer_policy restrict;
drop rls    app.orders_active_rls    restrict;
drop mask   app.customer_email_mask  restrict;

-- ---------------------------------------------------------------------------
-- Triggers and event triggers (created in 22_triggers.sql)
-- ---------------------------------------------------------------------------
drop trigger app.sales.orders_ad_stmt   restrict;
drop trigger app.sales.order_items_ad    restrict;
drop trigger app.sales.orders_au         restrict;
drop trigger app.sales.orders_ai         restrict;
drop trigger app.sales.orders_bi         restrict;
drop event trigger audit.ddl_table_changes restrict;

-- ---------------------------------------------------------------------------
-- Procedures (created in 21_procedures.sql)
-- ---------------------------------------------------------------------------
drop procedure app.list_customer_orders  restrict;
drop procedure app.award_loyalty_points  restrict;
drop procedure app.recompute_order_total restrict;

-- ---------------------------------------------------------------------------
-- Functions (created in 20_functions.sql)
-- ---------------------------------------------------------------------------
drop function app.days_since_signup    restrict;
drop function app.format_customer_label restrict;
drop function app.order_line_total      restrict;

-- ---------------------------------------------------------------------------
-- Materialized views
-- ---------------------------------------------------------------------------
drop materialized view app.sales.daily_revenue restrict;

-- ---------------------------------------------------------------------------
-- Normal views
-- ---------------------------------------------------------------------------
drop view app.sales.order_summary restrict;

-- ---------------------------------------------------------------------------
-- Tables: children before parents
-- Indexes owned by these tables are retired with the table.
-- ---------------------------------------------------------------------------
drop table app.sales.order_items restrict;
drop table audit.change_log     restrict;
drop table app.sales.orders     restrict;
drop table app.sales.customers  restrict;
drop table app.catalog.type_demo restrict;
drop table app.catalog.products  restrict;
drop table ts.sensor_reading    restrict;

-- ---------------------------------------------------------------------------
-- Sequences
-- ---------------------------------------------------------------------------
drop sequence app.order_id_seq    restrict;
drop sequence app.customer_id_seq restrict;

-- ---------------------------------------------------------------------------
-- Domains
-- ---------------------------------------------------------------------------
drop domain app.money        restrict;
drop domain app.positive_qty restrict;
drop domain app.email_addr   restrict;

-- ---------------------------------------------------------------------------
-- Principals (created in 23_security.sql)
-- ---------------------------------------------------------------------------
drop group app_support restrict;
drop role  app_auditor restrict;
drop role  app_writer  restrict;
drop role  app_reader  restrict;

-- ---------------------------------------------------------------------------
-- Schemas: children before parents
-- ---------------------------------------------------------------------------
drop schema app.sales   restrict;
drop schema app.catalog restrict;
drop schema app          restrict;
drop schema audit        restrict;
drop schema ts           restrict;
