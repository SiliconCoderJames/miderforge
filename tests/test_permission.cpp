// 权限门单测（规格 15：权限门路径判定必须可脱离 GUI 测试）
#include "core/AppContext.h"
#include "tools/PermissionGate.h"
#include <doctest/doctest.h>

using miderforge::PermissionGate;
using miderforge::PermissionMode;

static const QString WS = QStringLiteral("C:/ws/workspace");

TEST_CASE("读文件三档均自动放行") {
    PermissionGate gate;
    for (auto mode : {PermissionMode::Suggest, PermissionMode::AutoEdit, PermissionMode::FullAccess}) {
        CHECK(gate.evaluate(mode, PermissionGate::Kind::ReadFile, QStringLiteral("C:/ws/workspace/a.cpp"), WS)
              == PermissionGate::Decision::Allowed);
    }
}

TEST_CASE("Suggest：写文件走提案确认，命令与越界写不放行") {
    PermissionGate gate;
    const auto d1 = gate.evaluate(PermissionMode::Suggest, PermissionGate::Kind::WriteFile,
                                  WS + QStringLiteral("/a.cpp"), WS);
    CHECK(d1 == PermissionGate::Decision::NeedsConfirm); // 提案 → 卡片确认
    const auto d2 = gate.evaluate(PermissionMode::Suggest, PermissionGate::Kind::RunCommand,
                                  QStringLiteral("cmake --build ."), WS);
    CHECK(d2 == PermissionGate::Decision::NeedsConfirm);
}

TEST_CASE("Auto Edit：工作区内写自动通过，区外直接拒绝") {
    PermissionGate gate;
    CHECK(gate.evaluate(PermissionMode::AutoEdit, PermissionGate::Kind::WriteFile,
                        WS + QStringLiteral("/src/main.cpp"), WS)
          == PermissionGate::Decision::Allowed);
    CHECK(gate.evaluate(PermissionMode::AutoEdit, PermissionGate::Kind::WriteFile,
                        QStringLiteral("C:/Windows/system32/evil.dll"), WS)
          == PermissionGate::Decision::Denied);
    // 相对路径分隔符差异（反斜杠）也应判定为区内
    CHECK(gate.evaluate(PermissionMode::AutoEdit, PermissionGate::Kind::WriteFile,
                        QStringLiteral("C:\\ws\\workspace\\src\\a.cpp"), WS)
          == PermissionGate::Decision::Allowed);
}

TEST_CASE("永不解禁清单：任何档位直接拒绝") {
    PermissionGate gate;
    const QStringList forbidden = {
        QStringLiteral("C:/proj/credentials.json"),
        QStringLiteral("C:/proj/my_secret.txt"),
        QStringLiteral("C:/proj/.env"),
        QStringLiteral("C:/proj/.env.local"),
        QStringLiteral("C:/proj/prod.env"),
        QStringLiteral("C:/Users/x/.ssh/id_rsa"),
        QStringLiteral("C:/proj/API_TOKEN.h"),
    };
    for (const QString& p : forbidden) {
        CHECK_MESSAGE(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::ReadFile, p, WS)
                          == PermissionGate::Decision::Denied,
                      qPrintable(p));
        CHECK_MESSAGE(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::WriteFile, p, WS)
                          == PermissionGate::Decision::Denied,
                      qPrintable(p));
    }
}

TEST_CASE("永不解禁清单：git push 保护分支与磁盘级操作") {
    PermissionGate gate;
    // feature-x 非保护分支：不命中硬拒绝
    CHECK(gate.hardDenyReason(PermissionGate::Kind::RunCommand,
                              QStringLiteral("git push origin feature-x"), WS).isEmpty());
    CHECK_FALSE(gate.hardDenyReason(PermissionGate::Kind::RunCommand,
                                    QStringLiteral("git push origin main"), WS).isEmpty());
    CHECK_FALSE(gate.hardDenyReason(PermissionGate::Kind::RunCommand,
                                    QStringLiteral("git push origin master"), WS).isEmpty());
    CHECK_FALSE(gate.hardDenyReason(PermissionGate::Kind::RunCommand,
                                    QStringLiteral("format D:"), WS).isEmpty());
    CHECK_FALSE(gate.hardDenyReason(PermissionGate::Kind::RunCommand,
                                    QStringLiteral("rd /s C:\\Windows"), WS).isEmpty());
    // 工作区内递归删除放行（build 清理场景）
    CHECK(gate.hardDenyReason(PermissionGate::Kind::RunCommand,
                              QStringLiteral("rd /s C:\\ws\\workspace\\build"), WS).isEmpty());
}

TEST_CASE("命令白名单：不在名单内的首词直接拒绝") {
    PermissionGate gate;
    CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::RunCommand,
                        QStringLiteral("powershell -Command evil"), WS)
          == PermissionGate::Decision::Denied);
    CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::RunCommand,
                        QStringLiteral("curl http://evil"), WS)
          == PermissionGate::Decision::Denied);
    CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::RunCommand,
                        QStringLiteral("cmake --build ."), WS)
          == PermissionGate::Decision::Allowed);
}

TEST_CASE("网络：Full Access 白名单放行，其余走确认") {
    PermissionGate gate;
    CHECK(gate.evaluate(PermissionMode::Suggest, PermissionGate::Kind::Network,
                        QStringLiteral("https://example.com/data.json"), WS)
          == PermissionGate::Decision::NeedsConfirm);
    CHECK(gate.evaluate(PermissionMode::AutoEdit, PermissionGate::Kind::Network,
                        QStringLiteral("https://example.com/data.json"), WS)
          == PermissionGate::Decision::NeedsConfirm);
    // 默认白名单为空：Full Access 也需确认
    CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::Network,
                        QStringLiteral("https://example.com/data.json"), WS)
          == PermissionGate::Decision::NeedsConfirm);
}

TEST_CASE("会话内总是允许：命令通道授一次全放") {
    PermissionGate gate;
    CHECK(gate.evaluate(PermissionMode::Suggest, PermissionGate::Kind::RunCommand,
                        QStringLiteral("git status"), WS)
          == PermissionGate::Decision::NeedsConfirm);
    gate.grantAlwaysForSession(PermissionGate::Kind::RunCommand);
    CHECK(gate.evaluate(PermissionMode::Suggest, PermissionGate::Kind::RunCommand,
                        QStringLiteral("git status"), WS)
          == PermissionGate::Decision::Allowed);
    gate.resetSessionGrants();
    CHECK(gate.evaluate(PermissionMode::Suggest, PermissionGate::Kind::RunCommand,
                        QStringLiteral("git status"), WS)
          == PermissionGate::Decision::NeedsConfirm);
}
