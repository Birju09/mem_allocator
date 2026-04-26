#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory_resource>

namespace pmr_allocator::internal {

class PerThreadAllocator : public std::pmr::memory_resource {
public:
  PerThreadAllocator(const std::size_t max_initial_size);
  ~PerThreadAllocator() noexcept override;

  // Returns the usable size of an allocation (needed for realloc).
  size_t allocation_size(void *p) const;

protected:
  void *do_allocate(size_t bytes, size_t align) override;
  void do_deallocate(void *p, size_t bytes, size_t align) override;
  bool do_is_equal(const std::pmr::memory_resource &o) const noexcept override {
    return this == &o;
  }

private:
  void *buffer_{nullptr};
  size_t buffer_capactiy_{};
  size_t buffer_occupied_{};
  uintptr_t buf_end_;

  struct Header {
    size_t sz;
  };

  struct SizeBinEntry {
    void *free_list_head{nullptr};
    size_t num_allocated{};
    size_t num_deallocated{};
  };

  static constexpr size_t kNumSizeBins = 14;
  std::array<SizeBinEntry, kNumSizeBins> size_slabs_{};

  // Fallback allocations (mmap'd blocks > largest slab bin).
  // Fixed-size table to avoid any heap/PMR allocation.
  struct FallbackEntry {
    void *ptr{nullptr};
    size_t size{0};
    bool used{false};
  };
  static constexpr size_t kMaxFallbackEntries = 256;
  std::array<FallbackEntry, kMaxFallbackEntries> fallback_entries_{};
  size_t fallback_count_{0};
};

} // namespace pmr_allocator::internal
