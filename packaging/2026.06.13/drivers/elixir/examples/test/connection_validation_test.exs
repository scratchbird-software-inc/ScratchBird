# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBirdConnectionValidationTest do
  use ExUnit.Case

  alias ScratchBird.{Config, Connection}

  test "accepts sslmode=disable for explicit local parity flows" do
    cfg =
      Config.from_opts(
        url: "scratchbird://user:pass@localhost:3092/testdb?sslmode=disable"
      )

    assert cfg.sslmode == "disable"
  end

  test "accepts binary_transfer=false compatibility setting" do
    cfg =
      Config.from_opts(
        url: "scratchbird://user:pass@localhost:3092/testdb?binary_transfer=false"
      )

    assert cfg.binary_transfer == false
  end

  test "accepts compression=zstd compatibility setting" do
    cfg =
      Config.from_opts(
        url: "scratchbird://user:pass@localhost:3092/testdb?compression=zstd"
      )

    assert cfg.compression == "zstd"
  end

  test "rejects invalid protocol" do
    assert_raise ArgumentError, "Only protocol=native is supported; connect to the native parser listener/port.", fn ->
      Connection.connect(
        url: "scratchbird://user:pass@localhost:3092/testdb?protocol=postgres"
      )
    end
  end
end
