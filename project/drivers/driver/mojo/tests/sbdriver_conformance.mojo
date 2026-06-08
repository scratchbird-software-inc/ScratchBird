# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from python import Python


fn main() raises:
    var os = Python.import_module("os")
    var runpy = Python.import_module("runpy")
    var candidates = Python.list()
    _ = candidates.append("tests/sbdriver_conformance.py")
    _ = candidates.append("sbdriver_conformance.py")
    _ = candidates.append("project/drivers/driver/mojo/tests/sbdriver_conformance.py")
    for path in candidates:
        if os.path.exists(path):
            _ = runpy.run_path(path, run_name="__main__")
            return
    raise Error("sbdriver_conformance.py not found; run from mojo lane root or tests directory")
