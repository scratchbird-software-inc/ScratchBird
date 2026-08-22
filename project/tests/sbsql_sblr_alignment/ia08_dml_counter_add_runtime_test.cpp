#include "engine/sblr/sblr_dml_counter_add_runtime.hpp"
#include <cassert>

int main() {
  using namespace scratchbird::engine::sblr;
  SblrDmlCounterAddRequestV1 q; q.receipt[0] = 1; q.occurrence = 1; q.counter_occurrence = 2;
  auto qw = EncodeSblrDmlCounterAddRequestV1(q); SblrDmlCounterAddRequestV1 q2;
  assert(DecodeSblrDmlCounterAddRequestV1(qw.data(), qw.size(), &q2, nullptr)); qw[16] ^= 1;
  assert(!DecodeSblrDmlCounterAddRequestV1(qw.data(), qw.size(), &q2, nullptr));
  SblrDmlCounterAddDescriptorV1 d; d.evidence[0] = 2; d.availability = 1;
  auto dw = EncodeSblrDmlCounterAddDescriptorV1(d, true); SblrDmlCounterAddDescriptorV1 d2;
  assert(DecodeSblrDmlCounterAddDescriptorV1(dw.data(), dw.size(), &d2, nullptr, true)); dw[416] ^= 1;
  assert(!DecodeSblrDmlCounterAddDescriptorV1(dw.data(), dw.size(), &d2, nullptr, true));
  SblrDmlCounterAddResultV1 r; r.evidence[0] = 3; r.availability = 1; r.publication_barrier[0] = 4;
  auto rw = EncodeSblrDmlCounterAddResultV1(r); SblrDmlCounterAddResultV1 r2;
  assert(DecodeSblrDmlCounterAddResultV1(rw.data(), rw.size(), &r2, nullptr)); rw[256] ^= 1;
  assert(!DecodeSblrDmlCounterAddResultV1(rw.data(), rw.size(), &r2, nullptr));
  return 0;
}
