# ScratchBird SQLAlchemy Dialect

Standalone SQLAlchemy dialect package for ScratchBird.

## Install (editable)

```bash
cd lanes/active/adapters/scratchbird-sqlalchemy-dialect
python3 -m pip install -e .
```

## Usage

```python
from sqlalchemy import create_engine, inspect

engine = create_engine(
    "scratchbird://user:pass@localhost:3092/mydb?sslmode=require&binaryTransfer=true"
)
inspector = inspect(engine)
print(inspector.get_schema_names())
```

## Current Scope

- SQLAlchemy dialect registration (`sqlalchemy.dialects` entry point)
- Inspector metadata support:
  - schemas
  - tables/views
  - columns (with reflection keys: `name`, `type`, `nullable`, `default`, `autoincrement`)
  - primary keys
  - foreign keys
  - indexes
- Schema-qualified reflection support
- ScratchBird baseline config guardrails (`sslmode=disable` and `binaryTransfer=false` rejected)
- Full canonical ScratchBird ingress/auth/bootstrap option normalization for
  the Python driver contract

## Tests

```bash
cd lanes/active/adapters/scratchbird-sqlalchemy-dialect
python3 -m pytest -q tests/test_dialect_contract.py
```

The tests are deterministic and run without requiring a live ScratchBird server.

## Example ORM flow

See `examples/orm_flow_example.py` for a reference lifecycle using mapped
entities and a transaction commit path.
