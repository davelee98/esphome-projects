#pragma once

// A minimal test harness: TEST(name) registers a test, CHECK* record
// failures without stopping the run. main() is in test_main.cpp.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace host_test {

struct TestCase {
  const char *name;
  std::function<void()> fn;
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int &failures() {
  static int n = 0;
  return n;
}

inline int &checks() {
  static int n = 0;
  return n;
}

inline const char *&current() {
  static const char *name = "";
  return name;
}

struct Registrar {
  Registrar(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char *file, int line, const std::string &what) {
  failures()++;
  std::printf("  FAIL %s\n       %s:%d: %s\n", current(), file, line, what.c_str());
}

// Where tests may write images for review (set from argv by main).
inline std::string &output_dir() {
  static std::string dir;
  return dir;
}

}  // namespace host_test

#define HT_CAT2(a, b) a##b
#define HT_CAT(a, b) HT_CAT2(a, b)
#define TEST(name)                                                                       \
  static void HT_CAT(test_, name)();                                                     \
  static host_test::Registrar HT_CAT(reg_, name)(#name, HT_CAT(test_, name));            \
  static void HT_CAT(test_, name)()

#define CHECK(cond)                                                       \
  do {                                                                    \
    host_test::checks()++;                                                \
    if (!(cond))                                                          \
      host_test::fail(__FILE__, __LINE__, "CHECK(" #cond ")");            \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    host_test::checks()++;                                                                          \
    auto _a = (a);                                                                                  \
    auto _b = (b);                                                                                  \
    if (!(_a == _b))                                                                                \
      host_test::fail(__FILE__, __LINE__,                                                           \
                      std::string("CHECK_EQ(" #a ", " #b "): ") + std::to_string((long long) _a) + \
                          " != " + std::to_string((long long) _b));                                 \
  } while (0)

#define CHECK_STR(a, b)                                                                                     \
  do {                                                                                                      \
    host_test::checks()++;                                                                                  \
    std::string _a = (a), _b = (b);                                                                         \
    if (_a != _b)                                                                                           \
      host_test::fail(__FILE__, __LINE__, std::string("CHECK_STR(" #a ", " #b "): \"") + _a + "\" != \"" + \
                                              _b + "\"");                                                   \
  } while (0)
