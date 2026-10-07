# Core Config

Configuration and policy resolution. Subsystems must not parse independent configuration files directly.

## Metric bootstrap policy inputs

`metric_policy_config.hpp` provides `LoadMetricPolicyConfig(defaults_path,
families_path)` and the in-memory `ParseMetricPolicyConfig` equivalent. Select an
exact family's typed input with `SelectMetricPolicyDefinition`; supply any
existing governing family definition so it wins as a whole. An invalid governing
definition is an error, never permission to fall back.

Distributed files are `project/config/templates/metric-policy-defaults.conf`
and `metric-policy-families.conf`, installed into `etc/scratchbird`. Pass both
paths explicitly; there is no working-directory search, environment expansion,
include directive, silent compiled fallback or automatic runtime reload.

The operational profile supplies 14 days of raw samples and 400 days of
1-minute/1-hour/1-day rollups, database metric-read permission and redacted
sensitive labels. Only exact family sections opting into that profile inherit
it. Explicit sections require every field and never inherit subsequent default
changes. The supplied transaction-active and filespace-used definitions preserve
their 1024-series bound and 30-day/FAMILY-permission requirements respectively.
Security/audit families require their own explicit governing definitions; cluster
policies cannot be loaded through this local bootstrap input API.

The format uses ASCII `[section]` headers and `key = value` lines, whole-line
`#` comments, unsigned decimal seconds/counts, and comma-separated grains. Each
file is bounded to 256 KiB, each line to 2048 bytes, and the family file to 1024
families. Unknown fields, duplicate keys/sections, unsupported versions, invalid
numbers, malformed enums, NUL/control bytes and file errors reject the entire
input with `METRIC.RETENTION_POLICY_INVALID` and a detail string. The error never
returns a partially usable configuration. Configuration names contain no UUIDs.

These APIs return **identity-free create/import inputs**, not catalog policies,
authorization grants, observations or producer readiness. The catalog writer
must separately check the governing family contract, create native policy/label/
descriptor records with binary UUIDv7 identities and generations, and publish
them through MGA. Reopen uses those records, not these files. Changing a file
does not alter an existing node. Native metric bootstrap publication remains a
separate integration repair; loading this configuration does not complete it.
