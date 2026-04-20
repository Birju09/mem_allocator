#include "per_thread_allocator.h"
#include <cstring>

namespace {

thread_local pmr_allocator::internal::PerThreadAllocator
    allocator(0x10000000);

constexpr size_t kNaturalAlignment{alignof(std::max_align_t)};

} // namespace

// ---------------------------------------------------------------------------
// malloc / free family
// ---------------------------------------------------------------------------

extern "C" void *malloc(size_t size) {
  if (size == 0)
    return nullptr;
  return allocator.allocate(size, kNaturalAlignment);
}

extern "C" void free(void *ptr) {
  if (!ptr)
    return;
  allocator.deallocate(ptr, 0);
}

extern "C" void *calloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0)
    return nullptr;
  // Overflow guard
  if (size > SIZE_MAX / nmemb)
    return nullptr;
  const size_t total = nmemb * size;
  void *p = allocator.allocate(total, kNaturalAlignment);
  // mmap gives zeroed pages for fresh allocations, but recycled free-list
  // slots may contain stale data, so always zero.
  std::memset(p, 0, total);
  return p;
}

extern "C" void *realloc(void *ptr, size_t size) {
  if (!ptr)
    return malloc(size);
  if (size == 0) {
    free(ptr);
    return nullptr;
  }

  const size_t old_size = allocator.allocation_size(ptr);
  void *new_ptr = malloc(size);
  if (!new_ptr)
    return nullptr;

  std::memcpy(new_ptr, ptr, old_size < size ? old_size : size);
  free(ptr);
  return new_ptr;
}

// ---------------------------------------------------------------------------
// operator new / delete family
// ---------------------------------------------------------------------------

void *operator new(std::size_t size) {
  return allocator.allocate(size, kNaturalAlignment);
}

void *operator new[](std::size_t size) {
  return allocator.allocate(size, kNaturalAlignment);
}

void operator delete(void *ptr) noexcept {
  if (!ptr)
    return;
  allocator.deallocate(ptr, 0);
}

void operator delete[](void *ptr) noexcept {
  if (!ptr)
    return;
  allocator.deallocate(ptr, 0);
}

void operator delete(void *ptr, std::size_t bytes) noexcept {
  if (!ptr)
    return;
  allocator.deallocate(ptr, bytes);
}

void operator delete[](void *ptr, std::size_t bytes) noexcept {
  if (!ptr)
    return;
  allocator.deallocate(ptr, bytes);
}
