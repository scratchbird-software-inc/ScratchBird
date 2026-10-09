#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Inventory mutation oracles; never runtime or final acceptance evidence."""
import argparse
import contextlib
import copy
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

import sbsql_final_language_expansion_closure_gate as language
import final_sblr_sbsql_enterprise_proof_closure_gate as enterprise
import final_sblr_sbsql_master_closure_gate as master

parser = argparse.ArgumentParser()
parser.add_argument('--repo-root', type=Path, required=True)
args, remaining = parser.parse_known_args()
ROOT = args.repo_root.resolve()
_, CORE, WORK = language.resolve_control_roots(ROOT)
AUTHORITIES = language.manifest_authority_paths(CORE)
COMMANDS, OPCODES, ENVELOPES = language.validate_sblr_authority(CORE)


class InventoryOracles(unittest.TestCase):
    def test_blob_consumer_handoff_cannot_drop_typed_authority(self):
        original_read = language.read_csv
        mappings_path = CORE / language.CORE_REGISTRY_FILES['surface_to_sblr']
        for field in ('sblr_operation_family', 'required_context', 'binding_steps', 'result_shape', 'diagnostics'):
            with self.subTest(field=field):
                def read(path, columns, label):
                    rows = original_read(path, columns, label)
                    if path == mappings_path:
                        next(row for row in rows if row['surface_id'] == 'SBSQL-5E47A8E53DE3')[field] = 'invented'
                    return rows
                with patch.object(language, 'read_csv', side_effect=read):
                    self.rejected('BLOB receiving-owner handoff drift',
                                  lambda: language.validate_surface_registries(CORE))

    def test_nonstandalone_surface_boundaries_are_exact(self):
        surfaces, mappings = language.validate_surface_registries(CORE)
        by_id = {row['surface_id']: row for row in surfaces}
        profiles = {}
        for mapping in mappings:
            if mapping['ingress_envelope'] in {'SBLRExecutionEnvelope.v3', 'sbsql_domain_ddl_or_typed_value'}:
                continue
            surface = by_id[mapping['surface_id']]
            profiles.setdefault((surface['status'], mapping['binding_steps']), (surface, mapping))
        self.assertEqual(len(profiles), 11)
        for surface, mapping in profiles.values():
            for field in ('sblr_operation_family', 'ingress_envelope', 'required_context',
                          'binding_steps', 'result_shape', 'diagnostics'):
                with self.subTest(surface=surface['canonical_name'], field=field):
                    mutated = dict(mapping, **{field: 'invented_execution_authority'})
                    self.rejected('nonstandalone surface boundary drift',
                                  lambda: language.validate_nonstandalone_surface(surface, mutated))
            with self.subTest(surface=surface['canonical_name'], field='status'):
                self.rejected('nonstandalone surface boundary drift',
                              lambda: language.validate_nonstandalone_surface(
                                  dict(surface, status='invented_completion'), mapping))

    def test_pre_sblr_refusal_cannot_claim_execution_or_lose_reason(self):
        original_read = language.read_csv
        commands_path = CORE / language.CORE_REGISTRY_FILES['command_closure']
        for field, value in (
            ('root_route', 'SBLR_DIAGNOSTIC_REFUSAL'),
            ('executor_operation_id', 'execute_statement_timestamp'),
            ('bound_ast_node_kind', 'bound_timestamp'),
            ('descriptor_contract', 'timestamp'),
            ('transaction_effect', 'commit'),
            ('result_shape', 'scalar'),
            ('required_right', 'SELECT'),
            ('resource_contract', 'timezone_seed'),
            ('cluster_contract', 'cluster_provider'),
            ('diagnostic_key', 'generic_refusal'),
        ):
            with self.subTest(field=field):
                def read(path, columns, label):
                    rows = original_read(path, columns, label)
                    if path == commands_path:
                        target = next(row for row in rows if row['root_route_kind'] == 'pre_sblr_admission_refusal')
                        target[field] = value
                    return rows
                with patch.object(language, 'read_csv', side_effect=read):
                    self.rejected('pre-SBLR refusal claims downstream authority',
                                  lambda: language.validate_sblr_authority(CORE))

    def setUp(self):
        self.original = language.read_csv
        self.rows = {}
        for filename in ('IMPLEMENTATION_LEDGER.csv', 'TEST_LEDGER.csv', 'AREA_MATRIX.csv',
                         'TRACKER.csv', 'OWNER_DECISIONS.csv', 'FINDINGS.csv', 'GENERATED_PROVENANCE.csv'):
            self.rows[filename] = self.original(WORK / filename, set(), 'oracle baseline')

    def lookup(self, filename, key, value):
        matches = [row for row in self.rows[filename] if row[key] == value]
        self.assertEqual(len(matches), 1)
        return matches[0]

    def read(self, path, columns, label):
        if path.parent == WORK and path.name in self.rows:
            return copy.deepcopy(self.rows[path.name])
        return self.original(path, columns, label)

    def validate(self):
        with patch.object(language, 'read_csv', side_effect=self.read):
            return language.validate_workplan_obligations(AUTHORITIES, WORK, COMMANDS, OPCODES, ENVELOPES)

    def controls(self):
        with patch.object(enterprise, 'read_csv', side_effect=self.read):
            return enterprise.validate_active_controls(ROOT, WORK)

    def rejected(self, reason, callback=None):
        error = io.StringIO()
        with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as raised:
            (callback or self.validate)()
        self.assertEqual(raised.exception.code, 1)
        self.assertIn(reason, error.getvalue())

    def test_current_inventory_remains_in_progress(self):
        summary = self.validate()
        self.assertEqual(summary['implementation_items'], len(self.rows['IMPLEMENTATION_LEDGER.csv']))
        self.assertEqual(summary['test_obligations'], len(self.rows['TEST_LEDGER.csv']))
        self.assertEqual(summary['workplan_status'], 'in_progress')

    def test_master_accounts_for_exact_supplemental_identities(self):
        summary = self.validate()
        self.assertEqual(summary['profile_command_identities'], 3)
        self.assertEqual(summary['implementation_element_identities'], 4)
        with patch.object(enterprise, 'validate_enterprise', return_value=summary):
            self.assertEqual(master.validate_master(ROOT, None)['workplan_status'], 'in_progress')
            for field in ('implementation_items', 'profile_command_identities',
                          'implementation_element_identities', 'command_identities'):
                with self.subTest(field=field):
                    original = summary[field]
                    summary[field] += 1
                    self.rejected('implementation population does not equal',
                                  lambda: master.validate_master(ROOT, None))
                    summary[field] = original

    def test_retired_and_unknown_statuses_are_not_admitted(self):
        row = self.rows['IMPLEMENTATION_LEDGER.csv'][0]
        before = row['implementation_status']
        for value in ('absent', 'partial', 'exact_refusal_verified', 'blocked_owner_decision', 'complete', 'unavailable'):
            with self.subTest(value=value):
                row['implementation_status'] = value
                self.rejected('implementation obligation has unsupported/final status')
        row['implementation_status'] = before
        row = self.rows['TEST_LEDGER.csv'][0]
        for value in ('test_required', 'audit_candidate', 'unavailable', 'complete'):
            with self.subTest(value=value):
                row['implementation_status'] = value
                self.rejected('test obligation has unsupported/final status')

    def test_duplicate_missing_and_invented_inventory_rows(self):
        for filename, reason in (('IMPLEMENTATION_LEDGER.csv', 'duplicate implementation obligation'),
                                 ('TEST_LEDGER.csv', 'duplicate test obligation id')):
            with self.subTest(filename=filename):
                self.rows[filename].append(dict(self.rows[filename][0]))
                self.rejected(reason)
                self.rows[filename].pop()
        row = self.rows['IMPLEMENTATION_LEDGER.csv'].pop(0)
        self.rejected('active implementation ledger does not exactly cover')
        self.rows['IMPLEMENTATION_LEDGER.csv'].insert(0, row)
        invented = dict(row, item_type='implementation_element', item_id='INVENTED')
        self.rows['IMPLEMENTATION_LEDGER.csv'].append(invented)
        self.rejected('active implementation ledger does not exactly cover')

    def test_absorbed_optimizer_cannot_be_promoted_or_lose_review(self):
        row = next(row for row in self.rows['IMPLEMENTATION_LEDGER.csv']
                   if row['area_id'] == 'IA-EXT-OPT' and row['item_id'] != 'SBLR_MATCH_RECOGNIZE')
        row['implementation_status'] = 'corrected_verified'
        self.rejected('promoted without an exact local review')
        row['implementation_status'] = 'in_progress'
        row['review_status'] = 'invented_review'
        self.rejected('loses its review provenance')

    def test_shared_procedure_proofs_are_required(self):
        row = self.lookup('IMPLEMENTATION_LEDGER.csv', 'item_id', 'SBSQL-C5D151D17944')
        original = dict(row)
        row['expected_route'] = 'parent_profile:SBLR_DDL_CREATE_TABLE'
        self.rejected('procedure profile parent route drift')
        row.update(original)
        row['existing_test_evidence'] = row['existing_test_evidence'].replace('CSC-TEST-005810', 'REMOVED')
        self.rejected('procedure syntax child lost shared obligations')

    def test_all_element_obligations_remain_required(self):
        for item_id in language.ELEMENT_TEST_FAMILIES:
            with self.subTest(item_id=item_id):
                row = next(r for r in self.rows['TEST_LEDGER.csv'] if r['item_id'] == item_id)
                original = row['test_family']
                row['test_family'] = 'invented'
                self.rejected('implementation element obligation drift')
                row['test_family'] = original

    def test_core_qualifiers_and_authority_are_exact(self):
        row = self.lookup('TEST_LEDGER.csv', 'test_id', 'CSC-TEST-005831')
        original = row['controlling_authority']
        for invalid in ('executor=engine.op.unknown', 'operand=unknown', 'result=unknown', 'effect=none', 'arbitrary=allow'):
            with self.subTest(invalid=invalid):
                row['controlling_authority'] = original + '|' + invalid
                self.rejected('test opcode annotation differs from Core')
        row['controlling_authority'] = 'IA-DECISION-0010'
        self.rejected('test has annotations but no Core authority')
        row['controlling_authority'] = 'unadmitted/path.md'
        self.rejected('test obligation cites non-admitted Core authority')

    def test_area_and_layer_labels_are_exact_tokens(self):
        row = self.lookup('TEST_LEDGER.csv', 'test_id', 'CSC-TEST-005831')
        before = dict(row)
        row['area_id'] = 'IA-00'
        self.rejected('test/implementation area drift')
        row.update(before)
        for part in (row['area_id'], row['required_layer']):
            with self.subTest(part=part):
                row['planned_ctest_label'] = before['planned_ctest_label'].replace(part, 'prefix_' + part + '_suffix')
                self.rejected('test obligation label loses area/layer identity')

    def test_policy_opcode_retains_dedicated_cancellation_proof(self):
        row = self.lookup('TEST_LEDGER.csv', 'test_id', 'CSC-TEST-002164')
        row['existing_test_evidence'] = 'generic_fault_marker'
        self.rejected('policy observer lost dedicated fault proof')

    def test_both_kv_baseline_cohorts_remain_required(self):
        for test_id in ('CSC-TEST-002561', 'CSC-TEST-003933', 'CSC-TEST-002564', 'CSC-TEST-003936'):
            with self.subTest(test_id=test_id):
                row = self.lookup('TEST_LEDGER.csv', 'test_id', test_id)
                self.rows['TEST_LEDGER.csv'].remove(row)
                self.rejected('KV structured read lost a retained baseline cohort')
                self.rows['TEST_LEDGER.csv'].append(row)

    def test_open_owner_acceptance_remains_required(self):
        row = self.lookup('TRACKER.csv', 'phase_id', 'IA-15')
        row['status'] = 'complete'
        self.rejected('open owner-acceptance phase')

    def test_in_progress_and_component_findings_remain_open(self):
        summary = self.controls()
        rows = self.rows['FINDINGS.csv']
        self.assertEqual(summary['open_findings'], sum(r['status'] != 'resolved' for r in rows))
        self.assertEqual(summary['open_release_blockers'],
                         sum(r['status'] != 'resolved' and r['severity'] == 'release_blocking' for r in rows))
        row = next(r for r in rows if r['status'] == 'resolved_component_only')
        row['resolution'] = ''
        self.rejected('resolved finding has no resolution', self.controls)
        row['status'] = 'unknown'
        self.rejected('has unknown status', self.controls)

    def test_every_generator_recipe_member_is_required_and_contained(self):
        row = self.rows['GENERATED_PROVENANCE.csv'][0]
        original = row['generator_path']
        for extra in ('missing_generator.py', '/etc/passwd', '../escape.py', ''):
            with self.subTest(extra=extra):
                row['generator_path'] = original + ';' + extra
                self.rejected('retained generator', self.controls)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *remaining])
