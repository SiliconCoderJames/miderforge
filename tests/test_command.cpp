// run_command 工具单测：白名单纵深防御 + 真实子进程执行（含 Job Object 沙箱路径）
#include "core/AppContext.h"
#include "tools/CommandTools.h"
#include "tools/ToolRegistry.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <doctest/doctest.h>

using miderforge::ToolRegistry;

static QJsonObject run(ToolRegistry& reg, const QString& cmd) {
    const auto res = reg.execute(QStringLiteral("run_command"),
                                 QStringLiteral("{\"command\": \"%1\"}").arg(cmd));
    {
        QFile f(QStringLiteral("./cmd-test-dump.txt"));
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(QStringLiteral("ok=%1\ntext=%2\n").arg(res.ok).arg(res.text).toUtf8());
    }
    CHECK(res.ok);
    return QJsonDocument::fromJson(res.text.toUtf8()).object();
}

TEST_CASE("run_command：git --version 执行并回填输出") {
    ToolRegistry reg;
    miderforge::CommandTools::registerAll(reg);
    miderforge::AppContext::instance().workspaceRoot = QStringLiteral(".");

    const QJsonObject out = run(reg, QStringLiteral("git --version"));
    CHECK(out.value("ok").toBool());
    CHECK(out.value("exit_code").toInt() == 0);
    CHECK(out.value("output").toString().contains(QStringLiteral("git")));
    CHECK(out.value("timed_out").toBool() == false);
}

TEST_CASE("run_command：非白名单命令被工具层拒绝（纵深防御第二道）") {
    ToolRegistry reg;
    miderforge::CommandTools::registerAll(reg);
    miderforge::AppContext::instance().workspaceRoot = QStringLiteral(".");

    const auto res = reg.execute(QStringLiteral("run_command"),
                                 QStringLiteral("{\"command\": \"whoami\"}"));
    CHECK_FALSE(res.ok);
    CHECK(res.text.contains(QStringLiteral("白名单")));
}

TEST_CASE("M5 P3：取消令牌语义（请求→查询→消费复位）") {
    ToolRegistry reg;
    CHECK_FALSE(reg.toolCancelRequested());
    reg.requestToolCancel();
    CHECK(reg.toolCancelRequested());
    CHECK(reg.consumeToolCancel());
    CHECK_FALSE(reg.toolCancelRequested());
    CHECK_FALSE(reg.consumeToolCancel()); // 二次消费为否（标志已复位）
}

TEST_CASE("M5 P3：run_command 入口快检响应取消令牌") {
    ToolRegistry reg;
    miderforge::CommandTools::registerAll(reg);
    miderforge::AppContext::instance().workspaceRoot = QStringLiteral(".");

    reg.requestToolCancel();
    const auto res = reg.execute(QStringLiteral("run_command"),
                                 QStringLiteral("{\"command\": \"git --version\"}"));
    CHECK_FALSE(res.ok);
    CHECK(res.text.contains(QStringLiteral("取消")));
    // AgentLoop 在工具批收尾时消费判停；工具本身只读不消费
    CHECK(reg.toolCancelRequested());
    CHECK(reg.consumeToolCancel());
}
