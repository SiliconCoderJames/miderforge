// 对抗测试 doctest 入口：独立于 mider_tests（不拖 Widgets），ctest 按 suite 拆项执行。
// tests/adversarial/ 下的用例是安全回归资产：不许删、不许 skip（见 CONTRIBUTING.md）。
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

int main(int argc, char** argv) {
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
