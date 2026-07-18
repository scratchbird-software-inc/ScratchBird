# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

add_library(sbl_beta_parser_pipeline STATIC beta_parser.cpp)
target_link_libraries(sbl_beta_parser_pipeline PRIVATE sbl_parser_neutral_protocol)
