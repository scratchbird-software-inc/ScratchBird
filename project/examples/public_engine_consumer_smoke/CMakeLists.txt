# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.20)
project(scratchbird_public_engine_consumer_smoke C)

# PUBLIC_CONSUMER_SMOKE
find_package(ScratchBirdEngine CONFIG REQUIRED)

add_executable(scratchbird_public_engine_consumer_smoke main.c)
target_link_libraries(scratchbird_public_engine_consumer_smoke PRIVATE ScratchBird::sb_engine)
