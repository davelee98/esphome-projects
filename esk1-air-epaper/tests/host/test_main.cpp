#include "test_util.h"

int main(int argc, char **argv) {
  if (argc > 1)
    host_test::output_dir() = argv[1];
  const char *only = argc > 2 ? argv[2] : nullptr;
  int ran = 0;
  for (auto &t : host_test::registry()) {
    if (only != nullptr && std::strstr(t.name, only) == nullptr)
      continue;
    host_test::current() = t.name;
    int before = host_test::failures();
    t.fn();
    std::printf("%s %s\n", host_test::failures() == before ? "ok  " : "FAIL", t.name);
    ran++;
  }
  std::printf("\n%d tests, %d checks, %d failures\n", ran, host_test::checks(), host_test::failures());
  return host_test::failures() == 0 ? 0 : 1;
}
