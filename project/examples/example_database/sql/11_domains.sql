-- 11_domains.sql
-- Create user-defined domains in the app schema.
-- Domains add constraint and null policy on top of carrier descriptors.

-- email_addr: text value that must contain '@'
create domain app.email_addr as text
    not null
    check (position('@' in value) > 0);

-- positive_qty: bigint that must be greater than zero
create domain app.positive_qty as bigint
    not null
    check (value > 0);

-- money: exact decimal with 14 digits total, 2 fractional
create domain app.money as numeric(14, 2);

comment on domain app.email_addr is 'Email address; must contain @';
comment on domain app.positive_qty is 'Quantity field; must be positive';
comment on domain app.money is 'Monetary amount with 2 decimal places';
