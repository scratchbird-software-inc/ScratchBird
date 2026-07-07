# Reference Regression Acquisition

ScratchBird does not vendor upstream reference-engine regression suites.
The source tree provides a repeatable acquisition tool and source map so a
developer can download the suites locally when validating a reference parser.

Run from the public repository root:

```bash
python3 project/tests/reference_regression/acquire_reference_regression_assets.py \
  --repo-root "$PWD" \
  --download \
  --check \
  --strict-payload
```

To verify already-downloaded local suites:

```bash
python3 project/tests/reference_regression/acquire_reference_regression_assets.py \
  --repo-root "$PWD" \
  --check \
  --strict-payload
```

Downloaded payloads are written below:

```text
project/tests/reference_regression/reference_release_acquisition/<reference>/<version>/regression/acquired/
```

Generated local evidence is written below:

```text
build/reference_regression_acquisition/
```

The acquired payloads are intentionally ignored by git. Normal public CTest
runs do not depend on them; reference-parser replay jobs opt into them when
validating a specific parser lane against the original reference test tools.
