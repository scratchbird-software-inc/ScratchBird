#!/usr/bin/env bash
#
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
#
# run_example_database.sh
#
# Creates and fully populates an example/test ScratchBird database by running an
# ordered series of native SBsql scripts through the sb_isql command-line client
# against an already-running ScratchBird server.
#
# Run as root (or any account that can reach the server and read these scripts):
#     sudo ./run_example_database.sh
#
# Configuration is via environment variables (sensible defaults below). Override
# any of them for your installation, e.g.:
#     SB_BIN=/opt/scratchbird/bin SB_USER=admin SB_PASSWORD=secret \
#         SB_DB=example_db SB_CREATE_DB=1 ./run_example_database.sh
#
set -uo pipefail

# --------------------------------------------------------------------------- #
# Configuration                                                               #
# --------------------------------------------------------------------------- #
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SQL_DIR="${SB_SQL_DIR:-${HERE}/sql}"

# Directory that contains the sb_isql binary. Defaults try common build/install
# locations; override with SB_BIN.
SB_BIN="${SB_BIN:-}"
if [[ -z "${SB_BIN}" ]]; then
  for cand in \
      "${HERE}/../../output/linux-x86_64/bin" \
      "${HERE}/../../build/bin" \
      "/opt/scratchbird/bin" \
      "/usr/local/bin"; do
    if [[ -x "${cand}/sb_isql" ]]; then SB_BIN="${cand}"; break; fi
  done
fi
# Fall back to PATH if still unset.
SB_ISQL="${SB_ISQL:-${SB_BIN:+${SB_BIN}/}sb_isql}"

SB_HOST="${SB_HOST:-127.0.0.1}"
SB_PORT="${SB_PORT:-3092}"
SB_USER="${SB_USER:-alice}"
SB_PASSWORD="${SB_PASSWORD:-scratchbird}" # set this for your installation
SB_ROLE="${SB_ROLE:-sysarch}"            # connection role
SB_SSLMODE="${SB_SSLMODE:-require}"      # disable|allow|prefer|require|verify-ca|verify-full
SB_DB="${SB_DB:-example_db}"             # target database name/path the server uses
SB_CREATE_DB="${SB_CREATE_DB:-0}"        # 1 = attempt CREATE DATABASE first
# This is a TEST harness: its purpose is to exercise the documented command
# surface and REPORT every error, not to stop at the first one. By default it
# runs every statement in every script and aggregates all errors at the end.
SB_STOP_ON_ERROR="${SB_STOP_ON_ERROR:-0}" # 1 = stop at the first failing script
SB_BAIL_IN_SCRIPT="${SB_BAIL_IN_SCRIPT:-0}" # 1 = stop a script at its first failing statement (-b)
SB_RUN_REFUSALS="${SB_RUN_REFUSALS:-0}"  # 1 = run sql/expected_refusals/*.sql (inverted scoring)
SB_TEARDOWN="${SB_TEARDOWN:-0}"          # 1 = run 90_teardown.sql at the end (drops everything)
SB_LOG_DIR="${SB_LOG_DIR:-${HERE}/logs}"
# Marker that sb_isql prints to stderr for a failed statement (see sb_isql.cpp).
SB_ERROR_REGEX="${SB_ERROR_REGEX:-^Error}"

mkdir -p "${SB_LOG_DIR}"

# --------------------------------------------------------------------------- #
# Helpers                                                                     #
# --------------------------------------------------------------------------- #
say()  { printf '[example-db] %s\n' "$*"; }
err()  { printf '[example-db] ERROR: %s\n' "$*" >&2; }

isql() {
  # $1 = database, remaining args passed through to sb_isql
  local db="$1"; shift
  local extra=()
  [[ -n "${SB_PASSWORD}" ]] && extra+=(-P "${SB_PASSWORD}")
  [[ -n "${SB_SSLMODE}" ]]  && extra+=("--sslmode=${SB_SSLMODE}")
  [[ -n "${SB_ROLE}" ]]     && extra+=(--conn-opt "role=${SB_ROLE}")
  "${SB_ISQL}" "${db}" -U "${SB_USER}" \
      -H "${SB_HOST}" -p "${SB_PORT}" "${extra[@]}" "$@"
}

# --------------------------------------------------------------------------- #
# Preflight                                                                   #
# --------------------------------------------------------------------------- #
if ! command -v "${SB_ISQL}" >/dev/null 2>&1 && [[ ! -x "${SB_ISQL}" ]]; then
  err "sb_isql not found (looked for '${SB_ISQL}'). Set SB_BIN or SB_ISQL."
  exit 2
fi
if [[ ! -d "${SQL_DIR}" ]]; then
  err "SQL directory not found: ${SQL_DIR}"
  exit 2
fi

say "sb_isql      : ${SB_ISQL}"
say "server       : ${SB_USER}@${SB_HOST}:${SB_PORT} (role=${SB_ROLE}, sslmode=${SB_SSLMODE})"
say "database     : ${SB_DB}"
say "scripts      : ${SQL_DIR}"
say "logs         : ${SB_LOG_DIR}"

# --------------------------------------------------------------------------- #
# Optional: create the database                                              #
# --------------------------------------------------------------------------- #
# Database creation is installation-specific. If SB_CREATE_DB=1, this runs the
# 00_create_database.sql script, which contains a CREATE DATABASE statement you
# may need to adapt to your deployment (path, page size, owner). If the database
# already exists, leave SB_CREATE_DB=0 (the default) and create it with your
# normal administrative procedure first.
if [[ "${SB_CREATE_DB}" == "1" && -f "${SQL_DIR}/00_create_database.sql" ]]; then
  say "creating database ${SB_DB} ..."
  cdb_out="${SB_LOG_DIR}/00_create_database.out.log"
  cdb_err="${SB_LOG_DIR}/00_create_database.err.log"
  : >"${cdb_out}"; : >"${cdb_err}"
  cdb_drv="$(mktemp "${TMPDIR:-/tmp}/sb_drv.XXXXXX")"
  { printf 'SET ERROR %s\n' "${cdb_err}"; cat "${SQL_DIR}/00_create_database.sql"; } >"${cdb_drv}"
  isql "${SB_DB}" -e -b -o "${cdb_out}" -f "${cdb_drv}" >>"${cdb_out}" 2>>"${cdb_err}"
  cdb_rc=$?
  rm -f "${cdb_drv}"
  if [[ "${cdb_rc}" -ne 0 ]] || grep -qE "${SB_ERROR_REGEX}" "${cdb_err}" 2>/dev/null; then
    err "database creation failed; see ${cdb_out} and ${cdb_err}"
    exit 3
  fi
fi

# --------------------------------------------------------------------------- #
# Run the ordered population scripts                                          #
# --------------------------------------------------------------------------- #
# Population scripts are the numbered sql/*.sql files, EXCEPT:
#   00_create_database.sql  - handled above (optional, SB_CREATE_DB)
#   90_teardown.sql         - run last and only when SB_TEARDOWN=1
# The expected_refusals/ subdirectory is not picked up here (non-recursive glob);
# it is handled separately by the optional refusals pass below.
shopt -s nullglob
scripts=()
for f in "${SQL_DIR}"/*.sql; do
  base="$(basename "${f}")"
  [[ "${base}" == "00_create_database.sql" ]] && continue
  [[ "${base}" == "90_teardown.sql" ]] && continue
  scripts+=("${f}")
done
IFS=$'\n' scripts=($(sort <<<"${scripts[*]}")); unset IFS

if [[ "${#scripts[@]}" -eq 0 ]]; then
  err "no population scripts found in ${SQL_DIR}"
  exit 2
fi

# Per-script: echo each statement (-e) so errors line up with the SQL that
# caused them. Only pass -b (bail at first error) when explicitly requested;
# otherwise every statement is attempted so all errors are captured.
bail_flag=()
[[ "${SB_BAIL_IN_SCRIPT}" == "1" ]] && bail_flag=(-b)

ERR_REPORT="${SB_LOG_DIR}/errors_summary.log"
: >"${ERR_REPORT}"

# run_script <script-path> <output-file> <error-file>
# Uses sb_isql's own input/output/error directives:
#   input  -> -f <input-file>
#   output -> -o <output-file>   (results + echoed statements)
#   errors -> "SET ERROR <error-file>" directive prepended to the input stream
#             (sb_isql has no error-file CLI flag, so the directive is injected
#              the same way SET TERM is; see sb_isql.cpp).
# Shell-level redirection is also pointed at the same files so anything emitted
# before the directives take effect (connection banners/errors) is still kept.
run_script() {
  local script="$1" out="$2" errf="$3"
  local driver
  driver="$(mktemp "${TMPDIR:-/tmp}/sb_drv.XXXXXX")"
  { printf 'SET ERROR %s\n' "${errf}"; cat "${script}"; } >"${driver}"
  isql "${SB_DB}" -e "${bail_flag[@]}" -o "${out}" -f "${driver}" \
      >>"${out}" 2>>"${errf}"
  local rc=$?
  rm -f "${driver}"
  return ${rc}
}

run=0; clean=0; with_errors=0; total_errors=0
for f in "${scripts[@]}"; do
  base="$(basename "${f}")"
  out_log="${SB_LOG_DIR}/${base%.sql}.out.log"
  err_log="${SB_LOG_DIR}/${base%.sql}.err.log"
  : >"${out_log}"; : >"${err_log}"
  run=$((run+1))
  say "running ${base} ..."
  run_script "${f}" "${out_log}" "${err_log}"
  rc=$?
  # Count statement errors by scanning the error-output file for sb_isql's marker.
  n=$(grep -cE "${SB_ERROR_REGEX}" "${err_log}" 2>/dev/null || true)
  n=${n:-0}
  if [[ "${n}" -gt 0 ]]; then
    with_errors=$((with_errors+1))
    total_errors=$((total_errors+n))
    err "${base}: ${n} error(s) (exit ${rc}); out=${out_log} err=${err_log}"
    {
      printf '===== %s : %d error(s) (exit %d) =====\n' "${base}" "${n}" "${rc}"
      grep -nE "${SB_ERROR_REGEX}" "${err_log}"
      printf '\n'
    } >>"${ERR_REPORT}"
    if [[ "${SB_STOP_ON_ERROR}" == "1" ]]; then
      err "stopping (SB_STOP_ON_ERROR=1). Set SB_STOP_ON_ERROR=0 to collect all errors."
      break
    fi
  else
    clean=$((clean+1))
  fi
done

# --------------------------------------------------------------------------- #
# Optional: expected-refusal pass (inverted scoring)                          #
# --------------------------------------------------------------------------- #
# Files in sql/expected_refusals/ contain conversions the engine is DOCUMENTED
# to refuse. Here, success is the bug: a statement that does NOT raise an error
# is the finding (a silent lossy/unsafe conversion). Run before teardown so the
# domains these tests reference still exist.
refusal_findings=0
if [[ "${SB_RUN_REFUSALS}" == "1" ]]; then
  refusal_dir="${SQL_DIR}/expected_refusals"
  if [[ -d "${refusal_dir}" ]]; then
    say "------------------------------------------------------------"
    say "expected-refusal pass (a statement that does NOT error is a finding)"
    for f in "${refusal_dir}"/*.sql; do
      [[ -e "${f}" ]] || continue
      base="$(basename "${f}")"
      out_log="${SB_LOG_DIR}/refusal_${base%.sql}.out.log"
      err_log="${SB_LOG_DIR}/refusal_${base%.sql}.err.log"
      : >"${out_log}"; : >"${err_log}"
      say "running (refusal) ${base} ..."
      # Never bail here: every statement must be attempted.
      local_bail=("${bail_flag[@]}"); bail_flag=()
      run_script "${f}" "${out_log}" "${err_log}"
      bail_flag=("${local_bail[@]}")
      # Expected refusals = single-statement 'select ...;' lines (one per line by
      # construction). Observed refusals = error markers in the error output.
      expected=$(grep -cE '^[[:space:]]*select .*;[[:space:]]*$' "${f}" 2>/dev/null || true); expected=${expected:-0}
      observed=$(grep -cE "${SB_ERROR_REGEX}" "${err_log}" 2>/dev/null || true); observed=${observed:-0}
      missing=$(( expected - observed ))
      if [[ "${missing}" -gt 0 ]]; then
        refusal_findings=$((refusal_findings + missing))
        err "${base}: ${missing} of ${expected} refusals did NOT error (possible silent conversion); out=${out_log}"
        {
          printf '===== REFUSAL %s : %d of %d expected refusals did NOT error =====\n' \
            "${base}" "${missing}" "${expected}"
          printf '(inspect %s for statements that returned a value instead of a diagnostic)\n\n' "${out_log}"
        } >>"${ERR_REPORT}"
      else
        say "${base}: all ${expected} statements refused as expected."
      fi
    done
  else
    say "SB_RUN_REFUSALS=1 but ${refusal_dir} not found; skipping refusal pass."
  fi
fi

# --------------------------------------------------------------------------- #
# Optional: teardown                                                          #
# --------------------------------------------------------------------------- #
# Teardown drops every object the population scripts created, leaving an empty
# database. It is OFF by default so the populated example database is preserved.
if [[ "${SB_TEARDOWN}" == "1" && -f "${SQL_DIR}/90_teardown.sql" ]]; then
  say "------------------------------------------------------------"
  say "running 90_teardown.sql (SB_TEARDOWN=1) ..."
  out_log="${SB_LOG_DIR}/90_teardown.out.log"
  err_log="${SB_LOG_DIR}/90_teardown.err.log"
  : >"${out_log}"; : >"${err_log}"
  run_script "${SQL_DIR}/90_teardown.sql" "${out_log}" "${err_log}"
  n=$(grep -cE "${SB_ERROR_REGEX}" "${err_log}" 2>/dev/null || true); n=${n:-0}
  if [[ "${n}" -gt 0 ]]; then
    with_errors=$((with_errors+1)); total_errors=$((total_errors+n))
    err "90_teardown.sql: ${n} error(s); err=${err_log}"
    {
      printf '===== 90_teardown.sql : %d error(s) =====\n' "${n}"
      grep -nE "${SB_ERROR_REGEX}" "${err_log}"
      printf '\n'
    } >>"${ERR_REPORT}"
  else
    say "teardown completed cleanly."
  fi
fi

# --------------------------------------------------------------------------- #
# Summary                                                                     #
# --------------------------------------------------------------------------- #
say "------------------------------------------------------------"
say "scripts run        : ${run}"
say "clean scripts      : ${clean}"
say "scripts w/errors   : ${with_errors}"
say "total errors       : ${total_errors}"
[[ "${SB_RUN_REFUSALS}" == "1" ]] && say "refusal findings   : ${refusal_findings} (statements that should have errored but did not)"
say "per-script logs    : ${SB_LOG_DIR}"
if [[ "${total_errors}" -eq 0 && "${refusal_findings}" -eq 0 ]]; then
  say "no errors reported - example database populated and exercised cleanly."
  rm -f "${ERR_REPORT}"
  exit 0
fi
err "aggregated error report: ${ERR_REPORT}"
say "(this is a test harness; the errors/findings above are the deliverable to report)"
exit 1
