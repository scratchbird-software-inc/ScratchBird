# Parser V3 Golden Fixtures

These fixtures are seed inputs for the initial parser vertical slice.

Current seed coverage:

- `SHOW VERSION` accepted public fixture with engine API bridge expectation.
- `SHOW DATABASE` accepted public fixture with engine API bridge expectation.
- `SHOW CLUSTER STATE` public safe refusal fixture.
- Valid public package manifest fixture.
- Invalid public package manifest with private authority leakage.

Fixtures are not runtime parser implementation. They are conformance inputs for the registry linter, fixture linter, parser generator skeleton, parser/binder/lowerer packets, and the minimal parser-to-engine API bridge.
