# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird do
  @moduledoc false

  alias ScratchBird.{Errors, Metadata, Protocol}

  defdelegate schemas_query(), to: Metadata
  defdelegate tables_query(), to: Metadata
  defdelegate columns_query(), to: Metadata
  defdelegate indexes_query(), to: Metadata
  defdelegate index_columns_query(), to: Metadata
  defdelegate constraints_query(), to: Metadata
  defdelegate procedures_query(), to: Metadata
  defdelegate functions_query(), to: Metadata
  defdelegate routines_query(), to: Metadata
  defdelegate catalogs_query(), to: Metadata
  defdelegate primary_keys_query(), to: Metadata
  defdelegate foreign_keys_query(), to: Metadata
  defdelegate table_privileges_query(), to: Metadata
  defdelegate column_privileges_query(), to: Metadata
  defdelegate type_info_query(), to: Metadata

  defdelegate sqlstate_class(code), to: Errors
  defdelegate retry_scope(code), to: Errors
  defdelegate retryable?(code), to: Errors
  defdelegate canonical_read_committed_mode_label(mode), to: Protocol
  defdelegate probe_auth_surface(opts), to: ScratchBird.Connection
  defdelegate get_resolved_auth_context(state), to: ScratchBird.Connection
  defdelegate supports_prepared_transactions(), to: ScratchBird.Connection
  defdelegate supports_dormant_reattach(), to: ScratchBird.Connection
  defdelegate supports_portal_resume(), to: ScratchBird.Connection
end
