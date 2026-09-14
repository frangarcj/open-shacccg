#include <cstdio>
int test_abi();
int test_compiler();
int test_spirv();
int test_gxp();
int test_gxp_writer();
int test_usse();
int main() {
    int failures = 0;
    failures += test_abi();
    failures += test_compiler();
    failures += test_spirv();
    failures += test_gxp();
    failures += test_gxp_writer();
    failures += test_usse();
    std::printf("open-shacccg tests: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
