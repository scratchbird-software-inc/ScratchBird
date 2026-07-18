# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

add_library(sbl_alpha_parser_pipeline STATIC alpha_parser.cpp)
target_link_libraries(sbl_alpha_parser_pipeline PRIVATE sbl_beta_parser_pipeline)
target_sources(sbl_alpha_parser_pipeline PRIVATE
  ${PROJECT_SOURCE_DIR}/project/src/parsers/compatibility/beta/beta_generated.cpp
)
