# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sbl_donor_parser_common STATIC
  donor_dialect.cpp
  donor_worker_session.cpp
)

target_compile_features(sbl_donor_parser_common PUBLIC cxx_std_23)
target_include_directories(sbl_donor_parser_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
