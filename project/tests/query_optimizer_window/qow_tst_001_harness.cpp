// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define SCRATCHBIRD_QOW_HARNESS_NO_MAIN
#include "qow_native_harness.cpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] bool Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

}  // namespace

// QOW-TEST-TST-001-HARNESS-V1
// QOW_TEST_TST_001_HARNESS
int main()
{
    bool passed = true;

    passed &= Require(QOW_BUILD_QOW_CLOSURE_TESTS == 1,
                      "isolated QOW switch is not enabled");
    passed &= Require(QOW_BUILD_TESTS == 0,
                      "broad SB_BUILD_TESTS switch leaked into QOW enrollment");
    passed &= Require(QOW_BUILD_SBSQL_PARSER_WORKER_TESTS == 0,
                      "broad parser-worker test switch leaked into QOW enrollment");

    constexpr std::array<std::string_view, 1> self_test{"--self-test"};
    const auto success = scratchbird::qow::ExecuteHarness(self_test);
    passed &= Require(success.exit_code == 0, "self-test did not succeed");
    passed &= Require(
        success.output_json ==
            "{\"execution_mode\":\"non_semantic_self_test\","
            "\"schema_id\":\"qow-native-harness-bootstrap-v1\","
            "\"semantic_execution\":false,\"status\":\"success\"}\n",
        "self-test output is not the locked structured record");
    passed &= Require(success.diagnostic_json.empty(),
                      "successful self-test emitted a diagnostic");

    const auto missing = scratchbird::qow::ExecuteHarness({});
    passed &= Require(missing.exit_code == 64,
                      "missing mode did not fail closed");
    passed &= Require(missing.output_json.empty(),
                      "missing mode emitted a successful output record");
    passed &= Require(
        missing.diagnostic_json ==
            "{\"diagnostic_id\":\"QOW-HARNESS-MODE-REFUSED\","
            "\"reason\":\"missing_mode\","
            "\"schema_id\":\"qow-native-harness-diagnostic-v1\"}\n",
        "missing mode diagnostic differs");

    constexpr std::array<std::string_view, 1> unsupported{"--execute-sql"};
    const auto refusal = scratchbird::qow::ExecuteHarness(unsupported);
    passed &= Require(refusal.exit_code == 64,
                      "unsupported semantic mode did not fail closed");
    passed &= Require(refusal.output_json.empty(),
                      "unsupported semantic mode emitted successful output");
    passed &= Require(
        refusal.diagnostic_json.find("unsupported_mode") != std::string::npos,
        "unsupported semantic mode diagnostic differs");

    constexpr std::string_view kDigestA =
        "000000000000000000000000000000000000000000000000000000000000000a";
    constexpr std::string_view kDigestB =
        "111111111111111111111111111111111111111111111111111111111111111b";
    constexpr std::string_view kDigestC =
        "222222222222222222222222222222222222222222222222222222222222222c";
    constexpr std::string_view kDigestD =
        "333333333333333333333333333333333333333333333333333333333333333d";
    constexpr std::string_view kDigestE =
        "444444444444444444444444444444444444444444444444444444444444444e";
    constexpr std::string_view kDigestF =
        "555555555555555555555555555555555555555555555555555555555555555f";
    constexpr std::string_view kDigestG =
        "6666666666666666666666666666666666666666666666666666666666666660";
    constexpr std::string_view kDigestH =
        "7777777777777777777777777777777777777777777777777777777777777771";
    constexpr std::string_view kDigestI =
        "8888888888888888888888888888888888888888888888888888888888888882";
    constexpr std::string_view kDigestJ =
        "9999999999999999999999999999999999999999999999999999999999999993";
    constexpr std::string_view kDigestK =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    using scratchbird::qow::ProofRecordFieldView;
    using scratchbird::qow::ProofRecordScalarKind;

    const std::array<ProofRecordFieldView, 11> e3_fields{{
        {"stage_trace_sha256", kDigestH},
        {"record_id", "QOW-CASE-\"UTF8-π\\line\n-V1"},
        {"schema_id", "qow-proof-record-v1"},
        {"result_sha256", kDigestG},
        {"physical_plan_sha256", kDigestF},
        {"native_topology_manifest_sha256", kDigestE},
        {"logical_plan_sha256", kDigestD},
        {"evidence_level", "E3"},
        {"diagnostic_sha256", kDigestC},
        {"descriptor_sha256", kDigestB},
        {"boundast_sblr_sha256", kDigestA},
    }};
    const auto e3 = scratchbird::qow::ValidateAndSerializeProofRecord(e3_fields);
    passed &= Require(e3.accepted, "valid E3 record was refused");
    passed &= Require(e3.diagnostic_json.empty(),
                      "valid E3 record emitted a diagnostic");
    const std::string expected_e3 =
        std::string{"{\"boundast_sblr_sha256\":\""} + std::string{kDigestA} +
        "\",\"descriptor_sha256\":\"" + std::string{kDigestB} +
        "\",\"diagnostic_sha256\":\"" + std::string{kDigestC} +
        "\",\"evidence_level\":\"E3\",\"logical_plan_sha256\":\"" +
        std::string{kDigestD} +
        "\",\"native_topology_manifest_sha256\":\"" + std::string{kDigestE} +
        "\",\"physical_plan_sha256\":\"" + std::string{kDigestF} +
        "\",\"record_id\":\"QOW-CASE-\\\"UTF8-π\\\\line\\n-V1\","
        "\"result_sha256\":\"" + std::string{kDigestG} +
        "\",\"schema_id\":\"qow-proof-record-v1\",\"stage_trace_sha256\":\"" +
        std::string{kDigestH} + "\"}\n";
    passed &= Require(e3.canonical_json == expected_e3,
                      "E3 canonical JSON differs");

    auto reversed_e3 = e3_fields;
    std::ranges::reverse(reversed_e3);
    const auto e3_reversed =
        scratchbird::qow::ValidateAndSerializeProofRecord(reversed_e3);
    passed &= Require(e3_reversed.accepted &&
                          e3_reversed.canonical_json == e3.canonical_json,
                      "E3 field order changed canonical serialization");
    const auto e3_repeated =
        scratchbird::qow::ValidateAndSerializeProofRecord(e3_fields);
    passed &= Require(e3_repeated.canonical_json == e3.canonical_json,
                      "repeated E3 serialization is not deterministic");

    const std::array<ProofRecordFieldView, 15> e4_fields{{
        {"schema_id", "qow-proof-record-v1"},
        {"record_id", "QOW-E4-CASE-V1"},
        {"evidence_level", "E4"},
        {"boundast_sblr_sha256", kDigestA},
        {"descriptor_sha256", kDigestB},
        {"diagnostic_sha256", kDigestC},
        {"logical_plan_sha256", kDigestD},
        {"native_topology_manifest_sha256", kDigestE},
        {"physical_plan_sha256", kDigestF},
        {"result_sha256", kDigestG},
        {"stage_trace_sha256", kDigestH},
        {"causal_counter_sha256", kDigestI},
        {"forced_plan_record_key", "QOW-FORCED-PLAN-V1"},
        {"mga_recheck_sha256", kDigestJ},
        {"mutation_action_digest", kDigestK},
    }};
    const auto e4 = scratchbird::qow::ValidateAndSerializeProofRecord(e4_fields);
    passed &= Require(e4.accepted && e4.diagnostic_json.empty(),
                      "valid E4 extension was refused");
    passed &= Require(
        e4.canonical_json.find("\"causal_counter_sha256\"") !=
                std::string::npos &&
            e4.canonical_json.find("\"mga_recheck_sha256\"") !=
                std::string::npos &&
            e4.canonical_json.find("\"forced_plan_record_key\"") !=
                std::string::npos &&
            e4.canonical_json.back() == '\n',
        "E4 extension is missing required canonical fields");

    const auto require_refusal =
        [&passed](const std::span<const ProofRecordFieldView> fields,
                  const std::string_view reason,
                  const std::string_view field,
                  const std::string_view message) {
            const auto outcome =
                scratchbird::qow::ValidateAndSerializeProofRecord(fields);
            passed &= Require(!outcome.accepted && outcome.canonical_json.empty(),
                              message);
            passed &= Require(
                outcome.diagnostic_json.find(
                    std::string{"\"reason\":\""} + std::string{reason} + "\"") !=
                        std::string::npos &&
                    outcome.diagnostic_json.find(
                        std::string{"\"field\":\""} + std::string{field} +
                        "\"") != std::string::npos &&
                    outcome.diagnostic_json.back() == '\n',
                "refusal diagnostic is not exact canonical JSON");
        };

    auto upper_digest = e3_fields;
    upper_digest.back().value =
        "A00000000000000000000000000000000000000000000000000000000000000a";
    require_refusal(upper_digest, "sha256_invalid", "boundast_sblr_sha256",
                    "mixed-case digest was accepted");

    auto short_digest = e3_fields;
    short_digest.back().value = "abcd";
    require_refusal(short_digest, "sha256_invalid", "boundast_sblr_sha256",
                    "short digest was accepted");

    auto nonhex_digest = e3_fields;
    nonhex_digest.back().value =
        "g00000000000000000000000000000000000000000000000000000000000000a";
    require_refusal(nonhex_digest, "sha256_invalid", "boundast_sblr_sha256",
                    "non-hex digest was accepted");

    std::vector<ProofRecordFieldView> missing_fields(e3_fields.begin(),
                                                     e3_fields.end());
    missing_fields.erase(missing_fields.begin());
    require_refusal(missing_fields, "missing_field", "stage_trace_sha256",
                    "missing field was accepted");

    auto duplicate = e3_fields;
    duplicate.front() = duplicate.back();
    require_refusal(duplicate, "duplicate_field", "boundast_sblr_sha256",
                    "duplicate field was accepted");

    auto unknown = e3_fields;
    unknown.front().key = "unknown_sha256";
    require_refusal(unknown, "unknown_field", "unknown_sha256",
                    "unknown field was accepted");

    constexpr std::array<ProofRecordScalarKind, 5> nonscalar_kinds{
        ProofRecordScalarKind::kNumber,
        ProofRecordScalarKind::kBoolean,
        ProofRecordScalarKind::kNull,
        ProofRecordScalarKind::kObject,
        ProofRecordScalarKind::kArray,
    };
    for (const auto kind : nonscalar_kinds) {
        auto nonscalar = e3_fields;
        nonscalar.front().kind = kind;
        require_refusal(nonscalar, "nonscalar_field", "stage_trace_sha256",
                        "nonscalar field was accepted");
    }

    auto nonscalar_level = e3_fields;
    nonscalar_level[7].kind = ProofRecordScalarKind::kNumber;
    require_refusal(nonscalar_level, "nonscalar_field", "evidence_level",
                    "nonscalar evidence level was accepted");

    auto wrong_schema = e3_fields;
    wrong_schema[2].value = "qow-proof-record-v2";
    require_refusal(wrong_schema, "schema_id_invalid", "schema_id",
                    "wrong schema was accepted");

    auto empty_record_id = e3_fields;
    empty_record_id[1].value = "";
    require_refusal(empty_record_id, "required_identity_empty", "record_id",
                    "empty record identity was accepted");

    auto bad_level = e3_fields;
    bad_level[7].value = "E5";
    require_refusal(bad_level, "evidence_level_invalid", "evidence_level",
                    "unknown evidence level was accepted");

    auto empty_forced_plan = e4_fields;
    empty_forced_plan[12].value = "";
    require_refusal(empty_forced_plan, "required_identity_empty",
                    "forced_plan_record_key",
                    "empty forced-plan identity was accepted");

    const std::string invalid_utf8{"invalid-\xc0\xaf", 10};
    auto bad_utf8 = e3_fields;
    bad_utf8[1].value = invalid_utf8;
    require_refusal(bad_utf8, "utf8_invalid", "record_id",
                    "invalid UTF-8 was accepted");

    const std::string invalid_key{"bad-\xc0\xaf", 6};
    auto bad_utf8_key = e3_fields;
    bad_utf8_key.front().key = invalid_key;
    require_refusal(bad_utf8_key, "utf8_invalid", "field_key",
                    "invalid UTF-8 field key was accepted");

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
