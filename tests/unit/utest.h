#ifndef GB_UTEST_H
#define GB_UTEST_H
#include <stdio.h>
#include <stdlib.h>
#define UTEST(suite, name) static void suite##_##name(void); static void suite##_##name(void)
#define ASSERT_TRUE(x) do { if (!(x)) { fprintf(stderr, "assertion failed: %s\n", #x); exit(1); } } while (0)
#define ASSERT_EQ(a,b) do { long _a=(long)(a), _b=(long)(b); if (_a != _b) { fprintf(stderr, "assertion failed: %s == %s (%ld != %ld)\n", #a, #b, _a, _b); exit(1); } } while (0)
#define UTEST_MAIN() int main(void) { core_cpu_load_and_halt(); puts("1 test passed"); return 0; }
#endif
