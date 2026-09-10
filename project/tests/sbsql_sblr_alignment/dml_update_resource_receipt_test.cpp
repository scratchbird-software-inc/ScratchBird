// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Real database, public engine/session and engine-owned receipt fixture.
#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#include "ia01_package_cancellation_fault_test.cpp"
#include "engine/internal_api/dml/update_resource_authority_provider.hpp"

namespace {
struct ResourceReceipt {
  bridge::StatementContextReceiptHandle handle;
  api::EngineRequestContext context;
  std::shared_ptr<api::EngineDmlUpdateResourceReceiptV1> capability;
};
ResourceReceipt AcquireResourceReceipt(PublicSession& session, const api::EngineRequestContext& context) {
  ResourceReceipt result;
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context;
  request.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptView view;
  Require(bridge::AcquireStatementContextReceipt(session.session, &request, &result.handle, &view, nullptr) ==
              SB_ENGINE_STATUS_OK, "resource receipt acquisition failed");
  Require(bridge::CopyStatementContextEngineContextV1(result.handle, &result.context, nullptr) ==
              SB_ENGINE_STATUS_OK, "resource receipt context copy failed");
  result.capability = result.context.dml_update_resource_receipt.lock();
  Require(result.capability != nullptr, "public receipt omitted engine resource capability");
  return result;
}
void RefuseCallerCapability(PublicSession& session, api::EngineRequestContext context,
    const std::weak_ptr<api::EngineDmlUpdateResourceReceiptV1>& capability) {
  context.dml_update_resource_receipt = capability;
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context;
  request.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  Require(bridge::AcquireStatementContextReceipt(session.session, &request, &receipt, &view, nullptr) ==
              SB_ENGINE_STATUS_CONFLICT && !receipt, "caller-supplied resource capability was accepted");
}
}

int main() {
  using Reason = api::EngineDmlUpdateResourceReleaseV1;
  using Code = api::EngineDmlUpdateResourceReleaseCodeV1;
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  // This fixture supplies the trusted, materialized engine context. It is not
  // a password/login or privilege-provider test.
  context.authorization_context.security_context_generation = 1;
  context.query_cancellation_requested = [] { return false; };
  auto first = AcquireResourceReceipt(session, context);
  auto second = AcquireResourceReceipt(session, context);
  Require(first.capability != second.capability, "receipts shared one revocation capability");
  RefuseCallerCapability(session, context, first.context.dml_update_resource_receipt);
  Require(!first.capability->Capture(second.context).ok, "resource capability crossed receipts");
  std::vector<api::EngineDmlUpdateResourceCaptureV1> grants;
  for (unsigned i = 0; i < 16; ++i) {
    const auto& receipt = i % 2 ? second : first;
    auto grant = receipt.capability->Capture(receipt.context);
    if (!grant.ok) std::cerr << grant.diagnostic.code << ':' << grant.diagnostic.detail << '\n';
    Require(grant.ok, "engine runtime resource reservation failed");
    Require(grant.handle.carrier()->maximum_total_canonical_value_bytes == 16ull * 1024 * 1024 &&
                grant.handle.carrier()->exact_bytes.size() == 208, "runtime policy/carrier mismatch");
    grants.push_back(std::move(grant));
  }
  Require(!first.capability->Capture(first.context).ok && !second.capability->Capture(second.context).ok,
          "receipts did not share aggregate engine capacity");
  PublicSession independent(fixture);
  {
    auto third = AcquireResourceReceipt(independent, context);
    auto grant = third.capability->Capture(third.context);
    Require(grant.ok, "independent engine inherited another instance's exhausted pool");
    Require(third.capability->ReleaseNoAlloc(third.context, grant.handle, Reason::abandoned_before_publication) ==
                Code::released, "independent engine resource cleanup failed");
    Require(bridge::ReleaseStatementContextReceipt(third.handle) == SB_ENGINE_STATUS_OK,
            "independent receipt cleanup failed");
  }
  Require(bridge::ReleaseStatementContextReceipt(first.handle) == SB_ENGINE_STATUS_OK,
          "first receipt release failed");
  Require(!first.capability->Capture(first.context).ok &&
              first.capability->Revalidate(first.context, grants[0].handle).error,
          "released receipt admitted resource use");
  Require(!second.capability->Capture(second.context).ok,
          "receipt release prematurely returned outstanding grant capacity");
  for (unsigned i = 0; i < grants.size(); ++i) {
    const auto& receipt = i % 2 ? second : first;
    Require(receipt.capability->ReleaseNoAlloc(receipt.context, grants[i].handle, Reason::abandoned_before_publication) ==
                Code::released, "retained resource handle failed cleanup after receipt release");
  }
  auto refill = second.capability->Capture(second.context);
  Require(refill.ok && second.capability->ReleaseNoAlloc(second.context, refill.handle, Reason::abandoned_before_publication) ==
              Code::released, "drained engine pool could not be reused");
  first.capability.reset();
  Require(first.context.dml_update_resource_receipt.expired(), "resource receipt lifetime was cyclic");
  RefuseCallerCapability(session, context, first.context.dml_update_resource_receipt);
  Require(bridge::ReleaseStatementContextReceipt(second.handle) == SB_ENGINE_STATUS_OK,
          "second receipt release failed");

  // A cancellation callback can revoke its own receipt without deadlock or a
  // newly returned live grant; no SQL or durable mutation is executed here.
  ResourceReceipt reentrant;
  bool revoke_on_probe = false;
  context.query_cancellation_requested = [&] {
    if (revoke_on_probe) {
      revoke_on_probe = false;
      Require(bridge::ReleaseStatementContextReceipt(reentrant.handle) == SB_ENGINE_STATUS_OK,
              "reentrant receipt release failed");
    }
    return false;
  };
  reentrant = AcquireResourceReceipt(session, context);
  revoke_on_probe = true;
  auto substituted = reentrant.context;
  substituted.query_cancellation_requested = [] { return false; };
  Require(!reentrant.capability->Capture(substituted).ok,
          "capture survived reentrant receipt revocation");
  reentrant = AcquireResourceReceipt(session, context);
  auto revalidated_grant = reentrant.capability->Capture(reentrant.context);
  Require(revalidated_grant.ok, "revalidation fixture failed capture");
  revoke_on_probe = true;
  Require(reentrant.capability->Revalidate(reentrant.context, revalidated_grant.handle).error,
          "revalidation survived reentrant receipt revocation");
  Require(reentrant.capability->ReleaseNoAlloc(reentrant.context, revalidated_grant.handle,
              Reason::abandoned_before_publication) == Code::released,
          "revalidation revocation prevented resource-only cleanup");

  using Outcome = api::EngineDmlUpdateResourcePublicationOutcomeV1;
  using PublicationCode = api::EngineDmlUpdateResourcePublicationCodeV1;
  reentrant = AcquireResourceReceipt(session, context);
  auto preparing_grant = reentrant.capability->Capture(reentrant.context);
  Require(preparing_grant.ok, "publication revocation fixture failed capture");
  revoke_on_probe = true;
  Require(!reentrant.capability->PrepareDescriptorPublication(reentrant.context, preparing_grant.handle).ok &&
              reentrant.capability->ReleaseNoAlloc(reentrant.context, preparing_grant.handle,
                  Reason::abandoned_before_publication) == Code::released,
          "reentrant revocation admitted or stranded publication preparation");

  // The publication ticket records the journal owner's decision only. This
  // fixture simulates that decision; it does not perform a durable UPDATE.
  context.query_cancellation_requested = [] { return false; };
  auto publishing = AcquireResourceReceipt(session, context);
  auto unrelated = AcquireResourceReceipt(session, context);
  auto bound_grant = publishing.capability->Capture(publishing.context);
  auto nonwrite_grant = publishing.capability->Capture(publishing.context);
  Require(bound_grant.ok && nonwrite_grant.ok, "publication receipt capture failed");
  Require(!unrelated.capability->PrepareDescriptorPublication(publishing.context, bound_grant.handle).ok &&
              !unrelated.capability->PrepareDescriptorPublication(unrelated.context, bound_grant.handle).ok,
          "publication preparation crossed receipt owners");
  auto bound = publishing.capability->PrepareDescriptorPublication(publishing.context, bound_grant.handle);
  auto nonwrite = publishing.capability->PrepareDescriptorPublication(publishing.context, nonwrite_grant.handle);
  Require(bound.ok && nonwrite.ok, "publication receipt preparation failed");
  Require(bridge::ReleaseStatementContextReceipt(publishing.handle) == SB_ENGINE_STATUS_OK,
          "prepared publication receipt release failed");
  Require(!publishing.capability->PrepareDescriptorPublication(publishing.context, bound_grant.handle).ok &&
              publishing.capability->ReleaseNoAlloc(publishing.context, bound_grant.handle,
                  Reason::abandoned_before_publication) == Code::phase_mismatch,
          "receipt revocation admitted work or released uncertain publication");
  Require(bound.publication.CompleteNoAlloc(Outcome::published) == PublicationCode::completed &&
              nonwrite.publication.CompleteNoAlloc(Outcome::not_published) == PublicationCode::completed,
          "receipt revocation prevented resource-only publication resolution");
  Require(publishing.capability->ReleaseNoAlloc(publishing.context, bound_grant.handle, Reason::aborted) == Code::released &&
              publishing.capability->ReleaseNoAlloc(publishing.context, nonwrite_grant.handle,
                  Reason::abandoned_before_publication) == Code::released,
          "resolved publication receipt cleanup failed");
  publishing.capability.reset();
  Require(publishing.context.dml_update_resource_receipt.expired(),
          "publication ticket introduced a receipt ownership cycle");
  Require(bridge::ReleaseStatementContextReceipt(unrelated.handle) == SB_ENGINE_STATUS_OK,
          "unrelated publication receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok, "resource receipt MGA cleanup failed");
  std::cout << "resource receipts: shared pool, instance isolation, revocation, caller refusal, prepared publication, cleanup passed\n";
}
