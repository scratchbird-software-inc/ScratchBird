// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/typed_result_producer_cursor.hpp"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace datatypes = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;
namespace wire = scratchbird::wire;
using platform::byte;

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

wire::TypedResultUuid Uuid(byte discriminator) {
  wire::TypedResultUuid uuid{};
  uuid[0] = 0x01;
  uuid[1] = 0x9f;
  uuid[6] = 0x70;
  uuid[8] = 0x80;
  uuid[15] = discriminator;
  return uuid;
}

wire::TypedResultRowDescriptor Descriptor() {
  wire::TypedResultRowDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(0x31);
  descriptor.descriptor_generation = 7;
  descriptor.datatype_catalog_snapshot_uuid = Uuid(0x32);
  descriptor.datatype_catalog_generation = 8;
  descriptor.datatype_registry_generation = 9;

  wire::TypedResultColumnDescriptor column;
  column.ordinal = 0;
  column.name_occurrence = 0;
  column.name = "value;name=literal";
  column.nullability = wire::TypedResultNullability::nullable;
  column.descriptor_uuid = Uuid(0x33);
  column.descriptor_generation = 10;
  column.type_uuid = Uuid(0x34);
  column.type_generation = 11;
  column.canonical_type_id = datatypes::CanonicalTypeId::character;
  column.codec_id = "datatype.character.utf8.v1";
  column.codec_version = 1;
  column.codec_generation = 12;
  descriptor.columns.push_back(std::move(column));
  return descriptor;
}

wire::TypedResultRow Row(std::uint64_t ordinal, std::string value) {
  wire::TypedResultCell cell;
  cell.column_ordinal = 0;
  cell.name_occurrence = 0;
  cell.state = wire::TypedResultValueState::value_present;
  cell.canonical_payload.assign(value.begin(), value.end());
  wire::TypedResultRow row;
  row.row_ordinal = ordinal;
  row.cells.push_back(std::move(cell));
  return row;
}

struct AuthorityControl {
  api::TypedResultProducerOwnerObservationV1 owner =
      api::TypedResultProducerOwnerObservationV1::authorized;
  api::TypedResultProducerReceiptObservationV1 receipt =
      api::TypedResultProducerReceiptObservationV1::live;
  api::TypedResultProducerMgaObservationV1 mga =
      api::TypedResultProducerMgaObservationV1::live_and_equal;
  api::TypedResultProducerGrantObservationV1 grant =
      api::TypedResultProducerGrantObservationV1::live;
  bool descriptor_live = true;
  std::vector<api::TypedResultProducerCancellationObservationV1>
      cancellation_script;
  std::size_t cancellation_index = 0;

  std::atomic<int> owner_calls{0};
  std::atomic<int> receipt_calls{0};
  std::atomic<int> mga_calls{0};
  std::atomic<int> descriptor_calls{0};
  std::atomic<int> cancellation_calls{0};
  std::atomic<int> grant_calls{0};
  std::atomic<int> statement_releases{0};
  std::atomic<int> mga_releases{0};
  std::atomic<int> cancellation_releases{0};
  std::atomic<int> grant_releases{0};

  api::TypedResultProducerCancellationObservationV1 NextCancellation() {
    ++cancellation_calls;
    if (cancellation_index < cancellation_script.size()) {
      return cancellation_script[cancellation_index++];
    }
    return api::TypedResultProducerCancellationObservationV1::live;
  }

  void SetCancellationScript(
      std::vector<api::TypedResultProducerCancellationObservationV1> script) {
    cancellation_script = std::move(script);
    cancellation_index = 0;
  }
};

struct ProducerControl {
  std::vector<api::TypedResultProducerStageResultV1> staged;
  std::size_t next = 0;
  std::function<void(const api::TypedResultProducerStageRequestV1&)> on_stage;
  std::atomic<int> stage_calls{0};
  std::atomic<int> stage_commits{0};
  std::atomic<int> stage_aborts{0};
  std::atomic<int> close_calls{0};
  bool omit_next_lease = false;
  bool force_next_lease = false;
  api::TypedResultProducerStageCommitStatusV1 next_commit_status =
      api::TypedResultProducerStageCommitStatusV1::committed;
};

class ProducerStageLease final
    : public api::TypedResultProducerStageLeaseActionV1 {
 public:
  ProducerStageLease(std::shared_ptr<ProducerControl> control,
                     std::size_t staged_index)
      : control_(std::move(control)), staged_index_(staged_index) {}

  CommitStatus Commit() noexcept override {
    if (!control_ || control_->next != staged_index_) {
      return CommitStatus::stale;
    }
    if (control_->next_commit_status != CommitStatus::committed) {
      return control_->next_commit_status;
    }
    ++control_->next;
    ++control_->stage_commits;
    return CommitStatus::committed;
  }

  void Abort() noexcept override {
    if (control_ && control_->next == staged_index_) {
      ++control_->stage_aborts;
    }
  }

 private:
  std::shared_ptr<ProducerControl> control_;
  std::size_t staged_index_ = 0;
};

class StatementReceipt final : public api::TypedResultStatementReceiptHandleV1 {
 public:
  explicit StatementReceipt(std::shared_ptr<AuthorityControl> control)
      : control_(std::move(control)) {}

  api::TypedResultProducerOwnerObservationV1 ObserveOwner(
      const wire::TypedResultUuid&) override {
    ++control_->owner_calls;
    return control_->owner;
  }
  api::TypedResultProducerReceiptObservationV1 ObserveReceipt(
      const wire::TypedResultUuid&) override {
    ++control_->receipt_calls;
    return control_->receipt;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->statement_releases;
  }

 private:
  std::shared_ptr<AuthorityControl> control_;
};

class SnapshotPin final : public api::TypedResultMgaSnapshotPinHandleV1 {
 public:
  explicit SnapshotPin(std::shared_ptr<AuthorityControl> control)
      : control_(std::move(control)) {}

  api::TypedResultProducerMgaObservationV1 ObserveSnapshot(
      const wire::TypedResultUuid&,
      const wire::TypedResultUuid&) override {
    ++control_->mga_calls;
    return control_->mga;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->mga_releases;
  }

 private:
  std::shared_ptr<AuthorityControl> control_;
};

class CancellationReceipt final
    : public api::TypedResultCancellationReceiptHandleV1 {
 public:
  explicit CancellationReceipt(std::shared_ptr<AuthorityControl> control)
      : control_(std::move(control)) {}

  api::TypedResultProducerCancellationObservationV1 ObserveCancellation(
      const wire::TypedResultUuid&,
      std::uint64_t) override {
    return control_->NextCancellation();
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->cancellation_releases;
  }

 private:
  std::shared_ptr<AuthorityControl> control_;
};

class ResourceGrant final
    : public api::TypedResultResourceGrantReceiptHandleV1 {
 public:
  explicit ResourceGrant(std::shared_ptr<AuthorityControl> control)
      : control_(std::move(control)) {}

  api::TypedResultProducerGrantObservationV1 ObserveGrant(
      const wire::TypedResultUuid&,
      std::uint64_t,
      std::uint64_t,
      std::uint64_t) override {
    ++control_->grant_calls;
    return control_->grant;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->grant_releases;
  }

 private:
  std::shared_ptr<AuthorityControl> control_;
};

class Producer final : public api::TypedResultProducerSourceV1 {
 public:
  explicit Producer(std::shared_ptr<ProducerControl> control)
      : control_(std::move(control)) {}

  api::TypedResultProducerStageResultV1 Stage(
      const api::TypedResultProducerStageRequestV1& request) override {
    ++control_->stage_calls;
    if (control_->on_stage) control_->on_stage(request);
    if (control_->next >= control_->staged.size()) {
      api::TypedResultProducerStageResultV1 refused;
      refused.outcome = api::TypedResultProducerStageOutcomeV1::refused;
      refused.detail = "fixture_exhausted";
      return refused;
    }
    const auto& staged = control_->staged[control_->next];
    api::TypedResultProducerStageResultV1 result;
    result.outcome = staged.outcome;
    result.end_of_cursor = staged.end_of_cursor;
    result.rows = staged.rows;
    result.detail = staged.detail;
    const bool lease_required =
        result.outcome == api::TypedResultProducerStageOutcomeV1::batch ||
        result.outcome == api::TypedResultProducerStageOutcomeV1::empty_eos;
    if (lease_required && !control_->omit_next_lease) {
      result.lease = api::TypedResultProducerStageLeaseV1(
          std::make_unique<ProducerStageLease>(control_, control_->next));
    } else if (!lease_required && control_->force_next_lease) {
      result.lease = api::TypedResultProducerStageLeaseV1(
          std::make_unique<ProducerStageLease>(control_, control_->next));
    } else if (result.outcome ==
               api::TypedResultProducerStageOutcomeV1::empty_open) {
      ++control_->next;
    }
    control_->omit_next_lease = false;
    control_->force_next_lease = false;
    return result;
  }
  void Close(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->close_calls;
  }

 private:
  std::shared_ptr<ProducerControl> control_;
};

api::TypedResultProducerStageResultV1 Batch(
    std::vector<wire::TypedResultRow> rows,
    bool end = false) {
  api::TypedResultProducerStageResultV1 result;
  result.outcome = api::TypedResultProducerStageOutcomeV1::batch;
  result.end_of_cursor = end;
  result.rows = std::move(rows);
  return result;
}

api::TypedResultProducerStageResultV1 EmptyOpen() {
  api::TypedResultProducerStageResultV1 result;
  result.outcome = api::TypedResultProducerStageOutcomeV1::empty_open;
  return result;
}

api::TypedResultProducerStageResultV1 EmptyEos() {
  api::TypedResultProducerStageResultV1 result;
  result.outcome = api::TypedResultProducerStageOutcomeV1::empty_eos;
  result.end_of_cursor = true;
  return result;
}

api::TypedResultProducerOpenRequestV1 OpenRequest(
    const std::shared_ptr<AuthorityControl>& authority,
    const std::shared_ptr<ProducerControl>& producer) {
  api::TypedResultProducerOpenRequestV1 request;
  request.carrier_uuid = Uuid(0x40);
  request.carrier_generation = 7;
  request.cursor_uuid = Uuid(0x41);
  request.session_uuid = Uuid(0x42);
  request.statement_receipt_uuid = Uuid(0x43);
  request.statement_snapshot_uuid = Uuid(0x44);
  request.cancellation_receipt_uuid = Uuid(0x45);
  request.cancellation_generation = 8;
  request.resource_grant_receipt_uuid = Uuid(0x46);
  request.resource_grant_generation = 9;
  request.resource_grant_bytes = 65536;
  request.execution_uuid = Uuid(0x47);
  request.result_set_uuid = Uuid(0x48);
  request.row_descriptor_uuid = Uuid(0x31);
  request.row_descriptor_generation = 7;
  request.snapshot_uuid = request.statement_snapshot_uuid;
  request.cursor_stream_descriptor_uuid = Uuid(0x49);
  request.cursor_stream_descriptor_version = 1;
  request.cursor_stream_descriptor_generation = 10;
  request.max_chunk_rows = 8;
  request.max_chunk_bytes = 65536;
  request.row_descriptor = Descriptor();
  request.descriptor_authority = [authority](
                                     const wire::TypedResultRowDescriptor&) {
    ++authority->descriptor_calls;
    wire::TypedResultDescriptorAuthorityDecision decision;
    decision.accepted = authority->descriptor_live;
    if (!decision.accepted) {
      decision.diagnostic_code = "DATATYPE.DESCRIPTOR_INVALID";
      decision.detail = "test_descriptor_authority_refused";
    }
    return decision;
  };
  request.statement_receipt =
      std::make_unique<StatementReceipt>(authority);
  request.mga_snapshot_pin = std::make_unique<SnapshotPin>(authority);
  request.cancellation_receipt =
      std::make_unique<CancellationReceipt>(authority);
  request.resource_grant_receipt =
      std::make_unique<ResourceGrant>(authority);
  request.producer_state = std::make_unique<Producer>(producer);
  return request;
}

api::TypedResultProducerPullRequestV1 PullRequest(
    std::uint64_t ordinal = 0) {
  api::TypedResultProducerPullRequestV1 request;
  request.cursor_uuid = Uuid(0x41);
  request.carrier_generation = 7;
  request.cursor_stream_descriptor_uuid = Uuid(0x49);
  request.cursor_stream_descriptor_version = 1;
  request.cursor_stream_descriptor_generation = 10;
  request.expected_batch_ordinal = ordinal;
  request.maximum_rows = 4;
  request.maximum_bytes = 32768;
  request.timeout_millis = 1000;
  return request;
}

struct OpenedFixture {
  std::shared_ptr<AuthorityControl> authority;
  std::shared_ptr<ProducerControl> producer;
  std::unique_ptr<api::TypedResultProducerCursorCarrierV1> carrier;
};

OpenedFixture Opened() {
  OpenedFixture fixture;
  fixture.authority = std::make_shared<AuthorityControl>();
  fixture.producer = std::make_shared<ProducerControl>();
  auto opened = api::OpenTypedResultProducerCursorV1(
      OpenRequest(fixture.authority, fixture.producer));
  Require(opened.ok(), "valid producer cursor open was refused: " +
                           opened.diagnostic_code + ":" + opened.detail);
  fixture.carrier = std::move(opened.carrier);
  return fixture;
}

void RequireReleasedOnce(const OpenedFixture& fixture,
                         const std::string& context) {
  Require(fixture.authority->statement_releases == 1 &&
              fixture.authority->mga_releases == 1 &&
              fixture.authority->cancellation_releases == 1 &&
              fixture.authority->grant_releases == 1 &&
              fixture.producer->close_calls == 1,
          context + " did not release all retained authorities exactly once");
}

void OpenRetainsAndCloseReleasesExactlyOnce() {
  auto fixture = Opened();
  const auto snapshot = fixture.carrier->Snapshot();
  Require(snapshot.lifecycle == api::TypedResultProducerCursorLifecycleV1::open &&
              !snapshot.retained_authority_released &&
              snapshot.next_batch_ordinal == 0 && snapshot.row_position == 0,
          "open carrier mutable state was not exact");
  Require(fixture.authority->statement_releases == 0 &&
              fixture.authority->mga_releases == 0 &&
              fixture.authority->cancellation_releases == 0 &&
              fixture.authority->grant_releases == 0 &&
              fixture.producer->close_calls == 0,
          "open did not retain all authorities");
  const auto decoded = wire::DecodeTypedResultRowDescriptor(
      fixture.carrier->result_descriptor_vector());
  Require(decoded.ok() &&
              decoded.descriptor.descriptor_uuid == snapshot.row_descriptor_uuid &&
              decoded.descriptor.descriptor_evidence_sha256 ==
                  fixture.carrier->row_descriptor().descriptor_evidence_sha256,
          "open did not retain exact canonical descriptor bytes");

  const auto closed = api::CloseTypedResultProducerCursorV1(
      *fixture.carrier,
      api::TypedResultProducerCloseReasonV1::explicit_close);
  Require(closed.ok() &&
              closed.lifecycle ==
                  api::TypedResultProducerCursorLifecycleV1::closed,
          "valid explicit close failed");
  RequireReleasedOnce(fixture, "explicit close");
  const auto replay = api::CloseTypedResultProducerCursorV1(
      *fixture.carrier,
      api::TypedResultProducerCloseReasonV1::explicit_close);
  Require(!replay.ok() && replay.diagnostic_code == "CURSOR.STALE",
          "second close did not observe terminal state");
  RequireReleasedOnce(fixture, "second close");
  fixture.carrier.reset();
  RequireReleasedOnce(fixture, "destruction after close");
}

void OpenAndPullRefusalPrecedence() {
  {
    auto authority = std::make_shared<AuthorityControl>();
    auto producer = std::make_shared<ProducerControl>();
    authority->owner = api::TypedResultProducerOwnerObservationV1::denied;
    auto request = OpenRequest(authority, producer);
    request.max_chunk_rows = 0;
    const auto refused =
        api::OpenTypedResultProducerCursorV1(std::move(request));
    Require(!refused.ok() &&
                refused.diagnostic_code ==
                    "SB_ENGINE_STATUS_INVALID_ARGUMENT" &&
                authority->owner_calls == 0,
            "open invalid-argument precedence drifted");
    Require(authority->statement_releases == 1 &&
                authority->mga_releases == 1 &&
                authority->cancellation_releases == 1 &&
                authority->grant_releases == 1 && producer->close_calls == 1,
            "refused open did not release transferred handles once");
  }
  {
    auto authority = std::make_shared<AuthorityControl>();
    auto producer = std::make_shared<ProducerControl>();
    authority->owner = api::TypedResultProducerOwnerObservationV1::denied;
    authority->mga =
        api::TypedResultProducerMgaObservationV1::stale_or_unequal;
    authority->descriptor_live = false;
    auto request = OpenRequest(authority, producer);
    request.snapshot_uuid = Uuid(0xee);
    const auto refused =
        api::OpenTypedResultProducerCursorV1(std::move(request));
    Require(!refused.ok() &&
                refused.diagnostic_code == "SECURITY.ACCESS_DENIED" &&
                authority->mga_calls == 0 && authority->descriptor_calls == 0,
            "open owner precedence drifted below MGA or descriptor checks");
  }
  {
    auto authority = std::make_shared<AuthorityControl>();
    auto producer = std::make_shared<ProducerControl>();
    auto request = OpenRequest(authority, producer);
    request.row_descriptor_uuid = Uuid(0xef);
    const auto refused =
        api::OpenTypedResultProducerCursorV1(std::move(request));
    Require(!refused.ok() &&
                refused.diagnostic_code == "DATATYPE.DESCRIPTOR_INVALID" &&
                authority->descriptor_calls == 0,
            "open admitted a query-handle/descriptor identity mismatch");
  }
  {
    auto fixture = Opened();
    fixture.authority->owner =
        api::TypedResultProducerOwnerObservationV1::denied;
    fixture.authority->mga =
        api::TypedResultProducerMgaObservationV1::stale_or_unequal;
    fixture.authority->descriptor_live = false;
    auto request = PullRequest();
    request.maximum_rows = 0;
    auto result =
        api::PullTypedResultProducerCursorV1(*fixture.carrier, request);
    Require(!result.ok() &&
                result.diagnostic_code ==
                    "SB_ENGINE_STATUS_INVALID_ARGUMENT" &&
                fixture.producer->stage_calls == 0,
            "pull malformed-request precedence drifted");
    request = PullRequest();
    result = api::PullTypedResultProducerCursorV1(*fixture.carrier, request);
    Require(!result.ok() && result.diagnostic_code == "SECURITY.ACCESS_DENIED" &&
                fixture.producer->stage_calls == 0,
            "owner refusal did not precede MGA/descriptor refusal");
    RequireReleasedOnce(fixture, "owner revocation");
  }
  {
    auto fixture = Opened();
    fixture.authority->mga =
        api::TypedResultProducerMgaObservationV1::stale_or_unequal;
    fixture.authority->descriptor_live = false;
    auto request = PullRequest();
    request.cursor_uuid = Uuid(0xfe);
    const auto result =
        api::PullTypedResultProducerCursorV1(*fixture.carrier, request);
    Require(!result.ok() && result.diagnostic_code == "MGA.TRANSACTION.STALE" &&
                fixture.producer->stage_calls == 0,
            "MGA refusal did not precede descriptor/cursor refusal");
    RequireReleasedOnce(fixture, "MGA revocation");
  }
  {
    auto fixture = Opened();
    fixture.authority->descriptor_live = false;
    auto request = PullRequest();
    request.cursor_uuid = Uuid(0xfe);
    const auto result =
        api::PullTypedResultProducerCursorV1(*fixture.carrier, request);
    Require(!result.ok() &&
                result.diagnostic_code == "DATATYPE.DESCRIPTOR_INVALID" &&
                fixture.producer->stage_calls == 0,
            "descriptor refusal did not precede cursor refusal");
    RequireReleasedOnce(fixture, "descriptor revocation");
  }
  {
    auto fixture = Opened();
    fixture.authority->grant =
        api::TypedResultProducerGrantObservationV1::exhausted;
    fixture.authority->SetCancellationScript(
        {api::TypedResultProducerCancellationObservationV1::requested});
    auto request = PullRequest();
    request.cursor_uuid = Uuid(0xfe);
    auto result =
        api::PullTypedResultProducerCursorV1(*fixture.carrier, request);
    Require(!result.ok() && result.diagnostic_code == "CURSOR.STALE" &&
                fixture.producer->stage_calls == 0,
            "cursor refusal did not precede grant/cancellation refusal");
    request = PullRequest();
    result = api::PullTypedResultProducerCursorV1(*fixture.carrier, request);
    Require(!result.ok() &&
                result.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" &&
                fixture.producer->stage_calls == 0,
            "resource refusal did not precede cancellation");
    fixture.authority->grant =
        api::TypedResultProducerGrantObservationV1::live;
    result = api::PullTypedResultProducerCursorV1(*fixture.carrier, request);
    Require(!result.ok() && result.diagnostic_code == "PROCESS.CANCELLED" &&
                fixture.producer->stage_calls == 0,
            "admitted cancellation did not precede producer entry");
    RequireReleasedOnce(fixture, "cancellation refusal");
  }
  {
    auto fixture = Opened();
    api::TypedResultProducerStageResultV1 refused;
    refused.outcome = api::TypedResultProducerStageOutcomeV1::refused;
    fixture.producer->staged.push_back(std::move(refused));
    const auto result = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!result.ok() && result.diagnostic_code == "CURSOR.FETCH_FAILED" &&
                fixture.producer->stage_calls == 1 &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::open,
            "producer refusal mapping or nonterminal rollback drifted");
  }
}

void PostStageRefusalPrecedence() {
  {
    auto fixture = Opened();
    api::TypedResultProducerStageResultV1 refused;
    refused.outcome = api::TypedResultProducerStageOutcomeV1::refused;
    fixture.producer->staged.push_back(std::move(refused));
    fixture.producer->on_stage = [authority = fixture.authority](const auto&) {
      authority->owner = api::TypedResultProducerOwnerObservationV1::denied;
      authority->mga =
          api::TypedResultProducerMgaObservationV1::stale_or_unequal;
      authority->descriptor_live = false;
    };
    const auto result = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!result.ok() && result.diagnostic_code == "SECURITY.ACCESS_DENIED",
            "post-stage owner invalidation lost precedence to producer failure");
    RequireReleasedOnce(fixture, "post-stage owner invalidation");
  }
  {
    auto fixture = Opened();
    api::TypedResultProducerStageResultV1 refused;
    refused.outcome = api::TypedResultProducerStageOutcomeV1::refused;
    fixture.producer->staged.push_back(std::move(refused));
    fixture.producer->on_stage = [authority = fixture.authority](const auto&) {
      authority->descriptor_live = false;
      authority->receipt = api::TypedResultProducerReceiptObservationV1::stale;
      authority->grant =
          api::TypedResultProducerGrantObservationV1::exhausted;
    };
    const auto result = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!result.ok() &&
                result.diagnostic_code == "DATATYPE.DESCRIPTOR_INVALID",
            "post-stage descriptor invalidation lost refusal precedence");
    RequireReleasedOnce(fixture, "post-stage descriptor invalidation");
  }
  {
    auto fixture = Opened();
    api::TypedResultProducerStageResultV1 refused;
    refused.outcome = api::TypedResultProducerStageOutcomeV1::refused;
    fixture.producer->staged.push_back(std::move(refused));
    fixture.producer->on_stage = [authority = fixture.authority](const auto&) {
      authority->receipt = api::TypedResultProducerReceiptObservationV1::stale;
      authority->grant =
          api::TypedResultProducerGrantObservationV1::exhausted;
    };
    const auto result = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!result.ok() && result.diagnostic_code == "CURSOR.STALE",
            "post-stage receipt invalidation lost precedence to resource/fetch");
    RequireReleasedOnce(fixture, "post-stage receipt invalidation");
  }
  {
    auto fixture = Opened();
    api::TypedResultProducerStageResultV1 cancelled;
    cancelled.outcome = api::TypedResultProducerStageOutcomeV1::cancelled;
    fixture.producer->staged.push_back(std::move(cancelled));
    fixture.authority->SetCancellationScript(
        {api::TypedResultProducerCancellationObservationV1::live,
         api::TypedResultProducerCancellationObservationV1::stale});
    fixture.producer->on_stage = [](const auto& request) {
      Require(request.cancellation_requested &&
                  request.cancellation_requested(),
              "stale safe-point receipt was not observed");
    };
    const auto result = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!result.ok() && result.diagnostic_code == "CURSOR.STALE" &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::revoked,
            "stale cancellation receipt was misreported as cancellation");
    RequireReleasedOnce(fixture, "stale safe-point receipt");
  }
}

void BoundedPullAtomicPublicationAndEos() {
  auto fixture = Opened();
  fixture.producer->staged.push_back(
      Batch({Row(0, "first=x;y"), Row(1, "second=")}));
  fixture.producer->staged.push_back(EmptyOpen());
  fixture.producer->staged.push_back(Batch({Row(0, "terminal")}, true));

  auto over = PullRequest();
  over.maximum_rows = 9;
  auto result = api::PullTypedResultProducerCursorV1(*fixture.carrier, over);
  Require(!result.ok() &&
              result.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" &&
              fixture.producer->stage_calls == 0,
          "over-bound pull entered producer");

  result = api::PullTypedResultProducerCursorV1(
      *fixture.carrier, PullRequest());
  Require(result.ok() &&
              result.outcome == api::TypedResultProducerPullOutcomeV1::batch &&
              result.row_count == 2 && !result.row_data_packet.empty() &&
              result.batch.batch_ordinal == 0 && result.batch.cursor_bound,
          "first typed batch was not published exactly");
  auto snapshot = fixture.carrier->Snapshot();
  Require(snapshot.row_position == 2 && snapshot.next_batch_ordinal == 1 &&
              snapshot.lifecycle ==
                  api::TypedResultProducerCursorLifecycleV1::open,
          "first publication did not atomically advance position and ordinal");

  result = api::PullTypedResultProducerCursorV1(
      *fixture.carrier, PullRequest(1));
  Require(result.ok() &&
              result.outcome ==
                  api::TypedResultProducerPullOutcomeV1::empty_open &&
              result.row_data_packet.empty(),
          "empty-open polling result drifted");
  snapshot = fixture.carrier->Snapshot();
  Require(snapshot.row_position == 2 && snapshot.next_batch_ordinal == 1,
          "empty-open poll advanced state");

  result = api::PullTypedResultProducerCursorV1(
      *fixture.carrier, PullRequest(1));
  Require(result.ok() && result.end_of_cursor && result.row_count == 1 &&
              fixture.carrier->Snapshot().lifecycle ==
                  api::TypedResultProducerCursorLifecycleV1::eos &&
              fixture.carrier->Snapshot().row_position == 3 &&
              fixture.carrier->Snapshot().next_batch_ordinal == 2,
          "terminal batch did not publish before EOS");
  RequireReleasedOnce(fixture, "batch EOS");
  const auto replay = api::PullTypedResultProducerCursorV1(
      *fixture.carrier, PullRequest(2));
  Require(!replay.ok() && replay.diagnostic_code == "CURSOR.STALE" &&
              fixture.producer->stage_calls == 3,
          "post-EOS pull entered producer or changed diagnostic");
  RequireReleasedOnce(fixture, "post-EOS replay");

  auto malformed = Opened();
  malformed.producer->staged.push_back(Batch({Row(7, "bad-ordinal")}));
  const auto first_refusal = api::PullTypedResultProducerCursorV1(
      *malformed.carrier, PullRequest());
  const auto second_refusal = api::PullTypedResultProducerCursorV1(
      *malformed.carrier, PullRequest());
  const auto malformed_snapshot = malformed.carrier->Snapshot();
  Require(!first_refusal.ok() && !second_refusal.ok() &&
              first_refusal.diagnostic_code == "DATATYPE.DESCRIPTOR_INVALID" &&
              second_refusal.diagnostic_code == first_refusal.diagnostic_code &&
              malformed.producer->stage_calls == 2 &&
              malformed.producer->next == 0 &&
              malformed.producer->stage_aborts == 2 &&
              malformed_snapshot.row_position == 0 &&
              malformed_snapshot.next_batch_ordinal == 0 &&
              malformed_snapshot.lifecycle ==
                  api::TypedResultProducerCursorLifecycleV1::open,
          "malformed staged batch was not identically replayable after abort");
}

void EmptyEosAndCancellationBarriers() {
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(EmptyEos());
    const auto result = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(result.ok() &&
                result.outcome ==
                    api::TypedResultProducerPullOutcomeV1::empty_eos &&
                result.end_of_cursor && result.row_count == 0 &&
                result.row_data_packet.empty() &&
                fixture.carrier->Snapshot().next_batch_ordinal == 0 &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::eos,
            "empty EOS advanced sequence or failed terminal release");
    RequireReleasedOnce(fixture, "empty EOS");
  }
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "discard-me")}));
    fixture.authority->SetCancellationScript(
        {api::TypedResultProducerCancellationObservationV1::live,
         api::TypedResultProducerCancellationObservationV1::requested});
    const auto result = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!result.ok() && result.diagnostic_code == "PROCESS.CANCELLED" &&
                result.row_data_packet.empty() &&
                fixture.carrier->Snapshot().row_position == 0 &&
                fixture.carrier->Snapshot().next_batch_ordinal == 0 &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::cancelled,
            "pre-publication cancellation exposed or advanced a batch");
    RequireReleasedOnce(fixture, "pre-publication cancellation");
  }
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "published")}));
    fixture.authority->SetCancellationScript(
        {api::TypedResultProducerCancellationObservationV1::live,
         api::TypedResultProducerCancellationObservationV1::live,
         api::TypedResultProducerCancellationObservationV1::requested});
    const auto result = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(result.ok() && result.row_count == 1 &&
                !result.row_data_packet.empty() &&
                fixture.carrier->Snapshot().row_position == 1 &&
                fixture.carrier->Snapshot().next_batch_ordinal == 1 &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::open,
            "a post-barrier observation changed the committed publication");
    const auto cancelled = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest(1));
    Require(!cancelled.ok() &&
                cancelled.diagnostic_code == "PROCESS.CANCELLED" &&
                fixture.producer->stage_calls == 1 &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::cancelled,
            "next-operation cancellation did not win before producer entry");
    RequireReleasedOnce(fixture, "next-operation cancellation");
  }
  {
    auto fixture = Opened();
    fixture.authority->SetCancellationScript(
        {api::TypedResultProducerCancellationObservationV1::live,
         api::TypedResultProducerCancellationObservationV1::requested});
    fixture.producer->on_stage = [](const auto& request) {
      Require(request.cancellation_requested &&
                  request.cancellation_requested(),
              "producer safe-point probe missed cancellation");
    };
    api::TypedResultProducerStageResultV1 cancelled;
    cancelled.outcome = api::TypedResultProducerStageOutcomeV1::cancelled;
    fixture.producer->staged.push_back(std::move(cancelled));
    const auto result = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!result.ok() && result.diagnostic_code == "PROCESS.CANCELLED" &&
                fixture.carrier->Snapshot().row_position == 0,
            "producer safe-point cancellation did not discard staged state");
    RequireReleasedOnce(fixture, "safe-point cancellation");
  }
}

void PrecommitGateAbortAndRetry() {
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(
        Batch({Row(0, "gate-retry")}, true));
    std::vector<std::uint8_t> staged_packet;
    const auto refused = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest(),
        [&](const api::TypedResultProducerPullResultV1& candidate) {
          staged_packet = candidate.row_data_packet;
          api::TypedResultProducerPrecommitDecisionV1 decision;
          decision.refusal_status =
              api::TypedResultProducerCursorStatusV1::fetch_failed;
          decision.diagnostic_code = "CURSOR.FETCH_FAILED";
          decision.detail = "fixture_outer_encode_refused";
          return decision;
        });
    const auto after_refusal = fixture.carrier->Snapshot();
    Require(!refused.ok() && refused.row_data_packet.empty() &&
                refused.detail == "fixture_outer_encode_refused" &&
                !staged_packet.empty() && fixture.producer->next == 0 &&
                fixture.producer->stage_commits == 0 &&
                fixture.producer->stage_aborts == 1 &&
                after_refusal.row_position == 0 &&
                after_refusal.next_batch_ordinal == 0 &&
                after_refusal.lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::open,
            "precommit refusal consumed source or carrier state");

    const auto retried = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(retried.ok() && retried.row_data_packet == staged_packet &&
                retried.end_of_cursor && fixture.producer->next == 1 &&
                fixture.producer->stage_commits == 1 &&
                fixture.carrier->Snapshot().row_position == 1 &&
                fixture.carrier->Snapshot().next_batch_ordinal == 1,
            "precommit abort did not preserve an identical retry batch");
    RequireReleasedOnce(fixture, "precommit retry EOS");
  }
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "gate-throw")}));
    const auto refused = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest(),
        [](const api::TypedResultProducerPullResultV1&)
            -> api::TypedResultProducerPrecommitDecisionV1 {
          throw std::runtime_error("fixture gate exception");
        });
    Require(!refused.ok() &&
                refused.diagnostic_code == "CURSOR.FETCH_FAILED" &&
                refused.detail == "precommit_gate_exception" &&
                fixture.producer->next == 0 &&
                fixture.producer->stage_commits == 0 &&
                fixture.producer->stage_aborts == 1 &&
                fixture.carrier->Snapshot().row_position == 0 &&
                fixture.carrier->Snapshot().next_batch_ordinal == 0,
            "precommit exception consumed source or carrier state");
  }
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "post-gate-grant")}));
    const auto refused = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest(),
        [authority = fixture.authority](const auto&) {
          authority->grant =
              api::TypedResultProducerGrantObservationV1::exhausted;
          api::TypedResultProducerPrecommitDecisionV1 decision;
          decision.accepted = true;
          return decision;
        });
    Require(!refused.ok() &&
                refused.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" &&
                fixture.producer->next == 0 &&
                fixture.producer->stage_commits == 0 &&
                fixture.producer->stage_aborts == 1 &&
                fixture.carrier->Snapshot().row_position == 0 &&
                fixture.carrier->Snapshot().next_batch_ordinal == 0,
            "post-gate grant revalidation did not abort atomically");
    fixture.authority->grant =
        api::TypedResultProducerGrantObservationV1::live;
    const auto retried = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(retried.ok() && retried.row_count == 1 &&
                fixture.producer->stage_commits == 1,
            "post-gate grant refusal did not leave the batch retryable");
  }
}

void StageLeaseShapeValidation() {
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "missing-lease")}));
    fixture.producer->omit_next_lease = true;
    const auto refused = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!refused.ok() &&
                refused.diagnostic_code ==
                    "SB_ENGINE_STATUS_INVALID_ARGUMENT" &&
                refused.detail == "required_stage_lease_absent" &&
                fixture.producer->next == 0 &&
                fixture.producer->stage_commits == 0 &&
                fixture.carrier->Snapshot().row_position == 0,
            "batch without an owned lease crossed the Stage boundary");
  }
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(EmptyEos());
    fixture.producer->omit_next_lease = true;
    const auto refused = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!refused.ok() &&
                refused.detail == "required_stage_lease_absent" &&
                fixture.producer->next == 0 &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::open,
            "empty EOS without an owned lease terminalized the cursor");
  }
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(EmptyOpen());
    fixture.producer->force_next_lease = true;
    const auto refused = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!refused.ok() &&
                refused.detail == "unexpected_stage_lease_present" &&
                fixture.producer->next == 0 &&
                fixture.producer->stage_aborts == 1 &&
                fixture.carrier->Snapshot().row_position == 0,
            "empty-open result carried a mutating lease");
    const auto retried = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(retried.ok() &&
                retried.outcome ==
                    api::TypedResultProducerPullOutcomeV1::empty_open &&
                fixture.producer->next == 1,
            "lease-shape refusal changed the empty-open retry");
  }
}

void SourceCommitRefusalTransitions() {
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "commit-cancel")}));
    fixture.producer->next_commit_status =
        api::TypedResultProducerStageCommitStatusV1::cancelled;
    const auto refused = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!refused.ok() &&
                refused.diagnostic_code == "PROCESS.CANCELLED" &&
                refused.outcome ==
                    api::TypedResultProducerPullOutcomeV1::cancelled &&
                refused.row_data_packet.empty() &&
                fixture.producer->stage_aborts == 1 &&
                fixture.carrier->Snapshot().row_position == 0 &&
                fixture.carrier->Snapshot().next_batch_ordinal == 0 &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::cancelled,
            "last-instant source cancellation remained retryable");
    RequireReleasedOnce(fixture, "source commit cancellation");
  }
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "commit-stale")}));
    fixture.producer->next_commit_status =
        api::TypedResultProducerStageCommitStatusV1::stale;
    const auto refused = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!refused.ok() && refused.diagnostic_code == "CURSOR.STALE" &&
                refused.row_data_packet.empty() &&
                fixture.producer->stage_aborts == 1 &&
                fixture.carrier->Snapshot().row_position == 0 &&
                fixture.carrier->Snapshot().next_batch_ordinal == 0 &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::revoked,
            "last-instant source authority loss remained open");
    RequireReleasedOnce(fixture, "source commit authority loss");
  }
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "commit-budget")}));
    fixture.producer->next_commit_status =
        api::TypedResultProducerStageCommitStatusV1::resource_budget_exceeded;
    const auto refused = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!refused.ok() &&
                refused.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" &&
                fixture.producer->next == 0 &&
                fixture.producer->stage_aborts == 1 &&
                fixture.carrier->Snapshot().row_position == 0 &&
                fixture.carrier->Snapshot().lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::open,
            "source publication budget refusal changed cursor state");
    fixture.producer->next_commit_status =
        api::TypedResultProducerStageCommitStatusV1::committed;
    const auto retried = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(retried.ok() && retried.row_count == 1 &&
                fixture.producer->stage_commits == 1,
            "narrower source publication retry did not remain available");
  }
}

void RecoveryAndTerminalRace() {
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "must-not-run")}));
    const auto recovered = api::CloseTypedResultProducerCursorV1(
        *fixture.carrier,
        api::TypedResultProducerCloseReasonV1::recovery);
    Require(recovered.ok() &&
                recovered.lifecycle ==
                    api::TypedResultProducerCursorLifecycleV1::revoked &&
                fixture.producer->stage_calls == 0,
            "recovery invoked or reconstructed producer state");
    RequireReleasedOnce(fixture, "recovery revocation");
    const auto replay = api::PullTypedResultProducerCursorV1(
        *fixture.carrier, PullRequest());
    Require(!replay.ok() && replay.diagnostic_code == "CURSOR.STALE" &&
                fixture.producer->stage_calls == 0,
            "recovered carrier replay entered producer");
    RequireReleasedOnce(fixture, "recovery replay");
  }
  {
    auto fixture = Opened();
    fixture.producer->staged.push_back(Batch({Row(0, "race-eos")}, true));
    std::barrier gate(3);
    api::TypedResultProducerPullResultV1 pull;
    api::TypedResultProducerOperationResultV1 close;
    std::thread pull_thread([&] {
      gate.arrive_and_wait();
      pull = api::PullTypedResultProducerCursorV1(
          *fixture.carrier, PullRequest());
    });
    std::thread close_thread([&] {
      gate.arrive_and_wait();
      close = api::CloseTypedResultProducerCursorV1(
          *fixture.carrier,
          api::TypedResultProducerCloseReasonV1::explicit_close);
    });
    gate.arrive_and_wait();
    pull_thread.join();
    close_thread.join();

    Require(static_cast<int>(pull.ok()) + static_cast<int>(close.ok()) == 1,
            "EOS/close race admitted zero or two terminal winners");
    const auto snapshot = fixture.carrier->Snapshot();
    Require((snapshot.lifecycle ==
                 api::TypedResultProducerCursorLifecycleV1::eos ||
             snapshot.lifecycle ==
                 api::TypedResultProducerCursorLifecycleV1::closed) &&
                snapshot.retained_authority_released,
            "EOS/close race left a nonterminal carrier");
    if (pull.ok()) {
      Require(pull.row_count == 1 && close.diagnostic_code == "CURSOR.STALE",
              "EOS winner did not publish exactly one terminal batch");
    } else {
      Require(close.ok() && pull.diagnostic_code == "CURSOR.STALE" &&
                  pull.row_data_packet.empty(),
              "close winner allowed a partial batch");
    }
    RequireReleasedOnce(fixture, "EOS/close race");
  }
}

}  // namespace

int main() {
  try {
    OpenRetainsAndCloseReleasesExactlyOnce();
    OpenAndPullRefusalPrecedence();
    PostStageRefusalPrecedence();
    BoundedPullAtomicPublicationAndEos();
    EmptyEosAndCancellationBarriers();
    PrecommitGateAbortAndRetry();
    StageLeaseShapeValidation();
    SourceCommitRefusalTransitions();
    RecoveryAndTerminalRace();
    std::cout << "typed result producer cursor runtime conformance passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "typed result producer cursor runtime conformance failed: "
              << error.what() << '\n';
    return 1;
  }
}
