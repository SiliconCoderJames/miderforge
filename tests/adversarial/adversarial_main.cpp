// 对抗测试 doctest 入口：独立于 mider_tests（不拖 Widgets），ctest 按 suite 拆项执行。
// tests/adversarial/ 下的用例是安全回归资产：不许删、不许 skip（见 CONTRIBUTING.md）。
// QCoreApplication：读取通道端到端用例要驱动 ChatClient/AgentLoop 事件循环。
// QStandardPaths 测试模式：appdirs 隔离到 qttest 目录，用例不触碰真实用户数据目录。
#define DOCTEST_CONFIG_IMPLEMENT
#include <QCoreApplication>
#include <QStandardPaths>
#include <cstdio>
#include <doctest/doctest.h>

int main(int argc, char** argv) {
    // 无缓冲：进程 fastfail 时也不丢诊断输出
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Miderforge"));
    QCoreApplication::setApplicationName(QStringLiteral("Miderforge"));
    QStandardPaths::setTestModeEnabled(true);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
