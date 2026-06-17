-- 10_schemas.sql
-- Create the schema tree for the example database.
-- Schemas: app, app.sales, app.catalog, audit, ts.
-- Each child schema is created after its parent is visible.

create schema app;
comment on schema app is 'Application root schema';

create schema app.sales;
comment on schema app.sales is 'Sales and order processing objects';

create schema app.catalog;
comment on schema app.catalog is 'Product catalog objects';

create schema audit;
comment on schema audit is 'Audit and change-log objects';

create schema ts;
comment on schema ts is 'Time-series sensor data objects';
