#include "sblr_error_vector_descriptor_registry.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace scratchbird::engine::internal_api;

static std::string Identity(scratchbird::core::platform::UuidKind kind) {
  static std::uint64_t tick = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  auto value = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, ++tick);
  assert(value.ok());
  return scratchbird::core::uuid::UuidToString(value.value.value);
}

int main() {
  const auto base = std::filesystem::temp_directory_path() /
                    ("sb_error_vector_registry_" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto journal = base.string() + ".sb.sblr_error_vector_registry.v1";
  std::error_code ec;
  std::filesystem::remove(journal, ec);

  EngineRequestContext context;
  context.database_path = base.string();
  context.database_uuid.canonical = Identity(scratchbird::core::platform::UuidKind::database);
  context.session_uuid.canonical = Identity(scratchbird::core::platform::UuidKind::object);
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags = {"private_error_vector_registry"};

  scratchbird::engine::sblr::SblrErrorVectorEntryV1 entry;
  entry.occurrence_ordinal = 1;
  auto diagnostic = scratchbird::core::uuid::ParseUuid(
      Identity(scratchbird::core::platform::UuidKind::object));
  assert(diagnostic.ok());
  std::copy(diagnostic.value.bytes.begin(), diagnostic.value.bytes.end(),
            entry.diagnostic_uuid.begin());
  entry.diagnostic_generation = 1;
  entry.precedence_ordinal = 1;
  entry.severity_code = 1;
  entry.redaction_class = 1;

  const auto receipt = Identity(scratchbird::core::platform::UuidKind::object);
  const auto registry = Identity(scratchbird::core::platform::UuidKind::object);
  const auto diagnostics = Identity(scratchbird::core::platform::UuidKind::object);
  auto issued = IssueSblrErrorVectorDescriptorV1(
      context, receipt, registry, 1, diagnostics, 1, {entry});
  assert(issued.ok && !issued.snapshot.canonical_ervd.empty());
  assert(LookupSblrErrorVectorDescriptorV1(
      context, receipt, issued.snapshot.descriptor_uuid, 1).ok);

  auto admin = context;
  admin.trace_tags = {"right:SBLR_ERROR_VECTOR_REGISTRY_ADMIN"};
  assert(RecoverSblrErrorVectorDescriptorRegistryV1(admin).code == "OK");
  assert(!LookupSblrErrorVectorDescriptorV1(
      context, receipt, issued.snapshot.descriptor_uuid, 1).ok);

  std::ifstream input(journal, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto valid = contents.str();
  assert(!valid.empty());

  {
    std::ofstream output(journal, std::ios::binary | std::ios::app);
    output << "E\ttruncated\n";
  }
  assert(RecoverSblrErrorVectorDescriptorRegistryV1(admin).code ==
         "SBLR.ERROR_VECTOR.STALE");

  {
    auto corrupt = valid;
    const auto evidence = corrupt.find("sha256:");
    assert(evidence != std::string::npos);
    corrupt[evidence + 7] = corrupt[evidence + 7] == '0' ? '1' : '0';
    std::ofstream output(journal, std::ios::binary | std::ios::trunc);
    output << corrupt;
  }
  assert(RecoverSblrErrorVectorDescriptorRegistryV1(admin).code ==
         "SBLR.ERROR_VECTOR.STALE");

  std::filesystem::remove(journal, ec);
}
