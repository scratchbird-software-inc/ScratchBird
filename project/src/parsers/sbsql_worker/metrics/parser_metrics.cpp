// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metrics/parser_metrics.hpp"

#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::sbsql {
namespace {

std::string ResourceBudgetJson(const ParserResourceBudget& budget) {
  std::ostringstream out;
  out << "{\"max_statement_bytes\":" << budget.max_statement_bytes
      << ",\"max_identifier_bytes\":" << budget.max_identifier_bytes
      << ",\"max_token_count\":" << budget.max_token_count
      << ",\"max_literal_bytes\":" << budget.max_literal_bytes
      << ",\"max_ast_depth\":" << budget.max_ast_depth
      << ",\"max_parameter_count\":" << budget.max_parameter_count
      << ",\"max_sblr_envelope_bytes\":" << budget.max_sblr_envelope_bytes
      << ",\"max_diagnostic_payload_bytes\":"
      << budget.max_diagnostic_payload_bytes
      << ",\"max_message_vector_count\":" << budget.max_message_vector_count
      << ",\"max_result_metadata_columns\":"
      << budget.max_result_metadata_columns
      << ",\"max_render_output_bytes\":"
      << budget.max_render_output_bytes
      << ",\"max_parser_cache_entries\":"
      << budget.max_parser_cache_entries << '}';
  return out.str();
}

} // namespace

ParserMetrics::ParserMetrics() : start_(std::chrono::steady_clock::now()) {}

void ParserMetrics::Increment(std::string name, std::uint64_t by) {
  std::lock_guard lock(mutex_);
  counters_[std::move(name)] += by;
}

void ParserMetrics::SetGauge(std::string name, double value) {
  std::lock_guard lock(mutex_);
  gauges_[std::move(name)] = value;
}

void ParserMetrics::SetState(ParserState state) {
  std::lock_guard lock(mutex_);
  state_ = state;
}

ParserState ParserMetrics::State() const {
  std::lock_guard lock(mutex_);
  return state_;
}

std::string ParserMetrics::SnapshotJson(const ParserConfig& config,
                                        const SessionContext& session,
                                        const SblrTemplateCache& cache) const {
  std::lock_guard lock(mutex_);
  std::ostringstream out;
  out << "{\"namespace\":\"sys.metrics.parsers\","
      << "\"parser_uuid\":\"" << EscapeJson(config.parser_uuid) << "\","
      << "\"dialect\":\"" << EscapeJson(config.dialect) << "\","
      << "\"session_uuid\":\"" << EscapeJson(session.authenticated ? session.session_uuid : "") << "\","
      << "\"state\":\"" << StateName(state_) << "\","
      << "\"resource_budgets\":" << ResourceBudgetJson(config.resource_budget) << ','
      << "\"cache\":" << cache.SnapshotJson() << ",\"counters\":{";
  bool first = true;
  for (const auto& [name, value] : counters_) {
    if (!first) out << ',';
    first = false;
    out << '\"' << EscapeJson(name) << "\":" << value;
  }
  out << "},\"gauges\":{";
  first = true;
  for (const auto& [name, value] : gauges_) {
    if (!first) out << ',';
    first = false;
    out << '\"' << EscapeJson(name) << "\":" << value;
  }
  out << "}}";
  return out.str();
}

std::string ParserMetrics::HeartbeatJson(const ParserConfig& config,
                                         const SessionContext& session,
                                         const SblrTemplateCache& cache,
                                         std::string_view current_operation) const {
  const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start_).count();
  std::lock_guard lock(mutex_);
  std::ostringstream out;
  out << "{\"parser_uuid\":\"" << EscapeJson(config.parser_uuid) << "\","
#ifndef _WIN32
      << "\"parser_pid\":" << static_cast<long long>(::getpid()) << ','
#else
      << "\"parser_pid\":0,"
#endif
      << "\"dialect\":\"" << EscapeJson(config.dialect) << "\","
      << "\"connection_uuid\":\"" << EscapeJson(session.connection_uuid) << "\","
      << "\"session_uuid\":\"" << EscapeJson(session.authenticated ? session.session_uuid : "") << "\","
      << "\"state\":\"" << StateName(state_) << "\","
      << "\"uptime_ms\":" << uptime_ms << ','
      << "\"last_client_activity_ms\":0,\"last_server_activity_ms\":0,"
      << "\"current_operation\":\"" << EscapeJson(current_operation) << "\","
      << "\"current_operation_age_ms\":0,\"memory_bytes\":0,"
      << "\"health_flags\":[],\"cache_counts\":" << cache.SnapshotJson() << ','
      << "\"resource_budgets\":" << ResourceBudgetJson(config.resource_budget) << ','
      << "\"redaction_state\":\"" << EscapeJson(session.metric_redaction_policy) << "\"}";
  return out.str();
}

} // namespace scratchbird::parser::sbsql
