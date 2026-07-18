# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

add_library(sbu_beta_parser_support STATIC beta_udr.cpp)
target_link_libraries(sbu_beta_parser_support PRIVATE sbl_beta_parser_pipeline)
