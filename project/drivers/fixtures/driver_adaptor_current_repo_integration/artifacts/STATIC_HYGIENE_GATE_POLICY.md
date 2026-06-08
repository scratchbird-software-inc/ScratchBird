# Static Hygiene Gate Policy

Search key: `DRIVER_STATIC_HYGIENE_GATE_POLICY`

The static hygiene gate fails on:

- old external driver paths;
- legacy `tracks/p3` or `tracks/alpha` path assumptions;
- hardcoded user-local absolute paths;
- generated artifacts below `project/drivers/`;
- dependency trees below `project/drivers/`;
- CTest label drift from `CTEST_DRIVER_GATE_MATRIX.csv`;
- release documentation that claims support without passing evidence.

Historical preserved evidence may mention old paths only outside live
driver/adaptor/tool build and runtime code. Current benchmark and CTest scripts
must not use old paths.
