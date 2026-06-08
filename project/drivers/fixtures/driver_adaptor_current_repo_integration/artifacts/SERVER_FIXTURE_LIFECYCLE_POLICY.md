# Server Fixture Lifecycle Policy

Search key: `DRIVER_SERVER_FIXTURE_LIFECYCLE_POLICY`

Driver/adaptor/tool integration tests must use a CTest-managed current
`sb_server` fixture.

Required runtime root:

```text
build/drivers/_runtime/<ctest-label>/
```

Required runtime children:

- `control/`
- `runtime/`
- `logs/`
- `database/`
- `sockets/`

The fixture must:

- start only current-repo `sb_server`;
- use loopback or local IPC only;
- write PID, sockets, logs, and database files under `build/drivers/_runtime`;
- stop and cleanup on success or failure;
- preserve logs on failure;
- expose host, port, database, user, password, and protocol in a generated env
  file under `build/drivers/_runtime/<ctest-label>/fixture.env`.

No test may create runtime files in `project/drivers/`.
