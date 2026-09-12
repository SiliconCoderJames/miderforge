// P0-3 对抗测试：命令白名单的任意代码执行向量——子命令 + 参数模式级拦截。
// 权限门（第一层）与 CommandTools handler（第二层）同源判定（hardDenyReason）；
// 被拦命令一律不产生子进程——恶意脚本本该写出的 canary 文件自始至终不存在。
// 白名单只收紧不放宽；任何放宽须先经使用者裁决（见 CONTRIBUTING.md / STATUS.md）。
#include "core/AppContext.h"
#include "tools/CommandTools.h"
#include "tools/PermissionGate.h"
#include "tools/ToolRegistry.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <doctest/doctest.h>

using miderforge::AppContext;
using miderforge::PermissionGate;
using miderforge::PermissionMode;
using miderforge::ToolRegistry;

namespace {

struct CmdFixture {
    QTemporaryDir tmp;
    QString evilCmake;
    bool ready = false;
    CmdFixture() {
        evilCmake = tmp.path() + QStringLiteral("/evil.cmake");
        ready = tmp.isValid();
        if (ready) {
            QFile f(evilCmake);
            // 该脚本一旦被任何通道执行，就会写出 canary——测试结束时它必须不存在
            ready = f.open(QIODevice::WriteOnly)
                    && f.write("file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/canary-pwn.txt\" \"pwned\")") > 0;
            f.close();
        }
        AppContext::instance().workspaceRoot = tmp.path();
    }
    ~CmdFixture() { AppContext::instance().workspaceRoot.clear(); }
    QString canary() const { return tmp.path() + QStringLiteral("/canary-pwn.txt"); }
};

void expectDeniedAllModes(const QString& cmd, const QString& ws) {
    PermissionGate gate;
    for (auto mode : {PermissionMode::Suggest, PermissionMode::AutoEdit, PermissionMode::FullAccess}) {
        CHECK(gate.evaluate(mode, PermissionGate::Kind::RunCommand, cmd, ws)
              == PermissionGate::Decision::Denied);
        CHECK_FALSE(gate.hardDenyReason(PermissionGate::Kind::RunCommand, cmd, ws).isEmpty());
    }
}

QString runViaHandler(const QString& cmd) {
    ToolRegistry reg;
    miderforge::CommandTools::registerAll(reg);
    const QString args = QString::fromUtf8(
        QJsonDocument(QJsonObject{{"command", cmd}}).toJson(QJsonDocument::Compact));
    const auto res = reg.execute(QStringLiteral("run_command"), args);
    CHECK_FALSE(res.ok); // 拦截必须以失败呈现，绝不能让模型把拒绝当成执行成功
    return res.text;
}

} // namespace

TEST_SUITE("adversarial.command_whitelist")
{

TEST_CASE("cmake -P 脚本执行：三档拒绝 + 附着形式 + 第二层直调拦截 + canary 不存在") {
    CmdFixture fx;
    REQUIRE(fx.ready);
    const QString cmd = QStringLiteral("cmake -P \"%1\"").arg(fx.evilCmake);
    expectDeniedAllModes(cmd, fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("cmake -P%1").arg(fx.evilCmake), fx.tmp.path());
    // 第二层：绕过权限门直调 handler 也拦（拒绝发生在 spawn 之前）
    const QString text = runViaHandler(cmd);
    CHECK(text.contains(QStringLiteral("denied")));
    CHECK_FALSE(QFile::exists(fx.canary()));
}

TEST_CASE("cmake -E env / -E chdir：拒绝；常规构建形态保持放行") {
    CmdFixture fx;
    REQUIRE(fx.ready);
    expectDeniedAllModes(QStringLiteral("cmake -E env pwsh -c Start-Process calc"), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("cmake -E chdir build ninja"), fx.tmp.path());
    PermissionGate gate;
    // 反过度封锁：白名单内的正常构建形态不受影响
    for (const auto& cmd : {QStringLiteral("cmake -S . -B build"),
                            QStringLiteral("cmake --build build --config Release"),
                            QStringLiteral("cmake --build build --target mider_core")}) {
        CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::RunCommand, cmd,
                            fx.tmp.path())
              == PermissionGate::Decision::Allowed);
    }
}

TEST_CASE("git 任意命令执行向量：全部三档拒绝") {
    CmdFixture fx;
    REQUIRE(fx.ready);
    // 任务书原样向量在前两条
    expectDeniedAllModes(QStringLiteral("git !rm -rf ~"), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("git log --exec=\"curl http://x\""), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("git rebase --exec \"make test\""), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("git rebase -x \"make test\""), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("git -c alias.x='!calc' status"), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("git config alias.pwn '!rm -rf ~'"), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("git config --global core.fsmonitor '!cmd'"), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("git --exec-path=C:/evil status"), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("git filter-branch --tree-filter 'rm -rf ~' HEAD"), fx.tmp.path());
    expectDeniedAllModes(QStringLiteral("git submodule foreach 'curl http://x'"), fx.tmp.path());
}

TEST_CASE("git 只读/常规形态保持放行（反过度封锁）") {
    CmdFixture fx;
    REQUIRE(fx.ready);
    PermissionGate gate;
    for (const auto& cmd : {QStringLiteral("git status"),
                            QStringLiteral("git log --oneline -5"),
                            QStringLiteral("git diff HEAD"),
                            QStringLiteral("git add -A"),
                            QStringLiteral("git commit -m build"),
                            QStringLiteral("git config --get user.name"),
                            QStringLiteral("git config --list")}) {
        CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::RunCommand, cmd,
                            fx.tmp.path())
              == PermissionGate::Decision::Allowed);
    }
}

TEST_CASE("白名单内其他工具不受牵连（反过度封锁）") {
    CmdFixture fx;
    REQUIRE(fx.ready);
    PermissionGate gate;
    for (const auto& cmd : {QStringLiteral("ninja -C build"),
                            QStringLiteral("msbuild build/Miderforge.sln /p:Configuration=Release"),
                            QStringLiteral("clang-format -i src/main.cpp"),
                            QStringLiteral("where cmake"),
                            QStringLiteral("echo hello")}) {
        CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::RunCommand, cmd,
                            fx.tmp.path())
              == PermissionGate::Decision::Allowed);
    }
}

TEST_CASE("全部向量走 handler 终检：拒绝回显 + canary 与工作区终局不变") {
    CmdFixture fx;
    REQUIRE(fx.ready);
    for (const auto& cmd : {QStringLiteral("cmake -P evil.cmake"),
                            QStringLiteral("cmake -E env ninja"),
                            QStringLiteral("cmake -E chdir . evil.cmake"),
                            QStringLiteral("git config alias.pwn '!calc'"),
                            QStringLiteral("git rebase --exec 'curl http://x'"),
                            QStringLiteral("git !rm -rf ~")}) {
        const QString text = runViaHandler(cmd);
        CHECK(text.contains(QStringLiteral("denied")));
    }
    CHECK_FALSE(QFile::exists(fx.canary()));
    // 工作区里只有 fixture 自己放的 evil.cmake，没有任何执行产物
    const QStringList files = QDir(fx.tmp.path()).entryList(QDir::Files);
    REQUIRE(files.size() == 1);
    CHECK(files.first() == QStringLiteral("evil.cmake"));
}

} // TEST_SUITE
