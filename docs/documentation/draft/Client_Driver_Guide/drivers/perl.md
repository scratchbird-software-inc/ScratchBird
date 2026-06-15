# ScratchBird Perl Driver — DBI / DBD Binding

> **Status: beta\_2 / release\_candidate (draft stub)** — The Perl driver
> source tree contains only a `package_contract.json` at the time of this
> writing. No `README.md`, `Makefile.PL`, source files, or implementation docs
> are present in the source tree. This page documents what is verifiable from
> the manifest and the package contract; all API examples below are drawn
> exclusively from those sources. The driver is not yet usable for application
> development. This page will be updated when source is committed.

## Purpose

The Perl driver is intended to provide ScratchBird connectivity via the
standard Perl DBI interface using a `DBD::ScratchBird` driver module. DBI is
the de-facto Perl database abstraction layer; a DBD module implements the
database-specific backend. This driver exposes standard DBI handle methods
including `prepare`, `execute`, `fetchrow_arrayref`, `commit`, and `rollback`,
plus the DBI metadata methods `table_info` and `column_info`.

Target audience: Perl developers using DBI-based applications (web apps,
ETL pipelines, data scripts) who need ScratchBird connectivity.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0028-7000-8000-000000000028`     |
| `driver_family`          | `perl`                                     |
| `api_surface_set`        | `perl_dbi`                                 |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `connection_thread_confined`               |
| `pooling_capability`     | `connection_pool`                          |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_perl_gate`                         |

> **Thread safety:** The Perl driver is `connection_thread_confined`. Each DBI
> connection handle must be used from the thread that created it. Do not share
> connection handles across threads; create a separate handle per thread.

## Public API Surface (from `package_contract.json`)

The following DBI/DBD entry points are declared in the contract:

| Symbol                  | DBI role                                  |
|-------------------------|-------------------------------------------|
| `DBD::ScratchBird`      | Driver module name (for `DBI->connect`)   |
| `DBI->connect`          | Open a database handle (`$dbh`)           |
| `prepare`               | Prepare a statement, return `$sth`        |
| `execute`               | Execute a prepared statement handle       |
| `fetchrow_arrayref`     | Fetch next row as array reference         |
| `commit`                | Commit current transaction                |
| `rollback`              | Roll back current transaction             |
| `table_info`            | DBI metadata: list tables                 |
| `column_info`           | DBI metadata: list columns                |

## Install

Not yet available. When published, the expected CPAN form is:

```bash
# Via cpanm (anticipated; not yet verified)
cpanm DBD::ScratchBird
```

Or from source:

```bash
perl Makefile.PL
make
make test
make install
```

> No `Makefile.PL` or `META.json` is present in the source tree, so the
> package name and CPAN namespace are unconfirmed.

## Connecting

The DSN form follows standard DBI conventions with the ScratchBird URL embedded
or as colon-separated DBI DSN attributes:

```perl
# URL form (anticipated)
my $dbh = DBI->connect(
    "dbi:ScratchBird:database=mydb;host=localhost;port=3092",
    "user",
    "password",
    { AutoCommit => 0, RaiseError => 1 }
);
```

The `package_contract.json` declares `database`, `host`, `port`, `user`, and
`auth_method` as DSN keys. See ../connection\_and\_dsn.md for the full key
reference.

> No source files are present to confirm the exact DBI DSN attribute names or
> which `$dbh` attributes are accepted. The above is illustrative only.

## Executing Statements and Transactions

Anticipated DBI pattern:

```perl
# Prepare and execute
my $sth = $dbh->prepare("SELECT id, name FROM users WHERE active = ?");
$sth->execute(1);
while (my $row = $sth->fetchrow_arrayref) {
    print "$row->[0]  $row->[1]\n";
}

# Transaction
$dbh->{AutoCommit} = 0;
eval {
    $dbh->do("UPDATE counters SET n = n + 1 WHERE id = 1");
    $dbh->commit;
};
if ($@) {
    $dbh->rollback;
    die $@;
}
```

Transaction semantics conform to the MGA engine contract (source:
`package_contract.json` — `mga_transaction_finality`):

- sessions are always in a transaction
- `commit` / `rollback` reopen the next boundary
- no hidden replay of abandoned in-flight transactions

## Metadata

DBI metadata methods declared in the contract:

```perl
# List tables
my $sth = $dbh->table_info(undef, "public", "%", "TABLE");

# List columns
my $sth = $dbh->column_info(undef, "public", "users", "%");
```

These map to `sys_information_recursive` profile queries on the server.
Full reference: [../metadata\_sys\_information.md](../metadata_sys_information.md).

## Conformance (Declared in Contract)

The following conformance areas are declared in `package_contract.json`:

| Area                 | Declared |
|----------------------|----------|
| `connect_auth`       | yes      |
| `prepare_execute_fetch` | yes   |
| `transactions`       | yes      |
| `metadata`           | yes      |
| `type_mapping`       | yes      |
| `error_mapping`      | yes      |
| `reconnect`          | yes      |
| `protocol_negotiation` | yes    |
| `cancellation`       | yes      |

> These are contract declarations, not verified against implemented source.
> See [../conformance\_baseline.md](../conformance_baseline.md) for the
> `driver_perl_gate` conformance profile once source is available.

## What Is Not Yet Covered

Because the source tree contains only `package_contract.json`, the following
sections cannot be documented from source and are omitted:

- Actual `Makefile.PL` / CPAN package name and dependencies
- Concrete DBI DSN attribute names and accepted handle attributes
- Error handling (`$DBI::errstr`, `$DBI::state`, exception behaviour)
- Type mapping table (Perl scalar/reference types for each SBsql OID)
- Additional metadata methods beyond `table_info` and `column_info`
- Pooling configuration (e.g. `DBI::Pool` integration)
- TLS and authentication option details beyond DSN key names
- Thread-confinement enforcement details

These sections will be added when source is committed to the driver directory.

## See Also

- [../README.md](../README.md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire\_protocol\_sbwp.md](../wire_protocol_sbwp.md)
- [../type\_mapping.md](../type_mapping.md)
- [../metadata\_sys\_information.md](../metadata_sys_information.md)
- [../diagnostics\_and\_sqlstate.md](../diagnostics_and_sqlstate.md)
- [../pooling\_and\_concurrency.md](../pooling_and_concurrency.md)
- [../conformance\_baseline.md](../conformance_baseline.md)
