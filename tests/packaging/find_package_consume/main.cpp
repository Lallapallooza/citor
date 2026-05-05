#include <citor/cpos/parallel_reduce.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <cstdlib>

int main() {
  citor::ThreadPool pool(4);
  const long sum = pool.parallelReduce<citor::FixedBlockReduceHints>(
      0, 1000, 0L,
      [](std::size_t lo, std::size_t hi) {
        long s = 0;
        for (std::size_t i = lo; i < hi; ++i) {
          s += static_cast<long>(i);
        }
        return s;
      },
      [](long a, long b) { return a + b; });
  return sum == 499500L ? EXIT_SUCCESS : EXIT_FAILURE;
}
