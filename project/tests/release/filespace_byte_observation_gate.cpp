// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../database_lifecycle/filespace_metric_fixture.hpp"
#include "metric_contracts.hpp"
#include <array>
#include <iostream>
#include <limits>

int main() {
  namespace t = filespace_metric_test;
  namespace m = scratchbird::core::metrics;
  try {
    const auto filespace = t::Id(3);
    auto reserved = [&](std::uint64_t value, m::MetricUuid db, m::MetricUuid node) {
      return m::PublishFilespaceReservedBytes(value, db, filespace, node,
          "secondary_data", "file", "preallocated");
    };
    t::Check(reserved(1, t::Id(1), t::Id(2)).diagnostic_code=="SB-METRICS-FAMILY-UNKNOWN",
        "unbound source did not report missing descriptor");
    t::Fixture fixture(4);
    t::Check(!reserved(1, fixture.database, fixture.node).ok, "queue invented descriptors");
    fixture.RegisterDescriptors();
    t::Check(reserved(1, fixture.database, fixture.node).diagnostic_code==
        "METRIC.OBSERVATION_SOURCE_UNAVAILABLE", "descriptor invented series or changed diagnostic");
    t::Check(fixture.queue->Stats().admitted == 0, "missing binding published samples");
    fixture.RegisterFilespace(filespace);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr std::array<std::uint64_t, 7> values = {0, 1, (1ULL<<53)-1,
        (1ULL<<53)+1, (1ULL<<63)+1, maximum-1, maximum};
    for (const auto value : values) {
      t::Check(reserved(value, fixture.database, fixture.node).ok, "exact reserved producer failed");
      fixture.Expect("sb_filespace_reserved_bytes", filespace, value);
      t::Check(m::PublishFilespaceCapacitySnapshot(value, value, 0,
          fixture.database, filespace, fixture.node, "secondary_data", "file").ok,
          "exact capacity producer failed");
      fixture.Expect("sb_filespace_total_bytes", filespace, value);
      fixture.Expect("sb_filespace_used_bytes", filespace, value);
      fixture.Expect("sb_filespace_free_bytes", filespace, 0);
      t::Check(m::PublishFilespaceCapacitySnapshot(value, 0, value,
          fixture.database, filespace, fixture.node, "secondary_data", "file").ok,
          "exact free producer failed");
      fixture.Expect("sb_filespace_total_bytes", filespace, value);
      fixture.Expect("sb_filespace_used_bytes", filespace, 0);
      fixture.Expect("sb_filespace_free_bytes", filespace, value);
    }
    const auto admitted = fixture.queue->Stats().admitted;
    for (const auto bad : {m::MetricUuid{}, t::Id(9)}) {
      t::Check(!reserved(1, bad, fixture.node).ok, "invalid database published");
      t::Check(!reserved(1, fixture.database, bad).ok, "invalid node published");
    }
    t::Check(fixture.queue->Stats().admitted==admitted, "owner refusal emitted samples");
    // Queue saturation must not claim emission or replace the retained value.
    for (std::uint64_t value=0; value<4; ++value)
      t::Check(reserved(value, fixture.database, fixture.node).ok, "queue fill failed");
    const auto before = m::DefaultMetricRegistry().SnapshotCurrent();
    t::Check(reserved(maximum, fixture.database, fixture.node).diagnostic_code==
        "METRIC.OBSERVATION_RESOURCE_EXHAUSTED", "full queue fabricated emission or changed diagnostic");
    const auto after = m::DefaultMetricRegistry().SnapshotCurrent();
    t::Check(before.size()==after.size(), "queue refusal changed series count");
    for (std::size_t i=0; i<before.size(); ++i)
      t::Check(std::get<std::uint64_t>(before[i].value)==std::get<std::uint64_t>(after[i].value),
          "queue refusal replaced retained value");
    t::Check(fixture.queue->Stats().admitted==admitted+4 && fixture.queue->Stats().full==1,
        "queue full admission accounting mismatch");
    for (std::uint64_t value=0; value<4; ++value)
      fixture.Expect("sb_filespace_reserved_bytes", filespace, value);
    // A capacity snapshot is three admissions, not a fabricated atomic batch.
    for (std::uint64_t value=0; value<3; ++value)
      t::Check(reserved(value, fixture.database, fixture.node).ok, "partial test queue fill failed");
    const auto partial = m::PublishFilespaceCapacitySnapshot(8, 3, 5,
        fixture.database, filespace, fixture.node, "secondary_data", "file");
    t::Check(!partial.ok && partial.diagnostic_code=="METRIC.OBSERVATION_RESOURCE_EXHAUSTED",
        "partially admitted capacity snapshot claimed complete success");
    for (std::uint64_t value=0; value<3; ++value)
      fixture.Expect("sb_filespace_reserved_bytes", filespace, value);
    fixture.Expect("sb_filespace_total_bytes", filespace, 8);
    t::Check(fixture.queue->Stats().queued==0, "undrained producer observations");
    std::cout << "filespace_byte_observation=passed exact_uint64=true binary_owner=true\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
