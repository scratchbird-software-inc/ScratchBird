# DuckDB Native Tool Harness

This harness manifest records the reference-native tool or client driver
surfaces that must replay parser-facing tests against a ScratchBird
parser endpoint. The first-tranche replay is enforced by the regular CTest
gate `compatibility_sql_first_tranche_original_tool_replay_gate`, which uses a
locally installed external DuckDB regression runner under the ignored `tools/`
directory and writes regenerated evidence in the build tree. If the external
tool is not installed, CTest reports this lane as skipped rather than packaging
the tool in the public source tree.
