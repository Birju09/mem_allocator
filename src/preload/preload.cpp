#include "per_thread_allocator.h"
#include <climits>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <new>
#include <pthread.h>
#include <stdexcept>

// Real malloc/free obtained via dlsym (to avoid infinite recursion)
typedef void *(*malloc_t)(size_t);
typedef void (*free_t)(void *);
static malloc_t real_malloc = nullptr;
static free_t real_free = nullptr;

// Destructor called when thread exits
void allocator_destructor(void *arg) {
  // Cast and explicitly call destructor to clean up mmap'd buffer
  pmr_allocator::internal::PerThreadAllocator *alloc =
      static_cast<pmr_allocator::internal::PerThreadAllocator *>(arg);
  alloc->~PerThreadAllocator();

  // Free the memory using real free
  if (real_free == nullptr) {
    real_free = (free_t)dlsym(RTLD_NEXT, "free");
  }
  real_free(alloc);
}

namespace {

// pthread_key_t for thread-local allocator storage
static pthread_key_t allocator_key;
static pthread_once_t allocator_key_once = PTHREAD_ONCE_INIT;

// Called once per process to create the pthread key
void create_allocator_key() {
  pthread_key_create(&allocator_key, allocator_destructor);
}

// Get or create the allocator for current thread
pmr_allocator::internal::PerThreadAllocator &get_allocator() {
  // Ensure the key is created (thread-safe via pthread_once)
  pthread_once(&allocator_key_once, create_allocator_key);

  // Get the allocator for this thread
  auto *alloc = static_cast<pmr_allocator::internal::PerThreadAllocator *>(
      pthread_getspecific(allocator_key));

  if (!alloc) {
    // First time this thread has called get_allocator.
    // Use the real malloc to allocate the allocator object itself,
    // avoiding infinite recursion from our hooked malloc.
    if (real_malloc == nullptr) {
      real_malloc = (malloc_t)dlsym(RTLD_NEXT, "malloc");
    }
    void *ptr = real_malloc(sizeof(pmr_allocator::internal::PerThreadAllocator));
    if (!ptr) {
      throw std::bad_alloc{};
    }
    alloc = new (ptr) pmr_allocator::internal::PerThreadAllocator(0x10000000);
    pthread_setspecific(allocator_key, alloc);
  }

  return *alloc;
}

constexpr size_t kNaturalAlignment{alignof(std::max_align_t)};

} // namespace


// ---------------------------------------------------------------------------
// malloc / free family
// ---------------------------------------------------------------------------

extern "C" void *malloc(size_t size) {
  if (size == 0)
    return nullptr;
  return get_allocator().allocate(size, kNaturalAlignment);
}

extern "C" void free(void *ptr) {
  if (!ptr)
    return;
  get_allocator().deallocate(ptr, 0);
}

extern "C" void *calloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0)
    return nullptr;
  // Overflow guard
  if (size > SIZE_MAX / nmemb)
    return nullptr;
  const size_t total = nmemb * size;
  void *p = get_allocator().allocate(total, kNaturalAlignment);
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
  if (!ptr)
    return;
  auto *alloc = static_cast<pmr_allocator::internal::PerThreadAllocator *>(
      pthread_getspecific(allocator_key));
  if (alloc)
    alloc->deallocate(ptr, 0);
}

void operator delete[](void *ptr) noexcept {
  if (!ptr)
    return;
  auto *alloc = static_cast<pmr_allocator::internal::PerThreadAllocator *>(
      pthread_getspecific(allocator_key));
  if (alloc)
    alloc->deallocate(ptr, 0);
}

void operator delete(void *ptr, std::size_t bytes) noexcept {
  if (!ptr)
    return;
  auto *alloc = static_cast<pmr_allocator::internal::PerThreadAllocator *>(
      pthread_getspecific(allocator_key));
  if (alloc)
    alloc->deallocate(ptr, bytes);
}

void operator delete[](void *ptr, std::size_t bytes) noexcept {
  if (!ptr)
    return;
  auto *alloc = static_cast<pmr_allocator::internal::PerThreadAllocator *>(
      pthread_getspecific(allocator_key));
  if (alloc)
    alloc->deallocate(ptr, bytes);
}
