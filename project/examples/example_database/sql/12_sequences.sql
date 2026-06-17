-- 12_sequences.sql
-- Create sequences used for surrogate primary keys.

create sequence app.customer_id_seq
    as bigint
    start with 1
    increment by 1
    no cycle;

comment on sequence app.customer_id_seq is 'Customer surrogate key allocator';

create sequence app.order_id_seq
    as bigint
    start with 1
    increment by 1
    no cycle;

comment on sequence app.order_id_seq is 'Order surrogate key allocator';
