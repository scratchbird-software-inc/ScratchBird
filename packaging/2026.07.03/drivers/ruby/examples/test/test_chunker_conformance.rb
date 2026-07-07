# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"
require "json"

# Cross-driver statement-chunker conformance. Loads the shared fixture and
# asserts the Ruby client splitter reproduces every `expected` exactly. Mirrors
# tests/conformance/drivers/chunker_conformance/verify_python_reference.py.
class TestChunkerConformance < Minitest::Test
  CASES_PATH = File.expand_path(
    "../../../../tests/conformance/drivers/chunker_conformance/cases.json",
    __dir__
  )

  def split(input)
    # split_sql_statements is a private instance method; invoke it on a bare
    # allocated client without driving any connection setup.
    Scratchbird::Client.allocate.send(:split_sql_statements, input)
  end

  def test_chunker_conformance_cases
    cases = JSON.parse(File.read(CASES_PATH))["cases"]
    refute_empty cases, "fixture must contain cases"
    cases.each do |kase|
      got = split(kase["input"])
      assert_equal kase["expected"], got, "chunker case #{kase["name"]} mismatch"
    end
  end
end
