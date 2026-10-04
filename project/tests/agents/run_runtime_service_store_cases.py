"""Collect all original cases with one actual database/metric owner per process."""
import subprocess
import sys
import os
import shutil
import tempfile

failed = []
cases = ("round-trip", "atomic-batch", "checkpoint-evidence", "transaction-per-operation",
         "drain-shutdown", "supervision", "crash-replay", "duplicate-owner", "metric-owner-mismatch")
for case in cases:
    print(f"=== {case} ===", flush=True)
    directory = tempfile.mkdtemp(prefix=f"runtime-service-{case}-")
    try:
        result = subprocess.run([sys.argv[1], case], timeout=180,
                                env=dict(os.environ, TMPDIR=directory, TMP=directory, TEMP=directory))
        if result.returncode:
            failed.append(case)
    except subprocess.TimeoutExpired:
        failed.append(case)
    if case in failed:
        print(f"retained failed fixture: {directory}", flush=True)
    else:
        shutil.rmtree(directory)
print(f"runtime service cases: {len(cases) - len(failed)}/{len(cases)} passed; failures={failed}", flush=True)
sys.exit(bool(failed))
