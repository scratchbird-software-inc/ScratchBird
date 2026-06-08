# sb_database_probe

Private create/open vertical-slice probe. It creates a database file, reopens the header, builds bootstrap catalog and transaction inventory placeholders, and executes engine-owned `SHOW VERSION` / `SHOW DATABASE` runtime calls.

The probe is not a parser and does not accept SQL.
