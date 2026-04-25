#include "per_thread_allocator.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

namespace pmr_allocator::internal {

namespace {

constexpr std::array<size_t, 9> kSizeBins{{
    16,
    64,
    256,
    512,
    1024,
    2048,
    4096,
    16384,
    65536,
}};

// Returns the index into kSizeBins for a given size.
// If bytes exceeds the largest bin, returns kSizeBins.size().
size_t FindSizeBinIndex(size_t bytes) {
  for (size_t i = 0; i < kSizeBins.size(); ++i) {
    if (bytes <= kSizeBins[i])
      return i;
  }
  return kSizeBins.size();
}

} // namespace

PerThreadAllocator::PerThreadAllocator(const size_t max_initial_size)
    : buffer_capactiy_(max_initial_size) {

  buffer_ = mmap(nullptr, buffer_capactiy_, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

  if (buffer_ == MAP_FAILED) {
    throw std::bad_alloc{};
  }
}

PerThreadAllocator::~PerThreadAllocator() noexcept {
  // munmap first, before any other operations that might call into the runtime.
  if (buffer_ != nullptr) {
    munmap(buffer_, buffer_capactiy_);
    buffer_ = nullptr;
  }

  // Only write to stderr if we have outstanding fallback allocations.
  if (fallback_count_ > 0) {
    constexpr std::string_view msg{"Memory leak detected"};
    std::ignore = write(STDERR_FILENO, msg.data(), msg.size());
  }
}

size_t PerThreadAllocator::allocation_size(void *p) const {
  // Check fallback entries first.
  for (const auto &entry : fallback_entries_) {
    if (entry.used && entry.ptr == p)
      return entry.size;
  }
  Header h;
  std::memcpy(&h,
              reinterpret_cast<const void *>(reinterpret_cast<uintptr_t>(p) -
                                             sizeof(Header)),
              sizeof(Header));
  return h.sz - sizeof(Header);
}

void *PerThreadAllocator::do_allocate(size_t bytes, size_t alignment) {
  // Sizes above the largest slab bin go directly through mmap.
  if (bytes > kSizeBins.back()) {
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
      throw std::bad_alloc{};
    }
    // Find a free slot in the fallback table.
    bool found = false;
    for (auto &entry : fallback_entries_) {
      if (!entry.used) {
        entry.ptr = p;
        entry.size = bytes;
        entry.used = true;
        found = true;
        break;
      }
    }
    if (!found) {
      munmap(p, bytes);
      throw std::bad_alloc{};
    }
    ++fallback_count_;
    return p;
  }

  void *p = nullptr;
  auto binIdx = FindSizeBinIndex(bytes + sizeof(Header));
  auto szBin = kSizeBins[binIdx];
  auto &entry = size_slabs_[binIdx];
  void *&head = entry.free_list_head;

  if (head != nullptr) {
    // Sanity check: head should point within our buffer. If corrupted, discard
    // the free list and allocate fresh memory instead.
    uintptr_t head_addr = reinterpret_cast<uintptr_t>(head);
    uintptr_t buf_start = reinterpret_cast<uintptr_t>(buffer_);
    uintptr_t buf_end = buf_start + buffer_capactiy_;
    if (head_addr >= buf_start && head_addr < buf_end) {
      p = head;
      void *next;
      std::memcpy(&next, p, sizeof(void *));
      head = next;
    }
  }

  // Carve a new slot out of the main buffer if the free list was empty.
  if (!p && (buffer_capactiy_ > buffer_occupied_)) {
    uintptr_t next_addr =
        reinterpret_cast<uintptr_t>(buffer_) + buffer_occupied_;

    auto user_addr = next_addr + sizeof(Header);
    auto mod = user_addr % alignment;
    if (mod != 0) {
      user_addr += (alignment - mod);
      next_addr = user_addr - sizeof(Header);
    }
    if ((next_addr + szBin) <=
        reinterpret_cast<uintptr_t>(buffer_) + buffer_capactiy_) {
      p = reinterpret_cast<void *>(next_addr + sizeof(Header));
      buffer_occupied_ += szBin + (mod != 0 ? (alignment - mod) : 0);
    }
  }

  if (!p) {
    throw std::bad_alloc{};
  }

  Header h{szBin};
  auto header_location =
      reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(p) - sizeof(Header));
  std::memcpy(header_location, &h, sizeof(Header));

  ++entry.num_allocated;

  return p;
}

void PerThreadAllocator::do_deallocate(void *p, size_t bytes,
                                       size_t alignment) {
  static_cast<void>(alignment);

  // Check fallback table.
  for (auto &entry : fallback_entries_) {
    if (entry.used && entry.ptr == p) {
      if ((bytes > 0) && bytes != entry.size) {
        constexpr std::string_view msg{
            "Size being deallocated different than what was allocated"};
        std::ignore = write(STDERR_FILENO, msg.data(), msg.size());
      }
      munmap(p, entry.size);
      entry.used = false;
      entry.ptr = nullptr;
      entry.size = 0;
      --fallback_count_;
      return;
    }
  }

  if (bytes == 0) {
    auto header_location = reinterpret_cast<void *>(
        reinterpret_cast<uintptr_t>(p) - sizeof(Header));
    Header h;
    std::memcpy(&h, header_location, sizeof(Header));
    bytes = h.sz;
  } else {
    bytes += sizeof(Header);
  }

  auto binIdx = FindSizeBinIndex(bytes);
  auto &bin = size_slabs_[binIdx];

  if (bin.num_allocated == bin.num_deallocated) {
    return;
  }

  void *&head = bin.free_list_head;
  std::memcpy(p, &head, sizeof(void *));
  head = p;

  ++bin.num_deallocated;
}

} // namespace pmr_allocator::internal
