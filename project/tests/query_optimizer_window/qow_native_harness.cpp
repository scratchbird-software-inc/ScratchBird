// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::qow {

// QOW-SOURCE-TST-001-HARNESS-V1
// QOW_NATIVE_HARNESS_MAIN
struct HarnessOutcome {
    int exit_code{0};
    std::string output_json;
    std::string diagnostic_json;
};

// QOW-RCP-081A-CASE-FREE-PROOF-RECORD-V1
// This is a record codec only. It cannot execute a case, launch a topology,
// access a database, or decide transaction/snapshot/finality authority.
enum class ProofRecordScalarKind {
    kString,
    kNumber,
    kBoolean,
    kNull,
    kObject,
    kArray,
};

struct ProofRecordFieldView {
    std::string_view key;
    std::string_view value;
    ProofRecordScalarKind kind{ProofRecordScalarKind::kString};
};

struct ProofRecordOutcome {
    bool accepted{false};
    std::string canonical_json;
    std::string diagnostic_json;
};

namespace {

constexpr std::array<std::string_view, 11> kE3ProofRecordFields{
    "boundast_sblr_sha256",
    "descriptor_sha256",
    "diagnostic_sha256",
    "evidence_level",
    "logical_plan_sha256",
    "native_topology_manifest_sha256",
    "physical_plan_sha256",
    "record_id",
    "result_sha256",
    "schema_id",
    "stage_trace_sha256",
};

constexpr std::array<std::string_view, 15> kE4ProofRecordFields{
    "boundast_sblr_sha256",
    "causal_counter_sha256",
    "descriptor_sha256",
    "diagnostic_sha256",
    "evidence_level",
    "forced_plan_record_key",
    "logical_plan_sha256",
    "mga_recheck_sha256",
    "mutation_action_digest",
    "native_topology_manifest_sha256",
    "physical_plan_sha256",
    "record_id",
    "result_sha256",
    "schema_id",
    "stage_trace_sha256",
};

constexpr std::array<std::string_view, 11> kDigestFields{
    "boundast_sblr_sha256",
    "causal_counter_sha256",
    "descriptor_sha256",
    "diagnostic_sha256",
    "logical_plan_sha256",
    "mga_recheck_sha256",
    "mutation_action_digest",
    "native_topology_manifest_sha256",
    "physical_plan_sha256",
    "result_sha256",
    "stage_trace_sha256",
};

[[nodiscard]] bool IsContinuationByte(const unsigned char byte)
{
    return (byte & 0xc0U) == 0x80U;
}

[[nodiscard]] bool IsValidUtf8(const std::string_view value)
{
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1 >= value.size() ||
                !IsContinuationByte(static_cast<unsigned char>(value[index + 1]))) {
                return false;
            }
            index += 2;
            continue;
        }

        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2 >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[index + 1]);
            const auto third = static_cast<unsigned char>(value[index + 2]);
            if (!IsContinuationByte(second) || !IsContinuationByte(third) ||
                (first == 0xe0U && second < 0xa0U) ||
                (first == 0xedU && second >= 0xa0U)) {
                return false;
            }
            index += 3;
            continue;
        }

        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3 >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[index + 1]);
            const auto third = static_cast<unsigned char>(value[index + 2]);
            const auto fourth = static_cast<unsigned char>(value[index + 3]);
            if (!IsContinuationByte(second) || !IsContinuationByte(third) ||
                !IsContinuationByte(fourth) ||
                (first == 0xf0U && second < 0x90U) ||
                (first == 0xf4U && second >= 0x90U)) {
                return false;
            }
            index += 4;
            continue;
        }

        return false;
    }
    return true;
}

void AppendJsonString(std::string* output, const std::string_view value)
{
    constexpr std::string_view kHex = "0123456789abcdef";
    output->push_back('"');
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
            case '"':
                output->append("\\\"");
                break;
            case '\\':
                output->append("\\\\");
                break;
            case '\b':
                output->append("\\b");
                break;
            case '\f':
                output->append("\\f");
                break;
            case '\n':
                output->append("\\n");
                break;
            case '\r':
                output->append("\\r");
                break;
            case '\t':
                output->append("\\t");
                break;
            default:
                if (byte < 0x20U) {
                    output->append("\\u00");
                    output->push_back(kHex[(byte >> 4U) & 0x0fU]);
                    output->push_back(kHex[byte & 0x0fU]);
                } else {
                    output->push_back(raw);
                }
                break;
        }
    }
    output->push_back('"');
}

[[nodiscard]] std::string CanonicalStringObject(
    const std::map<std::string_view, std::string_view, std::less<>>& fields)
{
    std::string output;
    output.push_back('{');
    bool first = true;
    for (const auto& [key, value] : fields) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        AppendJsonString(&output, key);
        output.push_back(':');
        AppendJsonString(&output, value);
    }
    output.append("}\n");
    return output;
}

[[nodiscard]] std::string Refusal(const std::string_view reason,
                                  const std::string_view field)
{
    std::map<std::string_view, std::string_view, std::less<>> diagnostic{
        {"diagnostic_id", "QOW-PROOF-RECORD-REFUSED"},
        {"field", field},
        {"reason", reason},
        {"schema_id", "qow-proof-record-diagnostic-v1"},
    };
    return CanonicalStringObject(diagnostic);
}

[[nodiscard]] bool IsLowerHexSha256(const std::string_view value)
{
    return value.size() == 64U &&
           std::ranges::all_of(value, [](const char value_char) {
               return (value_char >= '0' && value_char <= '9') ||
                      (value_char >= 'a' && value_char <= 'f');
           });
}

[[nodiscard]] bool IsDigestField(const std::string_view key)
{
    return std::ranges::find(kDigestFields, key) != kDigestFields.end();
}

template <std::size_t FieldCount>
[[nodiscard]] bool IsExpectedField(
    const std::array<std::string_view, FieldCount>& expected,
    const std::string_view key)
{
    return std::ranges::find(expected, key) != expected.end();
}

template <std::size_t FieldCount>
[[nodiscard]] ProofRecordOutcome ValidateSelectedRecord(
    const std::span<const ProofRecordFieldView> input,
    const std::array<std::string_view, FieldCount>& expected)
{
    std::map<std::string_view, std::string_view, std::less<>> fields;
    for (const auto& field : input) {
        if (!IsValidUtf8(field.key)) {
            return ProofRecordOutcome{
                false, {}, Refusal("utf8_invalid", "field_key")};
        }
        if (field.kind != ProofRecordScalarKind::kString) {
            return ProofRecordOutcome{
                false, {}, Refusal("nonscalar_field", field.key)};
        }
        if (!IsExpectedField(expected, field.key)) {
            return ProofRecordOutcome{
                false, {}, Refusal("unknown_field", field.key)};
        }
        if (!fields.emplace(field.key, field.value).second) {
            return ProofRecordOutcome{
                false, {}, Refusal("duplicate_field", field.key)};
        }
    }

    for (const auto key : expected) {
        if (!fields.contains(key)) {
            return ProofRecordOutcome{
                false, {}, Refusal("missing_field", key)};
        }
    }

    if (fields.at("schema_id") != "qow-proof-record-v1") {
        return ProofRecordOutcome{
            false, {}, Refusal("schema_id_invalid", "schema_id")};
    }
    if (fields.at("evidence_level") != (FieldCount == 11U ? "E3" : "E4")) {
        return ProofRecordOutcome{
            false, {}, Refusal("evidence_level_invalid", "evidence_level")};
    }
    if (fields.at("record_id").empty()) {
        return ProofRecordOutcome{
            false, {}, Refusal("required_identity_empty", "record_id")};
    }

    for (const auto& [key, value] : fields) {
        if (!IsValidUtf8(value)) {
            return ProofRecordOutcome{
                false, {}, Refusal("utf8_invalid", key)};
        }
        if (IsDigestField(key) && !IsLowerHexSha256(value)) {
            return ProofRecordOutcome{
                false, {}, Refusal("sha256_invalid", key)};
        }
    }

    if constexpr (FieldCount == 15U) {
        if (fields.at("forced_plan_record_key").empty()) {
            return ProofRecordOutcome{
                false, {},
                Refusal("required_identity_empty", "forced_plan_record_key")};
        }
    }

    return ProofRecordOutcome{true, CanonicalStringObject(fields), {}};
}

}  // namespace

[[nodiscard]] ProofRecordOutcome ValidateAndSerializeProofRecord(
    const std::span<const ProofRecordFieldView> input)
{
    const auto level = std::ranges::find_if(input, [](const auto& field) {
        return field.key == "evidence_level";
    });
    if (level == input.end()) {
        return ProofRecordOutcome{
            false, {}, Refusal("missing_field", "evidence_level")};
    }
    if (level->kind != ProofRecordScalarKind::kString) {
        return ProofRecordOutcome{
            false, {}, Refusal("nonscalar_field", "evidence_level")};
    }
    if (level->value == "E3") {
        return ValidateSelectedRecord(input, kE3ProofRecordFields);
    }
    if (level->value == "E4") {
        return ValidateSelectedRecord(input, kE4ProofRecordFields);
    }
    return ProofRecordOutcome{
        false, {}, Refusal("evidence_level_invalid", "evidence_level")};
}

[[nodiscard]] HarnessOutcome ExecuteHarness(
    const std::span<const std::string_view> arguments)
{
    if (arguments.size() == 1 && arguments.front() == "--self-test") {
        return HarnessOutcome{
            0,
            "{\"execution_mode\":\"non_semantic_self_test\","
            "\"schema_id\":\"qow-native-harness-bootstrap-v1\","
            "\"semantic_execution\":false,\"status\":\"success\"}\n",
            {},
        };
    }

    const std::string_view reason = arguments.empty()
        ? "missing_mode"
        : "unsupported_mode";
    return HarnessOutcome{
        64,
        {},
        std::string{"{\"diagnostic_id\":\"QOW-HARNESS-MODE-REFUSED\","
                    "\"reason\":\""} + std::string{reason} +
            "\",\"schema_id\":\"qow-native-harness-diagnostic-v1\"}\n",
    };
}

}  // namespace scratchbird::qow

#ifndef SCRATCHBIRD_QOW_HARNESS_NO_MAIN
int main(int argc, char** argv)
{
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const auto outcome = scratchbird::qow::ExecuteHarness(arguments);
    if (!outcome.output_json.empty()) {
        std::cout << outcome.output_json;
    }
    if (!outcome.diagnostic_json.empty()) {
        std::cerr << outcome.diagnostic_json;
    }
    return outcome.exit_code;
}
#endif
