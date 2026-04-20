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

size_t FindSizeBin(size_t bytes) {
  size_t szBin = kSizeBins.front();
  for (const auto &s : kSizeBins) {
    if (bytes <= s) {
      szBin = s;
      break;
    }
  }
  return szBin;
}

} // namespace

PerThreadAllocator::PerThreadAllocator(const size_t max_initial_size)
    : buffer_capactiy_(max_initial_size) {

  buffer_ = mmap(nullptr, buffer_capactiy_, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

  if (buffer_ == MAP_FAILED) {
    throw std::bad_alloc{};
  }

  for (const auto &s : kSizeBins) {
    size_slabs_.insert({s, SizeBinEntry{}});
  }
}

PerThreadAllocator::~PerThreadAllocator() {
  munmap(buffer_, buffer_capactiy_);
  if (!fallback_allocations_.empty()) {
    constexpr std::string_view msg{"Memory leak detected"};
    std::ignore = write(STDERR_FILENO, msg.data(), msg.size());
  }
}

size_t PerThreadAllocator::allocation_size(void *p) const {
  if (fallback_allocations_.contains(p)) {
    return fallback_allocations_.at(p);
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
    fallback_allocations_.insert({p, bytes});
    return p;
  }

  void *p = nullptr;
  auto szBin = FindSizeBin(bytes + sizeof(Header));
  auto &entry = size_slabs_.at(szBin);
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

  if (fallback_allocations_.contains(p)) {
    if ((bytes > 0) && bytes != fallback_allocations_.at(p)) {
      constexpr std::string_view msg{
          "Size being deallocated different than what was allocated"};
      std::ignore = write(STDERR_FILENO, msg.data(), msg.size());
    }
    munmap(p, fallback_allocations_.at(p));
    fallback_allocations_.erase(p);
    return;
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

  auto szBin = FindSizeBin(bytes);
  auto &bin = size_slabs_.at(szBin);

  if (bin.num_allocated == bin.num_deallocated) {
    constexpr std::string_view msg{
        "Something went wrong, memory being freed again?"};
    std::ignore = write(STDERR_FILENO, msg.data(), msg.size());
    return;
  }

  void *&head = bin.free_list_head;
  std::memcpy(p, &head, sizeof(void *));
  head = p;

  ++bin.num_deallocated;
}

} // namespace pmr_allocator::internal
