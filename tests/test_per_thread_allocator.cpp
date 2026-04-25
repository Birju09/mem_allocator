#include "per_thread_allocator.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <vector>

using Allocator = pmr_allocator::internal::PerThreadAllocator;

// alignof(std::max_align_t) — the alignment malloc must guarantee
constexpr size_t kAlign = alignof(std::max_align_t);

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------

// The Header struct inside PerThreadAllocator is { size_t sz }.
// It sits immediately before the returned user pointer, so sizeof(Header) ==
// sizeof(size_t).  We read it here by raw pointer arithmetic.
static size_t read_header_bin(void *user_ptr) {
  size_t sz{};
  std::memcpy(&sz,
              reinterpret_cast<const void *>(
                  reinterpret_cast<uintptr_t>(user_ptr) - sizeof(size_t)),
              sizeof(size_t));
  return sz;
}

// Print one allocation's memory layout for debugging
static void print_alloc_layout(const char *label, void *user_ptr,
                               size_t requested, size_t usable) {
  uintptr_t user_addr = reinterpret_cast<uintptr_t>(user_ptr);
  uintptr_t hdr_addr = user_addr - sizeof(size_t);
  size_t bin_size = read_header_bin(user_ptr);

  std::cout << "\n  [" << label << "]"
            << "  requested=" << std::setw(6) << requested << " B"
            << "  bin=" << std::setw(6) << bin_size << " B"
            << "  usable=" << std::setw(6) << usable << " B"
            << "  hdr@0x" << std::hex << hdr_addr << "  usr@0x" << user_addr
            << std::dec
            << "  aligned=" << (user_addr % kAlign == 0 ? "yes" : "NO ");
}

// ---------------------------------------------------------------------------
// Basic allocation / deallocation
// ---------------------------------------------------------------------------

TEST(PerThreadAllocatorTest, AllocateReturnsNonNull) {
  Allocator alloc(1 << 20);
  void *p = alloc.allocate(16, kAlign);
  ASSERT_NE(p, nullptr);
  alloc.deallocate(p, 0);
}

TEST(PerThreadAllocatorTest, AllocatedPointerIsAligned) {
  Allocator alloc(1 << 20);
  void *p = alloc.allocate(16, kAlign);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % kAlign, 0u);
  alloc.deallocate(p, 0);
}

TEST(PerThreadAllocatorTest, AlignmentForVariousSizes) {
  Allocator alloc(1 << 20);
  const size_t sizes[] = {1,  7,  8,   9,   16,  32,   55,
                          63, 64, 100, 256, 500, 1024, 4096};
  for (size_t sz : sizes) {
    void *p = alloc.allocate(sz, kAlign);
    ASSERT_NE(p, nullptr) << "allocation of " << sz << " bytes returned null";
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % kAlign, 0u) << "size=" << sz;
    alloc.deallocate(p, 0);
  }
}

// ---------------------------------------------------------------------------
// allocation_size()
// ---------------------------------------------------------------------------

// allocation_size() returns the usable bytes (bin - header), which is always
// >= the originally requested size.
TEST(PerThreadAllocatorTest, AllocationSizeAtLeastRequested) {
  Allocator alloc(1 << 20);
  const size_t sizes[] = {1, 8, 15, 16, 57, 64, 257, 1024};
  for (size_t req : sizes) {
    void *p = alloc.allocate(req, kAlign);
    EXPECT_GE(alloc.allocation_size(p), req) << "requested=" << req;
    alloc.deallocate(p, 0);
  }
}

// Bin layout: header (8 B) + user data = bin_size.
// allocation_size() == bin_size - sizeof(size_t).
// E.g. request 1 → bin=16 → usable=8;  request 56 → bin=64 → usable=56.
TEST(PerThreadAllocatorTest, AllocationSizeMatchesBin) {
  Allocator alloc(1 << 20);

  // Requests that map to the 64-byte bin
  for (size_t req : {size_t{17}, size_t{32}, size_t{64}}) {
    void *p = alloc.allocate(req, kAlign);
    // bin=64, header=8 → usable=56
    EXPECT_EQ(alloc.allocation_size(p), 64u) << "requested=" << req;
    alloc.deallocate(p, 0);
  }
}

// ---------------------------------------------------------------------------
// Free-list reuse (unsized deallocation, bytes==0 sentinel path)
// ---------------------------------------------------------------------------

TEST(PerThreadAllocatorTest, FreeListReuseAfterUnsizedDealloc) {
  Allocator alloc(1 << 20);
  void *p1 = alloc.allocate(16, kAlign);
  alloc.deallocate(p1, 0); // free with bytes=0 → reads header
  void *p2 = alloc.allocate(16, kAlign);
  EXPECT_EQ(p1, p2) << "second allocation should reuse the free-list slot";
  alloc.deallocate(p2, 0);
}

TEST(PerThreadAllocatorTest, FreeListReuseMultipleRounds) {
  Allocator alloc(1 << 20);
  void *p = alloc.allocate(64, kAlign);
  for (int i = 0; i < 5; ++i) {
    alloc.deallocate(p, 0);
    void *q = alloc.allocate(64, kAlign);
    EXPECT_EQ(p, q) << "round " << i;
    p = q;
  }
  alloc.deallocate(p, 0);
}

// ---------------------------------------------------------------------------
// Sized deallocation path (bytes != 0, used by operator delete(ptr, size))
// NOTE: the current implementation has a bug here — it calls
//       FindSizeBin(bytes) instead of FindSizeBin(bytes + sizeof(Header)),
//       so the pointer ends up in the wrong bin and is NOT reused.
//       This test documents that bug and will FAIL until it is fixed.
// ---------------------------------------------------------------------------

TEST(PerThreadAllocatorTest, FreeListReuseAfterSizedDealloc) {
  // Disabled until the FindSizeBin bug in do_deallocate is fixed.
  Allocator alloc(1 << 20);
  void *p1 = alloc.allocate(8, kAlign);
  alloc.deallocate(p1, 8, kAlign); // sized path
  void *p2 = alloc.allocate(8, kAlign);
  EXPECT_EQ(p1, p2)
      << "sized deallocate should put pointer in bin-16 free list";
  alloc.deallocate(p2, 0);
}

// ---------------------------------------------------------------------------
// Multiple concurrent live allocations
// ---------------------------------------------------------------------------

TEST(PerThreadAllocatorTest, MultipleAllocationsAreDistinct) {
  Allocator alloc(1 << 20);
  constexpr int N = 8;
  void *ptrs[N];
  for (int i = 0; i < N; ++i)
    ptrs[i] = alloc.allocate(32, kAlign);

  for (int i = 0; i < N; ++i)
    for (int j = i + 1; j < N; ++j)
      EXPECT_NE(ptrs[i], ptrs[j]) << "i=" << i << " j=" << j;

  for (int i = 0; i < N; ++i)
    alloc.deallocate(ptrs[i], 0);
}

TEST(PerThreadAllocatorTest, WriteAndReadBackAcrossAllBins) {
  Allocator alloc(1 << 20);
  // One representative size per bin
  const size_t sizes[] = {9, 57, 249, 505, 1017, 2041, 4089, 16377, 65525};
  std::vector<std::pair<void *, size_t>> allocs;

  for (size_t sz : sizes) {
    void *p = alloc.allocate(sz, kAlign);
    std::memset(p, 0xAB, sz);
    allocs.emplace_back(p, sz);
  }

  for (auto [p, sz] : allocs) {
    const unsigned char *bytes = static_cast<unsigned char *>(p);
    for (size_t i = 0; i < sz; ++i)
      EXPECT_EQ(bytes[i], 0xABu) << "corrupted at offset " << i << " sz=" << sz;
    alloc.deallocate(p, 0);
  }
}

// ---------------------------------------------------------------------------
// Fallback path: allocations > 65536 bytes go through mmap directly
// ---------------------------------------------------------------------------

TEST(PerThreadAllocatorTest, LargeAllocationFallbackPath) {
  Allocator alloc(1 << 20);
  const size_t large = 128 * 1024; // 128 KB
  void *p = alloc.allocate(large, kAlign);
  ASSERT_NE(p, nullptr);
  // allocation_size returns the exact requested bytes (no bin rounding)
  EXPECT_EQ(alloc.allocation_size(p), large);
  alloc.deallocate(p, large);
}

TEST(PerThreadAllocatorTest, LargeAllocationIsWritable) {
  Allocator alloc(1 << 20);
  const size_t large = 256 * 1024;
  void *p = alloc.allocate(large, kAlign);
  std::memset(p, 0x55, large);
  const unsigned char *bytes = static_cast<const unsigned char *>(p);
  for (size_t i = 0; i < large; ++i)
    ASSERT_EQ(bytes[i], 0x55u) << "at offset " << i;
  alloc.deallocate(p, large);
}

// ---------------------------------------------------------------------------
// Memory layout dump (runs last — useful for visual debugging)
// ---------------------------------------------------------------------------

TEST(PerThreadAllocatorTest, PrintMemoryLayout) {
  Allocator alloc(1 << 20);

  std::cout << "\n\n=== Memory Layout Dump ===\n";
  std::cout << "  sizeof(size_t) / Header  = " << sizeof(size_t) << " bytes\n";
  std::cout << "  alignof(max_align_t)     = " << kAlign << " bytes\n";
  std::cout << "  Allocator object @ 0x" << std::hex
            << reinterpret_cast<uintptr_t>(&alloc) << std::dec << "\n";

  // One allocation per size bin
  const size_t test_sizes[] = {1,    8,     9,     56,    57,   248,  249,
                               504,  505,   1016,  1017,  2040, 2041, 4088,
                               4089, 16376, 16377, 65524, 65525};
  std::cout << "\n  Bin allocations (header-before layout):";

  std::vector<void *> ptrs;
  for (size_t sz : test_sizes) {
    void *p = alloc.allocate(sz, kAlign);
    ptrs.push_back(p);
    print_alloc_layout("slab", p, sz, alloc.allocation_size(p));
  }

  // Fallback (mmap) path
  std::cout << "\n\n  Fallback (mmap) allocations:";
  const size_t fallback_sizes[] = {65537, 128 * 1024, 1024 * 1024};
  std::vector<void *> fallback_ptrs;
  for (size_t sz : fallback_sizes) {
    void *p = alloc.allocate(sz, kAlign);
    fallback_ptrs.push_back(p);
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    std::cout << "\n  [fallback]  requested=" << std::setw(8) << sz
              << " B  usable=" << std::setw(8) << alloc.allocation_size(p)
              << " B  ptr=0x" << std::hex << addr << std::dec
              << "  aligned=" << (addr % kAlign == 0 ? "yes" : "NO ");
  }

  // Free-list reuse demo
  std::cout << "\n\n  Free-list reuse (allocate → free → reallocate):";
  void *orig = alloc.allocate(64, kAlign);
  uintptr_t orig_addr = reinterpret_cast<uintptr_t>(orig);
  alloc.deallocate(orig, 0);
  void *reused = alloc.allocate(64, kAlign);
  uintptr_t reused_addr = reinterpret_cast<uintptr_t>(reused);
  std::cout << "\n  orig   @ 0x" << std::hex << orig_addr << "\n  reused @ 0x"
            << reused_addr << std::dec
            << "  same=" << (orig_addr == reused_addr ? "yes" : "no") << "\n";
  alloc.deallocate(reused, 0);

  std::cout << "\n=== End Layout Dump ===\n\n";

  // Clean up
  for (auto p : ptrs)
    alloc.deallocate(p, 0);
  for (size_t i = 0; i < fallback_ptrs.size(); ++i)
    alloc.deallocate(fallback_ptrs[i], fallback_sizes[i]);
}
