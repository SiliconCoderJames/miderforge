// doctest 主入口：QCoreApplication（支撑带事件循环的流式测试）
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    const int rc = ctx.run();
    return rc;
}
