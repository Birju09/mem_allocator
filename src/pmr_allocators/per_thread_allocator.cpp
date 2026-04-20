#include "per_thread_allocator.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <new>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

namespace pmr_allocator::internal {

namespace {

constexpr std::array<size_t, 10> kSizeBins{{
    8,
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
  // Find bin size
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

  // Use mmap to get memory from the OS
  buffer_ = mmap(nullptr, buffer_capactiy_, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

  if (buffer_ == MAP_FAILED) {
    throw std::bad_alloc{};
  }

  for (const auto &s : kSizeBins) {
    size_slabs_.insert({s, SizeBinEntry(&free_list_mbr_)});
  }
}

PerThreadAllocator::~PerThreadAllocator() {
  munmap(buffer_, buffer_capactiy_);
  if (!fallback_allocations_.empty()) {
    // memory leak detected
    constexpr std::string_view msg{"Memory leak detected"};

    // Can't do much here, ignore return value
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
  // h.sz is the full bin size; subtract the embedded header to get user space
  return h.sz - sizeof(Header);
}

void *PerThreadAllocator::do_allocate(size_t bytes, size_t alignment) {
  // Use mmap for sizes above 64 Kb
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

  auto &free_list = size_slabs_.at(szBin).free_list;

  // Find the first free entry
  if (!free_list.empty()) {
    p = free_list.begin()->addr;
    free_list.pop_front();
  }

  // Find from buffer
  if (!p && (buffer_capactiy_ > buffer_occupied_)) {
    // Find next alignment
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

  ++size_slabs_.at(szBin).num_allocated;

  return p;
}

void PerThreadAllocator::do_deallocate(void *p, size_t bytes,
                                       size_t alignment) {
  static_cast<void>(alignment);
  if (fallback_allocations_.contains(p)) {
    if ((bytes > 0) && bytes != fallback_allocations_.at(p)) {
      // Placeholder print, add more logic later
      constexpr std::string_view msg{
          "Size being deallocated different than what was allocated"};
      // Can't do much here, ignore return value
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
    // Can't do much here, ignore return value
    std::ignore = write(STDERR_FILENO, msg.data(), msg.size());
    return;
  }

  auto &free_list = bin.free_list;
  free_list.push_back(FreeListEntry{p});

  ++bin.num_deallocated;
}

} // namespace pmr_allocator::internal