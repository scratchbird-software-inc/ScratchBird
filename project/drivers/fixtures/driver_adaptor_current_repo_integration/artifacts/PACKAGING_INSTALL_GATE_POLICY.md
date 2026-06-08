# Packaging And Install Gate Policy

Search key: `DRIVER_PACKAGING_INSTALL_GATE_POLICY`

Every driver, adaptor, and tool with a distributable artifact must build that
artifact under:

```text
build/drivers/_packages/<category>/<name>/
```

Install/import smoke tests must install from the package output into an
isolated test prefix under:

```text
build/drivers/_install/<category>/<name>/
```

Package gates must not write generated artifacts, dependency trees, or install
outputs into `project/drivers/`.

Release status values:

- `supported`: build, tests, conformance, package, install smoke, and static
  hygiene pass.
- `experimental`: source builds and unit tests pass, but full conformance or
  packaging is incomplete.
- `toolchain-waived`: source is imported but the toolchain is explicitly waived
  for this run.
- `not-imported`: no source exists under `project/drivers/`.
