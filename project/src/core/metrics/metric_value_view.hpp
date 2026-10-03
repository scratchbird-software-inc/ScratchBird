// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"
#include <span>
#include <iterator>

namespace scratchbird::core::metrics {
// Text borrows bytes; all other alternatives remain fixed native values.
// native is never an owning text value when returned by a borrowed decoder.
struct MetricScalarView {
  MetricScalar native;
  std::optional<std::string_view> text;
};
struct MetricValueViewResult;
namespace detail {
template<class Definition> MetricValueViewResult DecodeValueView(
    const Definition&, std::span<const platform::byte>) noexcept;
}
class MetricHistogramView {
 public:
  MetricHistogramView() = default;
  std::size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  MetricScalar bound(std::size_t index) const;
  u64 bucket(std::size_t index) const;
 private:
  template<class D> friend MetricValueViewResult detail::DecodeValueView(
      const D&, std::span<const platform::byte>) noexcept;
  MetricHistogramView(std::span<const platform::byte> bytes, MetricScalarType type,
      std::size_t count) : bytes_(bytes), type_(type), count_(count) {}
  std::span<const platform::byte> bytes_;
  MetricScalarType type_ = MetricScalarType::invalid;
  std::size_t count_ = 0;
};
struct MetricValueLabelView {
  std::string_view key;
  std::variant<std::string_view, MetricUuid> value;
};
class MetricValueLabelsView {
 public:
  class Iterator {
   public:
    using value_type = MetricValueLabelView;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;
    Iterator() = default;
    value_type operator*() const;
    Iterator& operator++();
    Iterator operator++(int) { auto old=*this; ++*this; return old; }
    bool operator==(const Iterator&) const = default;
   private:
    friend class MetricValueLabelsView;
    explicit Iterator(const platform::byte* at) : at_(at) {}
    const platform::byte* at_ = nullptr;
  };
  MetricValueLabelsView() = default;
  std::size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  Iterator begin() const { return Iterator(bytes_.data()); }
  Iterator end() const { return Iterator(bytes_.empty()?bytes_.data():bytes_.data()+bytes_.size()); }
 private:
  template<class D> friend MetricValueViewResult detail::DecodeValueView(
      const D&, std::span<const platform::byte>) noexcept;
  MetricValueLabelsView(std::span<const platform::byte> bytes, std::size_t count)
      : bytes_(bytes), count_(count) {}
  std::span<const platform::byte> bytes_;
  std::size_t count_ = 0;
};
struct MetricValueView {
  // family borrows the supplied descriptor; all other variable bytes borrow
  // the SBMV input. Both owners and their grants must outlive every access.
  std::string_view family;
  MetricValueLabelsView labels;
  MetricType type = MetricType::counter;
  MetricScalarView value;
  u64 count = 0;
  MetricScalarView sum;
  MetricHistogramView histogram;
  bool buckets_cumulative = true;
  bool arithmetic_inexact = false;
  std::string_view state_text;
};
}  // namespace scratchbird::core::metrics
