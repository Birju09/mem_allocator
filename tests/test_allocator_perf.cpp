#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Benchmark infrastructure
// ---------------------------------------------------------------------------

struct BenchResult {
  double duration_us; // microseconds
  long long ops;
  bool exhausted; // true if bad_alloc stopped the run early
};

static double ops_per_sec(const BenchResult &r) {
  return r.ops * 1.0e6 / r.duration_us;
}

// Spin barrier: all N threads block at arrive_and_wait() until all have
// arrived, then they all proceed together.
class SpinBarrier {
  std::atomic<int> count_;

public:
  explicit SpinBarrier(int n) : count_(n) {}

  void arrive_and_wait() {
    count_.fetch_sub(1, std::memory_order_acq_rel);
    while (count_.load(std::memory_order_acquire) > 0)
      std::this_thread::yield();
  }
};

// Single-threaded benchmark: run fn() for up to `iters` iterations.
// Stops early and sets result.exhausted if bad_alloc is thrown.
template <typename Fn> BenchResult run_bench(long long iters, Fn &&fn) {
  bool exhausted = false;
  long long actual = 0;
  auto t0 = std::chrono::steady_clock::now();
  try {
    for (; actual < iters; ++actual)
      fn();
  } catch (const std::bad_alloc &) {
    exhausted = true;
  }
  auto t1 = std::chrono::steady_clock::now();
  double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  return {us, actual, exhausted};
}

// Multi-threaded benchmark: spawn `nthreads` threads, each running fn()
// for up to `iters_per_thread` iterations. All threads start simultaneously.
// If any thread hits bad_alloc, all threads stop at the next iteration
// boundary.
template <typename Fn>
BenchResult run_bench_mt(int nthreads, long long iters_per_thread, Fn &&fn) {
  SpinBarrier barrier(nthreads + 1); // +1 for main thread
  std::atomic<long long> total_ops{0};
  std::atomic<bool> stop{false};

  std::vector<std::thread> threads;
  threads.reserve(nthreads);
  for (int i = 0; i < nthreads; ++i) {
    threads.emplace_back([&]() {
      barrier.arrive_and_wait(); // wait for all threads to be ready
      long long done = 0;
      try {
        for (; done < iters_per_thread && !stop.load(std::memory_order_relaxed);
             ++done)
          fn();
      } catch (const std::bad_alloc &) {
        stop.store(true, std::memory_order_relaxed);
      }
      total_ops.fetch_add(done, std::memory_order_relaxed);
    });
  }

  auto t0 = std::chrono::steady_clock::now();
  barrier.arrive_and_wait(); // release all threads simultaneously
  for (auto &t : threads)
    t.join();
  auto t1 = std::chrono::steady_clock::now();

  double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  return {us, total_ops.load(), stop.load()};
}

static void print_result(const std::string &label, const BenchResult &r) {
  std::cout << "  " << std::left << std::setw(52) << label << std::fixed
            << std::setprecision(1) << std::setw(9) << r.duration_us << " us"
            << "   " << std::setprecision(0) << std::setw(13) << ops_per_sec(r)
            << " ops/s";
  if (r.exhausted)
    std::cout << "  [stopped: allocator capacity exhausted after " << r.ops
              << " ops]";
  std::cout << "\n";
}

static std::string label(const char *tag, int threads, size_t sz) {
  std::string s = tag;
  s += "  thr=" + std::to_string(threads);
  if (sz > 0)
    s += "  size=" + std::to_string(sz) + "B";
  return s;
}

// Returns unique thread counts to benchmark against (deduplicates hw count).
static std::vector<int> thread_counts() {
  int hw = static_cast<int>(std::thread::hardware_concurrency());
  std::vector<int> counts;
  for (int n : {1, 2, 4, 8, hw}) {
    if (n >= 1 && (counts.empty() || counts.back() != n))
      counts.push_back(n);
  }
  return counts;
}

// ---------------------------------------------------------------------------
// 1. Single-thread alloc/free churn — one size at a time
// ---------------------------------------------------------------------------

TEST(AllocPerfTest, SingleThread_AllocFreeChurn) {
  std::cout << "\n=== Single-Thread Alloc/Free Churn ===\n";
  struct Case {
    size_t sz;
    long long iters;
  };
  const Case
      cases[] =
          {
              {8, 5'000'000},   {16, 5'000'000},   {64, 5'000'000},
              {256, 2'000'000}, {1024, 1'000'000}, {4096, 500'000},
              {65536, 50'000}, // hits allocator fallback (mmap) path
              {131072, 20'000},
          };

  for (auto [sz, iters] : cases) {
    auto r = run_bench(iters, [sz]() {
      void *p = malloc(sz);
      // Touch first and last bytes so the compiler cannot elide the alloc.
      static_cast<char *>(p)[0] = 0x42;
      static_cast<char *>(p)[sz - 1] = 0x42;
      free(p);
    });
    print_result(label("alloc/free", 1, sz), r);
  }

  SUCCEED();
}

// ---------------------------------------------------------------------------
// 2. Single-thread batch: alloc N pointers, then free all N
//
// Tests the allocator under live pressure (N concurrent live allocations)
// rather than the reuse path.
// ---------------------------------------------------------------------------

TEST(AllocPerfTest, SingleThread_BatchAllocThenFree) {

  std::cout << "\n=== Single-Thread Batch Alloc Then Free (batch=256) ===\n";

  constexpr int kBatch = 256;
  constexpr int kRounds = 10'000;

  const size_t sizes[] = {16, 64, 256, 1024, 4096};
  std::vector<void *> ptrs(kBatch);

  for (size_t sz : sizes) {
    auto r = run_bench(kRounds, [&]() {
      for (int i = 0; i < kBatch; ++i)
        ptrs[i] = malloc(sz);
      for (int i = 0; i < kBatch; ++i)
        free(ptrs[i]);
    });
    // Each round does kBatch allocs + kBatch frees = 2*kBatch ops.
    BenchResult adj{r.duration_us, r.ops * kBatch * 2, r.exhausted};
    print_result(label("batch alloc+free", 1, sz), adj);
  }

  SUCCEED();
}

// ---------------------------------------------------------------------------
// 3. Single-thread calloc
// ---------------------------------------------------------------------------

TEST(AllocPerfTest, SingleThread_Calloc) {
  std::cout << "\n=== Single-Thread calloc/free ===\n";

  struct Case {
    size_t sz;
    long long iters;
  };
  const Case cases[] = {
      {64, 2'000'000},
      {256, 1'000'000},
      {1024, 500'000},
  };

  for (auto [sz, iters] : cases) {
    auto r = run_bench(iters, [sz]() {
      void *p = calloc(1, sz);
      // Verify zeroing isn't optimised away by the OS.
      static_cast<volatile char *>(p)[sz - 1];
      free(p);
    });
    print_result(label("calloc/free", 1, sz), r);
  }

  SUCCEED();
}

// ---------------------------------------------------------------------------
// 4. Single-thread realloc growth
//
// Simulates a growing buffer (e.g. reading a file into a dynamically
// sized string). Each iteration starts at 8 bytes and doubles to 4096.
// Our custom realloc always allocates new + copies; system realloc may
// extend in-place, so expect the custom allocator to be slower here.
// ---------------------------------------------------------------------------

TEST(AllocPerfTest, SingleThread_ReallocGrowth) {
  std::cout << "\n=== Single-Thread realloc Growth (8 → 4096 bytes) ===\n";

  constexpr long long kIters = 500'000;
  auto r = run_bench(kIters, []() {
    void *p = malloc(8);
    static_cast<char *>(p)[0] = 0x1;
    for (size_t sz = 16; sz <= 4096; sz *= 2) {
      p = realloc(p, sz);
      static_cast<char *>(p)[sz - 1] = 0x1; // touch new tail
    }
    free(p);
  });
  print_result("realloc growth 8→4096  thr=1", r);

  SUCCEED();
}

// ---------------------------------------------------------------------------
// 5. Multi-thread alloc/free scaling — small allocations
//
// Each thread independently allocs and frees its own memory, so there is
// no cross-thread sharing. PerThreadAllocator has zero lock contention;
// system allocators vary.
// ---------------------------------------------------------------------------

TEST(AllocPerfTest, MultiThread_SmallAllocScaling) {
  std::cout << "\n=== Multi-Thread Alloc/Free Scaling (64 bytes) ===\n";

  constexpr size_t kSize = 64;
  constexpr long long kItersPerThread = 2'000'000;

  for (int nthreads : thread_counts()) {
    auto r = run_bench_mt(nthreads, kItersPerThread, []() {
      void *p = malloc(kSize);
      static_cast<char *>(p)[0] = 0x42;
      static_cast<char *>(p)[63] = 0x42;
      free(p);
    });
    print_result(label("alloc/free", 1, kSize), r);
  }

  SUCCEED();
}

TEST(AllocPerfTest, MultiThread_MediumAllocScaling) {
  std::cout << "\n=== Multi-Thread Alloc/Free Scaling (1024 bytes) ===\n";

  constexpr size_t kSize = 1024;
  constexpr long long kItersPerThread = 1'000'000;

  for (int nthreads : thread_counts()) {
    auto r = run_bench_mt(nthreads, kItersPerThread, []() {
      void *p = malloc(kSize);
      static_cast<char *>(p)[0] = 0x42;
      static_cast<char *>(p)[1023] = 0x42;
      free(p);
    });
    print_result(label("alloc/free", nthreads, kSize), r);
  }

  SUCCEED();
}

// ---------------------------------------------------------------------------
// 6. Multi-thread mixed sizes
//
// Each thread cycles through a fixed set of sizes that spans several bins.
// Thread-local cycling means no inter-thread coordination for size selection.
// ---------------------------------------------------------------------------

TEST(AllocPerfTest, MultiThread_MixedSizes) {
  std::cout << "\n=== Multi-Thread Mixed Sizes ===\n";

  constexpr long long kItersPerThread = 1'000'000;

  static constexpr size_t kSizes[] = {8, 16, 64, 128, 256, 512, 1024, 2048};
  constexpr int kNumSizes =
      static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0]));

  for (int nthreads : thread_counts()) {
    auto r = run_bench_mt(nthreads, kItersPerThread, []() {
      // Thread-local counter: no atomic, no contention.
      thread_local long long idx = 0;
      size_t sz = kSizes[idx++ % kNumSizes];
      void *p = malloc(sz);
      static_cast<char *>(p)[0] = 0x42;
      free(p);
    });
    print_result(label("mixed sizes", nthreads, 0), r);
  }

  SUCCEED();
}

// ---------------------------------------------------------------------------
// 7. Multi-thread batch alloc/free
//
// Each thread holds a batch of live allocations before freeing them.
// This stresses the allocator's ability to service concurrent live sets
// without running out of address space or hitting lock contention.
// ---------------------------------------------------------------------------

TEST(AllocPerfTest, MultiThread_BatchAllocFree) {
  std::cout
      << "\n=== Multi-Thread Batch Alloc Then Free (size=64, batch=128) ===\n";

  constexpr size_t kSize = 64;
  constexpr int kBatch = 128;
  constexpr long long kRoundsPerThread = 5'000;

  for (int nthreads : thread_counts()) {
    auto r = run_bench_mt(nthreads, kRoundsPerThread, []() {
      void *ptrs[kBatch];
      for (int i = 0; i < kBatch; ++i) {
        ptrs[i] = malloc(kSize);
        static_cast<char *>(ptrs[i])[0] = 0x42;
      }
      for (int i = 0; i < kBatch; ++i)
        free(ptrs[i]);
    });
    // Each round = kBatch allocs + kBatch frees = 2*kBatch ops.
    BenchResult adj{r.duration_us, r.ops * kBatch * 2, r.exhausted};
    print_result(label("batch alloc+free", nthreads, kSize), adj);
  }

  SUCCEED();
}

// ---------------------------------------------------------------------------
// 8. Multi-thread realloc growth
// ---------------------------------------------------------------------------

TEST(AllocPerfTest, MultiThread_ReallocGrowth) {
  std::cout << "\n=== Multi-Thread realloc Growth (8 → 1024 bytes) ===\n";

  constexpr long long kItersPerThread = 200'000;

  for (int nthreads : thread_counts()) {
    auto r = run_bench_mt(nthreads, kItersPerThread, []() {
      void *p = malloc(8);
      static_cast<char *>(p)[0] = 0x1;
      for (size_t sz = 16; sz <= 1024; sz *= 2) {
        p = realloc(p, sz);
        static_cast<char *>(p)[sz - 1] = 0x1;
      }
      free(p);
    });
    print_result(label("realloc growth 8→1024", nthreads, 0), r);
  }

  SUCCEED();
}
