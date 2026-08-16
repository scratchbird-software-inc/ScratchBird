// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#define SCRATCHBIRD_IA01_MISSING_EXECUTOR_FIXTURE_ONLY
#include "ia01_package_missing_executor_integration_test.cpp"
#include "ia01_literal_refusal_scenarios.hpp"
int main() { return RunLiteralRefusalScenario(false); }  // CSC-TEST-002327
