-- 13_tables.sql
-- Create all example tables with constraints, defaults, and comments.
-- Dependency order: products and customers first, orders next,
-- order_items last (references both orders and products).

-- ---------------------------------------------------------------------------
-- app.catalog.products
-- ---------------------------------------------------------------------------
create table app.catalog.products (
    product_id   bigint                not null,
    sku          text                  not null,
    name         text                  not null,
    description  text,
    price        app.money             not null default 0,
    in_stock     boolean               not null default true,
    attributes   document,
    created_at   timestamptz           not null default current_timestamp,
    primary key (product_id),
    unique (sku)
);

comment on table  app.catalog.products            is 'Product catalogue entries';
comment on column app.catalog.products.sku        is 'Human-readable product stock-keeping unit';
comment on column app.catalog.products.attributes is 'Flexible product attributes as a document';

-- ---------------------------------------------------------------------------
-- app.catalog.type_demo
-- Exercises the confirmed scalar, temporal, binary, UUID, document, and vector
-- type surface.  Types used:
--   int8, int16 (smallint), int32 (int), int64 (bigint), int128,
--   uint8, uint16, uint32, uint64, uint128,
--   numeric(14,2), real, double precision,
--   boolean, text, char(8), varchar(64),
--   date, time, timestamp, timestamptz, interval,
--   uuid, bytea, blob,
--   document, json_document, binary_json_document,
--   vector
-- ---------------------------------------------------------------------------
create table app.catalog.type_demo (
    id                   bigint               not null,
    col_int8             int8,
    col_int16            smallint,
    col_int32            int,
    col_int64            bigint,
    col_int128           int128,
    col_uint8            uint8,
    col_uint16           uint16,
    col_uint32           uint32,
    col_uint64           uint64,
    col_uint128          uint128,
    col_numeric          numeric(14, 2),
    col_real             real,
    col_double           double precision,
    col_boolean          boolean,
    col_text             text,
    col_char             char(8),
    col_varchar          varchar(64),
    col_date             date,
    col_time             time,
    col_timestamp        timestamp,
    col_timestamptz      timestamptz,
    col_interval         interval,
    col_uuid             uuid,
    col_bytea            bytea,
    col_blob             blob,
    col_document         document,
    col_json_document    json_document,
    col_binary_json      binary_json_document,
    col_vector           vector,
    primary key (id)
);

comment on table app.catalog.type_demo is 'One-column-per-type demonstration table';

-- ---------------------------------------------------------------------------
-- app.sales.customers
-- ---------------------------------------------------------------------------
create table app.sales.customers (
    customer_id    bigint           not null,
    email          app.email_addr   not null,
    full_name      text             not null,
    signup_date    date             not null,
    loyalty_points bigint           not null default 0,
    external_ref   uuid,
    primary key (customer_id),
    unique (email)
);

comment on table  app.sales.customers               is 'Registered customers';
comment on column app.sales.customers.loyalty_points is 'Accumulated loyalty points; non-negative';
comment on column app.sales.customers.external_ref   is 'Optional UUID reference to an external system';

-- ---------------------------------------------------------------------------
-- app.sales.orders
-- ---------------------------------------------------------------------------
create table app.sales.orders (
    order_id    bigint       not null,
    customer_id bigint       not null references app.sales.customers(customer_id),
    status      text         not null default 'open'
                             check (status in ('open', 'paid', 'shipped', 'cancelled')),
    order_ts    timestamptz  not null default current_timestamp,
    total       app.money    not null default 0,
    primary key (order_id)
);

comment on table  app.sales.orders         is 'Customer orders';
comment on column app.sales.orders.status  is 'Order lifecycle status';
comment on column app.sales.orders.total   is 'Order total; computed from line items';

-- ---------------------------------------------------------------------------
-- app.sales.order_items
-- ---------------------------------------------------------------------------
create table app.sales.order_items (
    order_id    bigint          not null references app.sales.orders(order_id),
    line_no     int             not null,
    product_id  bigint          not null references app.catalog.products(product_id),
    quantity    app.positive_qty not null,
    unit_price  app.money        not null,
    primary key (order_id, line_no)
);

comment on table app.sales.order_items is 'Individual line items within an order';

-- ---------------------------------------------------------------------------
-- audit.change_log
-- ---------------------------------------------------------------------------
create table audit.change_log (
    event_id    bigint      not null,
    event_ts    timestamptz not null default current_timestamp,
    table_name  text        not null,
    action      text        not null,
    actor       text,
    detail      document,
    primary key (event_id)
);

comment on table  audit.change_log            is 'Append-only change log for auditing';
comment on column audit.change_log.table_name is 'Name of the affected table';
comment on column audit.change_log.action     is 'INSERT, UPDATE, or DELETE';

-- ---------------------------------------------------------------------------
-- ts.sensor_reading
-- ---------------------------------------------------------------------------
create table ts.sensor_reading (
    sensor_id   text             not null,
    reading_ts  timestamptz      not null,
    value       double precision not null,
    primary key (sensor_id, reading_ts)
);

comment on table ts.sensor_reading is 'Time-series sensor readings keyed by sensor and timestamp';
