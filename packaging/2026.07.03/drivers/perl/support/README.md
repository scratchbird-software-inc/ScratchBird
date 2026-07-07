# ScratchBird Perl DBI Driver

Perl DBI lane for ScratchBird.

This lane now has a real Perl package layout:

- `Makefile.PL`
- `lib/DBD/ScratchBird.pm`
- `tools/sb_isql_perl.pl`
- `t/sb_isql_perl_missing_dependency.t`
- [Baseline requirement mapping](BASELINE_REQUIREMENT_MAPPING.md)

## Status

The lane is native route-runner source at this stage. It exposes a `DBD::ScratchBird`
package shape, DBI connection entrypoint, metadata method surfaces, and the
complete-coverage native tool. Unsupported TLS dependency, embedded transport,
attach/create, transaction execution, and SBWP statement execution fail closed
with SQLSTATE-style diagnostics.

It does not delegate to another ScratchBird driver or command-line tool, and it
does not return fabricated rows.

## Runtime Dependencies

Required Perl modules:

- `DBI`
- `JSON::PP`

`JSON::PP` is available in standard Perl installations. If `DBI` is missing,
`tools/sb_isql_perl.pl` exits non-zero and writes the complete artifact set
with a precise dependency diagnostic.

## Native Tool

The native conformance tool is:

```bash
perl project/drivers/driver/perl/tools/sb_isql_perl.pl \
  --database example.sbdb \
  --host 127.0.0.1 \
  --port 3092 \
  --user sysdba \
  --password masterkey \
  --role '' \
  --sslmode disable \
  --route listener-parser \
  --parser-mode server-parser \
  --page-size 8k \
  --namespace users.public.examples.perl.manual \
  --input script.sbsql \
  --output build/perl/stdout.log \
  --error build/perl/stderr.log \
  --diagnostics build/perl/diagnostics.jsonl \
  --metrics build/perl/process-metrics.jsonl \
  --transcript build/perl/wire-transcript.jsonl \
  --summary build/perl/summary.json
```

The tool accepts the common complete-coverage matrix arguments and writes:

- `command-events.jsonl`
- `summary.json`
- `diagnostics.jsonl`
- `wire-transcript.jsonl`
- `timing-groups.json`
- `result-digests.json`
- `metadata-snapshots.json`
- `route-environment.json`
- `process-metrics.jsonl`
- `security-refusals.json`
- `native-api-coverage.json`
- `code-example-review.json`
- `junit.xml`
- `stdout.log`
- `stderr.log`

## API Sketch

```perl
use DBI;

my $dbh = DBI->connect(
    'dbi:ScratchBird:database=example.sbdb;host=127.0.0.1;port=3092;sslmode=disable',
    'sysdba',
    'masterkey',
    { RaiseError => 1, PrintError => 0 },
);

my $sth = $dbh->prepare('SELECT 1');
$sth->execute;
while (my $row = $sth->fetchrow_arrayref) {
    print $row->[0], "\n";
}
$dbh->disconnect;
```

Until the Perl SBWP executor is present, statement execution fails closed
with `0A000`.
