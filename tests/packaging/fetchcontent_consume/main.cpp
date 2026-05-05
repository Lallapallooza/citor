#include <citor/cpos/fork_join.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <atomic>
#include <cstdlib>

int main() {
  citor::ThreadPool pool(4);
  std::atomic<int> ran{0};
  pool.forkJoin<citor::HintsDefaults>(
      citor::CancellationToken{},
      [&] { ran.fetch_add(1, std::memory_order_relaxed); },
      [&] { ran.fetch_add(1, std::memory_order_relaxed); },
      [&] { ran.fetch_add(1, std::memory_order_relaxed); },
      [&] { ran.fetch_add(1, std::memory_order_relaxed); });
  return ran.load() == 4 ? EXIT_SUCCESS : EXIT_FAILURE;
}
