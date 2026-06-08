# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(transaction_inventory_publish_fault_conformance
  transaction_inventory_publish_fault_conformance.cpp
)
target_compile_features(transaction_inventory_publish_fault_conformance PRIVATE cxx_std_23)
target_link_libraries(transaction_inventory_publish_fault_conformance PRIVATE
  sb_core_hash
  sb_core_memory
  sb_core_platform
  sb_core_uuid
  sb_storage_database
  sb_storage_disk
  sb_storage_page
  sb_transaction_mga
)

add_test(
  NAME transaction_inventory_publish_fault_conformance
  COMMAND transaction_inventory_publish_fault_conformance
)
set_tests_properties(transaction_inventory_publish_fault_conformance PROPERTIES
  LABELS "ELER-022;ELER-GATE-022;ELER-080;ELER-GATE-080;ELER-100;ELER-GATE-100;ELER-101;ELER-GATE-101;ELER-104;ELER-GATE-104;engine_listener_enterprise;fault_injection;crash_recovery;transaction_inventory;generation_publish;partial_write;stale_tail;checksum;transaction_inventory_property;linux_proof"
)
