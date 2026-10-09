// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Linux syscall-level regression of real bootstrap catalog page publication.
#include "database_lifecycle.hpp"
#include "catalog_page.hpp"
#include "disk_device.hpp"
#include "page_header.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"
#include "memory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
bool observe = false;
unsigned page_size = 0, body_writes = 0;
unsigned overflow_calls = 0, maximum_batch_pages = 0;
enum class Fault { none, write_error, partial_write, partial_remainder, process_exit };
Fault fault = Fault::none;
std::map<off_t, std::vector<platform::byte>> images;
std::map<off_t, unsigned> full_writes;
platform::TypedUuid Id(platform::UuidKind kind) {
  const auto result = uuid::GenerateDurableEngineIdentityV7(kind, 1790000000100ull);
  Check(result.ok(), "generate binary system identity");
  return result.value;
}
struct Temporary {
  std::filesystem::path root;
  Temporary() {
    auto pattern = (std::filesystem::temp_directory_path() / "sb_catalog_write.XXXXXX").string();
    std::vector<char> text(pattern.begin(), pattern.end()); text.push_back(0);
    const auto made = ::mkdtemp(text.data());
    Check(made != nullptr, "create owned temporary directory"); root = made;
  }
  void Clean() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    Check(!error, "remove owned database and all generated sidecars");
    Check(!std::filesystem::exists(root), "owned test artifacts remained after cleanup");
    root.clear();
  }
  ~Temporary() {
    if (!root.empty()) { std::error_code error; std::filesystem::remove_all(root, error); }
  }
};
db::DatabaseCreateConfig Config(const Temporary& temporary, unsigned size) {
  db::DatabaseCreateConfig config;
  config.path = (temporary.root / "node.sbdb").string();
  config.database_uuid = Id(platform::UuidKind::database);
  config.filespace_uuid = Id(platform::UuidKind::filespace);
  config.page_size = size;
  config.creation_unix_epoch_millis = 1790000000100ull;
  config.resource_seed_pack_root = SB_CATALOG_TEST_SEED_ROOT;
  config.require_bootstrap_principal = false;
  config.allow_uncredentialed_bootstrap = true;
  return config;
}
void Reset(unsigned size, Fault next = Fault::none) {
  page_size = size; body_writes = 0; images.clear(); full_writes.clear();
  overflow_calls = 0; maximum_batch_pages = 0;
  fault = next; observe = true;
}
}

extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t count, off_t offset) {
  if (observe && fault == Fault::partial_remainder) {
    fault = Fault::none; errno = EIO; return -1;
  }
  if (observe && offset >= static_cast<off_t>(db::kCatalogOverflowFirstPageNumber * page_size)) {
    if (images.contains(offset - disk::kPageHeaderSerializedBytes)) ++body_writes;
    if (count >= page_size && count % page_size == 0 && offset % page_size == 0) {
      disk::SerializedPageHeader bytes;
      std::memcpy(bytes.data(), data, bytes.size());
      const auto header = disk::ParsePageHeader(bytes);
      if (header.ok() && header.header.page_type == disk::PageType::catalog) {
        ++overflow_calls;
        maximum_batch_pages = std::max(maximum_batch_pages, static_cast<unsigned>(count / page_size));
        const auto* begin = static_cast<const platform::byte*>(data);
        for (size_t position = 0; position < count; position += page_size) {
          disk::SerializedPageHeader current;
          std::memcpy(current.data(), begin + position, current.size());
          const auto parsed = disk::ParsePageHeader(current);
          Check(parsed.ok() && parsed.header.page_type == disk::PageType::catalog &&
                    parsed.header.page_number == (offset + position) / page_size,
                "coalesced page header lost exact physical identity");
          ++full_writes[offset + position];
          images[offset + position] = {begin + position, begin + position + page_size};
        }
        const auto selected = fault;
        fault = Fault::none;
        if (selected == Fault::write_error) { errno = EIO; return -1; }
        if (selected == Fault::partial_write) {
          fault = Fault::partial_remainder;
          return __real_pwrite(fd, data, count / 2 + (count > page_size ? page_size / 2 : 0), offset);
        }
        if (selected == Fault::process_exit) {
          const auto written = __real_pwrite(fd, data, count, offset);
          ::_exit(written == static_cast<ssize_t>(count) ? 74 : 75);
        }
      }
    }
  }
  return __real_pwrite(fd, data, count, offset);
}

int main(int argc, char** argv) try {
  namespace memory = scratchbird::core::memory;
  memory::AllocationPolicy policy;
  policy.policy_name = "catalog_initial_page_write_test";
  policy.hard_limit_bytes = 128ull * 1024 * 1024;
  policy.soft_limit_bytes = 96ull * 1024 * 1024;
  policy.per_context_limit_bytes = 64ull * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 32ull * 1024 * 1024;
  unsigned bounded_pages = 0;
  const bool allocation_faults = argc == 2 && std::string_view(argv[1]) == "--allocation-failures";
  if (argc == 2 && std::string_view(argv[1]) == "--one-pages") bounded_pages = 1;
  else if (argc == 2 && std::string_view(argv[1]) == "--five-pages") bounded_pages = 5;
  else Check(argc == 1 || allocation_faults, "unknown catalog batching test option");
  if (bounded_pages) policy.page_buffer_pool_limit_bytes = bounded_pages * 8192;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  const auto memory_configured = memory::ConfigureDefaultMemoryManager(policy, "catalog_write_regression");
  Check(memory_configured.ok() && !memory_configured.fixture_mode, "configure admitted memory policy");
  if (allocation_faults) {
    for (unsigned sequence : {2u, 3u}) {
      Temporary temporary;
      const auto config = Config(temporary, 8192);
      Reset(8192);
      memory::MemoryFailureInjectionConfiguration injection;
      injection.test_guard = memory::MakeMemoryFailureInjectionTestGuard();
      injection.fixture_enabled = true;
      injection.fixture_name = "catalog_batch_admission_failure";
      memory::MemoryFailureInjectionRule rule;
      rule.rule_id = "catalog_image_batch";
      rule.purpose = "catalog_page_body_write";
      rule.category = memory::MemoryCategory::page_buffer;
      rule.fail_on_matched_sequence = sequence;
      injection.rules.push_back(rule);
      Check(memory::DefaultMemoryManager().EnableAllocationFailureInjection(injection).ok(),
            "enable exact catalog-buffer allocation failure");
      const auto created = db::CreateDatabaseFile(config);
      observe = false;
      const auto fault_state = memory::DefaultMemoryManager().FailureInjectionSnapshot();
      Check(memory::DefaultMemoryManager().DisableAllocationFailureInjection().ok(),
            "disable catalog-buffer allocation failure");
      Check(fault_state.rules.size() == 1 && fault_state.rules[0].failure_count == 1 &&
                fault_state.rules[0].matched_sequence == sequence,
            "catalog allocation failure was skipped or retried");
      Check(!created.ok() && created.create_finality == db::DatabaseCreateFinalityClass::not_published &&
                !std::filesystem::exists(config.path) &&
                memory::DefaultMemoryManager().Snapshot().page_buffer_current_bytes == 0,
            "catalog allocation refusal leaked buffers or published partial work");
      Check(images.size() == (sequence == 2 ? 0 : 16),
            "allocation refusal changed the exact physical batch prefix");
      temporary.Clean();
    }
    std::cout << "catalog_batch_allocation_failures checks=" << checks << " failures=0\n";
    return 0;
  }
  for (unsigned size : {8192u, 16384u, 32768u, 65536u, 131072u}) {
    if (bounded_pages && size != 8192) continue;
    Temporary temporary;
    const auto config = Config(temporary, size);
    Reset(size);
    const auto created = db::CreateDatabaseFile(config);
    observe = false;
    if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << '\n';
    Check(created.ok() && created.create_finality != db::DatabaseCreateFinalityClass::not_published,
          "real resource-seeded database creation must commit");
    Check(!images.empty() && body_writes == 0,
          "catalog overflow headers and bodies must use one complete-page write");
    const auto expected_max = bounded_pages ? bounded_pages : 16;
    Check(maximum_batch_pages == expected_max &&
              overflow_calls == (images.size() + expected_max - 1) / expected_max,
          "contiguous overflow writes did not use the exact admitted batch bound");
    Check(memory::DefaultMemoryManager().Snapshot().page_buffer_current_bytes == 0,
          "catalog batch retained page-buffer accounting after create");
    disk::FileDevice device;
    Check(device.Open(config.path, disk::FileOpenMode::open_existing_read_only).ok(),
          "reopen actual catalog file after create");
    for (const auto& [offset, expected] : images) {
      Check(full_writes.at(offset) == 1, "overflow page written more than once during bootstrap");
      std::vector<platform::byte> actual(size);
      Check(device.ReadAt(offset, actual.data(), actual.size()).ok() && actual == expected,
            "reopened complete catalog page bytes differ");
      const std::vector<platform::byte> body(actual.begin() + disk::kPageHeaderSerializedBytes,
                                             actual.end());
      const auto parsed = page::ParseCatalogPageBody(body, offset / size);
      Check(parsed.ok() && !parsed.body.rows.empty(), "catalog body checksum or rows lost");
    }
    Check(device.Close().ok(), "release owned read device");
    db::DatabaseOpenConfig open;
    open.path = config.path; open.read_only = true; open.suppress_background_agents = true;
    const auto reopened = db::OpenDatabaseFile(open);
    Check(reopened.ok() && reopened.state.typed_catalog_record_count == created.state.typed_catalog_record_count
              && reopened.state.database_uuid.value == config.database_uuid.value,
          "reopen lost catalog records or binary database identity");
    temporary.Clean();
  }
  for (unsigned size : {8192u, 16384u, 32768u, 65536u, 131072u}) {
   if (bounded_pages && size != 8192) continue;
   for (const auto injected : {Fault::write_error, Fault::partial_write, Fault::process_exit}) {
    Temporary temporary;
    const auto config = Config(temporary, size);
    Reset(size, injected);
    if (injected == Fault::process_exit) {
      const auto child = ::fork(); Check(child >= 0, "fork crash probe");
      if (child == 0) { (void)db::CreateDatabaseFile(config); ::_exit(76); }
      int status = 0;
      Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 74,
            "crash cut must occur after a real overflow page write");
      observe = false;
      db::DatabaseOpenConfig open;
      open.path = config.path; open.read_only = true; open.suppress_background_agents = true;
      Check(!db::OpenDatabaseFile(open).ok(), "partial bootstrap exposed as a committed database");
    } else {
      const auto result = db::CreateDatabaseFile(config);
      observe = false;
      Check(fault == Fault::none && !result.ok() &&
                result.create_finality == db::DatabaseCreateFinalityClass::not_published &&
                !result.diagnostic.diagnostic_code.empty(),
            "physical write failure became published success");
      Check(!std::filesystem::exists(config.path), "failed fresh creation retained an admitted main file");
    }
    temporary.Clean();
   }
  }
  std::cout << "catalog_initial_page_write checks=" << checks
            << " profiles=" << (bounded_pages ? 1 : 5)
            << " fault_cases=" << (bounded_pages ? 3 : 15)
            << " batch_page_limit=" << (bounded_pages ? bounded_pages : 16)
            << " failures=0\n";
  return 0;
} catch (const std::exception& error) {
  observe = false;
  std::cerr << "FAIL " << error.what() << '\n';
  return 1;
}
