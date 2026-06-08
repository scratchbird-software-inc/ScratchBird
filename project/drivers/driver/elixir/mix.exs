# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBirdEcto.MixProject do
  use Mix.Project

  def project do
    [
      app: :scratchbird_ecto,
      version: "0.1.0",
      description: "ScratchBird Ecto adapter and native protocol client",
      elixir: "~> 1.14",
      package: package(),
      deps: deps(),
      start_permanent: Mix.env() == :prod
    ]
  end

  def application do
    [extra_applications: [:logger, :crypto, :ssl]]
  end

  defp deps do
    [
      {:db_connection, "~> 2.6"},
      {:ecto_sql, "~> 3.11"},
      {:decimal, "~> 2.0"},
      {:jason, "~> 1.4"},
      {:postgrex, "~> 0.19.0"}
    ]
  end

  defp package do
    [
      licenses: ["Apache-2.0"],
      links: %{
        "Project" => "https://scratchbird.invalid/driver"
      }
    ]
  end
end
