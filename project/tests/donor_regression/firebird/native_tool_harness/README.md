# FirebirdSQL Native Tool Harness

This harness manifest records the donor-native tool or client driver
surfaces that must replay parser-facing tests against a ScratchBird
parser endpoint. The first-tranche replay is enforced by the regular CTest
gate `donor_sql_first_tranche_original_tool_replay_gate`, which stages the
original tool under `tools/` and writes regenerated evidence in the build tree.
