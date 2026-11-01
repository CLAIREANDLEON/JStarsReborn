// JStarsReborn: failed GPU assertions are fatal and reported through the runtime (exit code 5),
// never ignored: continuing after a violated invariant could corrupt guest or GPU state.
#include <cstdio>
#include <cstdlib>
#include "common/assert.h"
extern "C" void jgpu_fatal(const char* what);
void assert_fail_impl() { jgpu_fatal("assertion failed (see the preceding gpu[...critical] log line)"); std::abort(); }
[[noreturn]] void unreachable_impl() { jgpu_fatal("unreachable code reached"); std::abort(); }
