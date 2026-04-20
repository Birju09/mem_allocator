#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory_resource>
#include <unordered_map>

namespace pmr_allocator::internal {

class PerThreadAllocator : public std::pmr::memory_resource {
public:
  PerThreadAllocator(const std::size_t max_initial_size);
  ~PerThreadAllocator();

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

  struct Header {
    size_t sz;
  };

  struct SizeBinEntry {
    void  *free_list_head{nullptr};
    size_t num_allocated{};
    size_t num_deallocated{};
  };

  static constexpr size_t kMaxFreeListSize = 0x100000;
  std::array<char, kMaxFreeListSize> free_list_buf_{};
  std::pmr::monotonic_buffer_resource free_list_mbr_{
      free_list_buf_.data(), free_list_buf_.size(),
      std::pmr::null_memory_resource()};

  // For book keeping
  std::pmr::unordered_map<size_t, SizeBinEntry> size_slabs_{&free_list_mbr_};

  // Fallback allocations (mmap'd blocks > largest slab bin)
  std::pmr::unordered_map<void *, size_t> fallback_allocations_{
      &free_list_mbr_};
};

} // namespace pmr_allocator::internal
