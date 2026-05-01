#include "per_thread_allocator.h"
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <new>
#include <pthread.h>

// Real malloc/free obtained via dlsym (to avoid infinite recursion)
// These are global (not thread_local) since they're the same function pointers
// for all threads
typedef void *(*malloc_t)(size_t);
typedef void (*free_t)(void *);
static malloc_t real_malloc = nullptr;
static free_t real_free = nullptr;

namespace {

using PerThreadAllocator = pmr_allocator::internal::PerThreadAllocator;

struct AllocatorDestructor {
  void operator()(PerThreadAllocator *allocator) {
    if (!allocator) {
      return;
    }
    if (real_free == nullptr) {
      real_free = (free_t)dlsym(RTLD_NEXT, "free");
    }
    real_free(allocator);
  }
};

static thread_local std::unique_ptr<PerThreadAllocator, AllocatorDestructor>
    allocator;

PerThreadAllocator &get_allocator() {
  if (!allocator) {
    if (real_malloc == nullptr) {
      real_malloc = (malloc_t)dlsym(RTLD_NEXT, "malloc");
    }
    void *ptr = real_malloc(sizeof(PerThreadAllocator));
    if (!ptr) {
      throw std::bad_alloc{};
    }
    allocator.reset(new (ptr) PerThreadAllocator(0x40000000));
  }
  return *allocator;
}

constexpr size_t kNaturalAlignment{alignof(std::max_align_t)};

} // namespace

// ---------------------------------------------------------------------------
// malloc / free family
// ---------------------------------------------------------------------------

extern "C" void *malloc(size_t size) {
  if (size == 0) {
    return nullptr;
  }

  return get_allocator().allocate(size, kNaturalAlignment);
}

extern "C" void free(void *ptr) {
  if (!ptr) {
    return;
  }
  get_allocator().deallocate(ptr, 0);
}

extern "C" void *calloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0) {
    return nullptr;
  }
  // Overflow guard
  if (size > SIZE_MAX / nmemb) {
    return nullptr;
  }
  const size_t total = nmemb * size;
  void *p = get_allocator().allocate(total, kNaturalAlignment);
  std::memset(p, 0, total);
  return p;
}

extern "C" void *realloc(void *ptr, size_t size) {
  if (!ptr) {
    return malloc(size);
  }
  if (size == 0) {
    free(ptr);
    return nullptr;
  }

  const size_t old_size = get_allocator().allocation_size(ptr);
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
  return get_allocator().allocate(size, kNaturalAlignment);
}

void *operator new[](std::size_t size) {
  return get_allocator().allocate(size, kNaturalAlignment);
}

void operator delete(void *ptr) noexcept {
  if (!ptr) {
    return;
  }
  // auto *alloc = static_cast<pmr_allocator::internal::PerThreadAllocator *>(
  //     pthread_getspecific(allocator_key));
  // if (alloc)
  get_allocator().deallocate(ptr, 0);
}

void operator delete[](void *ptr) noexcept {
  if (!ptr) {
    return;
  }
  get_allocator().deallocate(ptr, 0);
}

void operator delete(void *ptr, std::size_t bytes) noexcept {
  if (!ptr) {
    return;
  }
  get_allocator().deallocate(ptr, bytes);
}

void operator delete[](void *ptr, std::size_t bytes) noexcept {
  if (!ptr) {
    return;
  }
  get_allocator().deallocate(ptr, bytes);
}
