# Index Comparison Verdict Model

The index-comparison lane separates execution state from comparative judgment.

## Execution Status

- `pass`
  - the scenario executed and produced a valid normalized result
- `fail`
  - the scenario executed but violated a correctness expectation such as
    returning too few rows
- `error`
  - the scenario could not complete or plan capture failed
- `unsupported`
  - the engine or target does not support the required capability

## Comparative Verdict

Comparative verdicts are emitted by the pairwise comparator, not by the
per-engine runner.

- `better`
  - candidate plan quality is no worse than the baseline and measured
    performance is outside the noise band in the candidate's favor
- `equivalent`
  - candidate and baseline are materially the same within the noise band
- `worse`
  - candidate plan quality or measured performance is materially worse
- `fallback`
  - candidate fell back to a materially weaker path such as a full scan when
    the baseline did not
- `unsupported`
  - candidate cannot support the required capability for the scenario
- `invalid`
  - one or both sides do not have a valid comparable run artifact

## Score Mapping

If a compact ordinal score is needed for dashboards, use:

- `better` -> `100`
- `equivalent` -> `85`
- `worse` -> `60`
- `fallback` -> `25`
- `unsupported` -> `10`
- `invalid` -> `0`

The score is only a convenience layer. The text verdict remains authoritative.

## Noise Band

The current noise band is `5%`.

That means a result is usually treated as `equivalent` unless:

- latency improves by more than `5%`
- throughput improves by more than `5%`
- or plan quality changes materially
