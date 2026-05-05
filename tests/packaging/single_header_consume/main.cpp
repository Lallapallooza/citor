#include "citor.hpp"

#include <cstddef>
#include <cstdlib>
#include <vector>

int main() {
  citor::ThreadPool pool(4);
  std::vector<int> v(1024, 1);
  pool.parallelFor<citor::HintsDefaults>(
      0, v.size(), [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          v[i] *= 2;
        }
      });
  long sum = 0;
  for (int x : v) {
    sum += x;
  }
  return sum == 2048 ? EXIT_SUCCESS : EXIT_FAILURE;
}
