// Smoke-test the amalgamated single header. Same body as the
// find_package/Conan consumer; the only thing exercised here is the
// `single_include/citor.hpp` drop-in path with no build system.

#include "citor.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

int main() {
  citor::ThreadPool pool(/*participants=*/4);

  std::atomic<std::uint64_t> seen{0};
  pool.parallelFor<citor::HintsDefaults>(
      std::size_t{0}, std::size_t{1024}, [&](std::size_t lo, std::size_t hi) {
        seen.fetch_add(hi - lo, std::memory_order_relaxed);
      });
  if (seen.load() != 1024U) {
    std::cerr << "parallelFor mismatch: seen=" << seen.load() << "\n";
    return EXIT_FAILURE;
  }

  std::vector<std::int64_t> in(64, 1);
  std::vector<std::int64_t> out(64, 0);
  const std::int64_t total = pool.inclusiveScan<citor::HintsDefaults>(
      std::span<const std::int64_t>(in), std::span<std::int64_t>(out),
      std::int64_t{0}, [](std::int64_t a, std::int64_t b) { return a + b; });
  if (total != 64 || out.back() != 64) {
    std::cerr << "inclusiveScan mismatch: total=" << total
              << " back=" << out.back() << "\n";
    return EXIT_FAILURE;
  }

  std::cout << "citor single-header smoke: OK\n";
  return EXIT_SUCCESS;
}
