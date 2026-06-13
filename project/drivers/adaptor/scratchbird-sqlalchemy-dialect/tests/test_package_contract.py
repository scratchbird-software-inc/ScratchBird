# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import json
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_package_contract_records_sqlalchemy_release_posture() -> None:
    contract = json.loads((ROOT / "package_contract.json").read_text(encoding="utf-8"))

    assert contract["component_id"] == "adaptor:scratchbird-sqlalchemy-dialect"
    assert contract["status"] == "beta_2"
    assert contract["release_scope"] == "in_scope_required"
    assert contract["server_revalidation_required"] is True
    assert contract["transaction_authority"] == "mga_engine"
    assert contract["delegation_posture"]["mode"] == "delegates_to_python"
    assert contract["delegation_posture"]["target_component"] == "driver:python"
    assert contract["artifact_verification"]["package_type"] == "python_wheel"
    assert contract["dbeaver_exclusion"]["this_artifact_includes_dbeaver"] is False
    assert len(contract["best_in_class_deltas"]) >= 2


def test_package_contract_files_are_present_and_exclude_dbeaver() -> None:
    contract = json.loads((ROOT / "package_contract.json").read_text(encoding="utf-8"))

    for relative in contract["package_files"]:
        assert "dbeaver" not in relative
        assert (ROOT / relative).is_file(), relative
