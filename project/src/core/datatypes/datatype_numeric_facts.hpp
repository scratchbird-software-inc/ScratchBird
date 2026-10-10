// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace scratchbird::core::datatypes {

// Operation facts are distinct from diagnostics and from the numeric value.
// Nontrapping inexact/underflow results remain successful without losing facts.
struct DatatypeNumericFacts {
  bool inexact = false;
  bool underflow = false;
  bool overflow = false;
  bool invalid = false;
  bool divide_by_zero = false;
  bool subnormal = false;
  bool unordered = false;
};

}  // namespace scratchbird::core::datatypes
