// run_command 工具单测：白名单纵深防御 + 真实子进程执行（含 Job Object 沙箱路径）
#include "core/AppContext.h"
#include "tools/CommandTools.h"
#include "tools/ToolRegistry.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <doctest/doctest.h>

using miderforge::ToolRegistry;

static QJsonObject run(ToolRegistry& reg, const QString& cmd) {
    const auto res = reg.execute(QStringLiteral("run_command"),
                                 QStringLiteral("{\"command\": \"%1\"}").arg(cmd));
    INFO(res.text.toUtf8().data());
    CHECK(res.ok);
    return QJsonDocument::fromJson(res.text.toUtf8()).object();
}

TEST_CASE("run_command：echo 执行并回填输出") {
    ToolRegistry reg;
    miderforge::CommandTools::registerAll(reg);
    AppContext::instance().workspaceRoot = QStringLiteral(".");

    const QJsonObject out = run(reg, QStringLiteral("echo hello-miderforge"));
    CHECK(out.value("ok").toBool());
    CHECK(out.value("exit_code").toInt() == 0);
    CHECK(out.value("output").toString().contains(QStringLiteral("hello-miderforge")));
    CHECK(out.value("timed_out").toBool() == false);
}

TEST_CASE("run_command：非白名单命令被工具层拒绝（纵深防御第二道）") {
    ToolRegistry reg;
    miderforge::CommandTools::registerAll(reg);
    AppContext::instance().workspaceRoot = QStringLiteral(".");

    const auto res = reg.execute(QStringLiteral("run_command"),
                                 QStringLiteral("{\"command\": \"whoami\"}"));
    CHECK_FALSE(res.ok);
    CHECK(res.text.contains(QStringLiteral("白名单")));
}
