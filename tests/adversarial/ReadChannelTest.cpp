// P0-2 对抗测试：读取通道的永不解禁——名字清单 + 读取侧保护区（应用自身数据）约束
// read_file / list_dir / search_files / read_skill：任何档位拒绝，记 read_denied 事件
//（六元组：actor/authorizer/target/operation/outcome/reason；ts/task_id 走 events 列），
// 返回内容不含文件内容。fixture 全部在 QTemporaryDir 内；provider 配置经 QStandardPaths
// 测试模式隔离到 qttest，不碰真实用户目录。端到端用例用本地 mock SSE 驱动真实 AgentLoop。
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "core/EventBus.h"
#include "db/Database.h"
#include "llm/ChatClient.h"
#include "llm/ProviderManager.h"
#include "mock_sse_server.h"
#include "tools/ExtraTools.h"
#include "tools/FileTools.h"
#include "tools/PermissionGate.h"
#include "tools/ToolRegistry.h"
#include "util/AppDirs.h"
#include "util/PathReal.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <doctest/doctest.h>
#include <optional>

using miderforge::AgentLoop;
using miderforge::AppContext;
using miderforge::ChatClient;
using miderforge::Database;
using miderforge::EventBus;
using miderforge::PermissionGate;
using miderforge::PermissionMode;
using miderforge::ProviderManager;
using miderforge::ToolRegistry;

namespace {

constexpr const char* kSecret = "TOPSECRET-P0-CANARY-7Q2X";

// 读取保护 fixture：全部落在 QTemporaryDir（不碰真实用户目录）
struct ReadFixture {
    QTemporaryDir tmp;
    QString protectedDir;
    QString secretFile;
    QString namedSecret;
    QString normalFile;
    bool ready = false;
    ReadFixture() {
        protectedDir = tmp.path() + QStringLiteral("/appdata-protected");
        secretFile = protectedDir + QStringLiteral("/providers.json");
        namedSecret = tmp.path() + QStringLiteral("/credentials.json");
        normalFile = tmp.path() + QStringLiteral("/normal/a.cpp");
        ready = tmp.isValid() && QDir().mkpath(protectedDir)
                && QDir().mkpath(tmp.path() + QStringLiteral("/normal"));
        if (ready) {
            QFile f(secretFile);
            ready = f.open(QIODevice::WriteOnly) && f.write(kSecret) > 0;
            f.close();
        }
        if (ready) {
            QFile g(namedSecret);
            ready = g.open(QIODevice::WriteOnly) && g.write(kSecret) > 0;
            g.close();
        }
        if (ready) {
            QFile h(normalFile);
            ready = h.open(QIODevice::WriteOnly) && h.write("int main() { return 0; }") > 0;
            h.close();
        }
    }
};

void installProtection(const ReadFixture& fx) {
    AppContext::instance().protectedReadRoots.clear();
    AppContext::instance().protectedReadRoots
        << miderforge::pathreal::resolveReal(fx.protectedDir);
    REQUIRE_FALSE(AppContext::instance().protectedReadRoots.first().isEmpty());
}

void resetProtection() {
    AppContext::instance().protectedReadRoots.clear();
}

} // namespace

TEST_SUITE("adversarial.read_channel")
{

TEST_CASE("read_file 保护区命中：三档全拒 + 原因码") {
    ReadFixture fx;
    REQUIRE(fx.ready);
    installProtection(fx);
    PermissionGate gate;
    for (auto mode : {PermissionMode::Suggest, PermissionMode::AutoEdit, PermissionMode::FullAccess}) {
        CHECK(gate.evaluate(mode, PermissionGate::Kind::ReadFile, fx.secretFile, fx.tmp.path())
              == PermissionGate::Decision::Denied);
        CHECK_FALSE(
            gate.hardDenyReason(PermissionGate::Kind::ReadFile, fx.secretFile, fx.tmp.path()).isEmpty());
    }
}

TEST_CASE("read_file 名字清单命中回归：三档全拒") {
    ReadFixture fx;
    REQUIRE(fx.ready);
    resetProtection(); // 本用例只验证名字清单，不受保护区注入影响
    PermissionGate gate;
    for (auto mode : {PermissionMode::Suggest, PermissionMode::AutoEdit, PermissionMode::FullAccess}) {
        CHECK(gate.evaluate(mode, PermissionGate::Kind::ReadFile, fx.namedSecret, fx.tmp.path())
              == PermissionGate::Decision::Denied);
    }
    // 正常文件保持可读（三档自动）
    CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::ReadFile, fx.normalFile,
                        fx.tmp.path())
          == PermissionGate::Decision::Allowed);
}

TEST_CASE("list_dir：保护区条目被隐去且计数返回") {
    ReadFixture fx;
    REQUIRE(fx.ready);
    installProtection(fx);
    AppContext::instance().workspaceRoot = fx.tmp.path();
    // 入口层：直接列保护目录被拒
    PermissionGate gate;
    CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::ReadFile, fx.protectedDir,
                        fx.tmp.path())
          == PermissionGate::Decision::Denied);
    // handler 层：列父目录时保护项被跳过、计数随 envelope 返回
    ToolRegistry reg;
    miderforge::FileTools::registerAll(reg);
    const QString args = QString::fromUtf8(
        QJsonDocument(QJsonObject{{"path", fx.tmp.path()}}).toJson(QJsonDocument::Compact));
    const auto res = reg.execute(QStringLiteral("list_dir"), args);
    REQUIRE(res.ok);
    const QJsonObject out = QJsonDocument::fromJson(res.text.toUtf8()).object();
    const QJsonDocument namesDoc(out.value("entries").toArray());
    const QByteArray names = namesDoc.toJson(QJsonDocument::Compact);
    CHECK_FALSE(names.contains("appdata-protected"));
    CHECK_FALSE(names.contains("credentials.json"));
    CHECK(out.value("skipped_protected").toInt() == 2);
    CHECK_FALSE(res.text.contains(kSecret));
}

TEST_CASE("search_files：保护项在内容匹配前被跳过，结果与文本零泄露") {
    ReadFixture fx;
    REQUIRE(fx.ready);
    installProtection(fx);
    AppContext::instance().workspaceRoot = fx.tmp.path();
    ToolRegistry reg;
    miderforge::FileTools::registerAll(reg);
    const QString args = QString::fromUtf8(QJsonDocument(QJsonObject{
                                               {"pattern", QStringLiteral("*")},
                                               {"content", QStringLiteral("TOPSECRET")},
                                               {"path", fx.tmp.path()},
                                           }).toJson(QJsonDocument::Compact));
    const auto res = reg.execute(QStringLiteral("search_files"), args);
    REQUIRE(res.ok);
    const QJsonObject out = QJsonDocument::fromJson(res.text.toUtf8()).object();
    for (const auto& h : out.value("hits").toArray()) {
        CHECK_FALSE(h.toString().contains(QStringLiteral("credentials.json")));
        CHECK_FALSE(h.toString().contains(QStringLiteral("providers.json")));
    }
    CHECK(out.value("skipped_protected").toInt() >= 2);
    CHECK_FALSE(res.text.contains(kSecret));
}

TEST_CASE("read_skill：判定对象为实际读取路径，命中名字清单即拒") {
    resetProtection();
    PermissionGate gate;
    // 与 AgentLoop::classifyTarget 同构的路径组合
    const QString badSkill = miderforge::appdirs::file(
        QStringLiteral("skills/%1/SKILL.md").arg(QStringLiteral("credentials")));
    CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::ReadFile, badSkill,
                        miderforge::appdirs::workspaceRoot())
          == PermissionGate::Decision::Denied);
    const QString okSkill = miderforge::appdirs::file(
        QStringLiteral("skills/cpp-cmake-qt-build/SKILL.md"));
    CHECK(gate.evaluate(PermissionMode::FullAccess, PermissionGate::Kind::ReadFile, okSkill,
                        miderforge::appdirs::workspaceRoot())
          == PermissionGate::Decision::Allowed);
}

TEST_CASE("端到端：模型请求读保护文件 → read_denied 落表 + 回填不含文件内容") {
    ReadFixture fx;
    REQUIRE(fx.ready);
    installProtection(fx);
    AppContext::instance().workspaceRoot = fx.tmp.path();

    // events 基建（真实双写：JSONL + SQLite events 表）
    Database db;
    REQUIRE(db.open(fx.tmp.path() + QStringLiteral("/events-test.db")));
    EventBus bus(fx.tmp.path() + QStringLiteral("/events.jsonl"));
    bus.setDatabase(&db);

    ToolRegistry reg;
    miderforge::FileTools::registerAll(reg);
    miderforge::ExtraTools::registerAll(reg, &db, nullptr);

    // 供应商：模板落在 qttest 隔离目录，base_url 指向本地 mock（不碰真实用户目录）
    MockSseServer server;
    REQUIRE(server.start());
    ProviderManager providers;
    REQUIRE(providers.initializeFromTemplate());
    // mock 不校验 Key，但 ChatClient::start 拒绝未配置供应商——落一把假 Key（DPAPI 加密进 qttest 目录）
    REQUIRE(providers.saveKey(QStringLiteral("zhipu"), QStringLiteral("sk-mock-adversarial")));
    REQUIRE(providers.setBaseUrl(QStringLiteral("zhipu"),
                                 QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port())));
    REQUIRE(providers.setActive(QStringLiteral("zhipu")));

    ChatClient chat;
    AgentLoop loop({&chat, &reg, &providers, &bus, &db, nullptr, nullptr});

    // 场景1：read_file 工具调用（读保护文件）→ 场景2：正文收束 → 场景3：收尾提炼回 JSON
    const QString callArgs = QString::fromUtf8(
        QJsonDocument(QJsonObject{{"path", fx.secretFile}}).toJson(QJsonDocument::Compact));
    const QJsonObject toolCall{
        {"index", 0},
        {"id", QStringLiteral("call_1")},
        {"type", QStringLiteral("function")},
        {"function", QJsonObject{
            {"name", QStringLiteral("read_file")},
            {"arguments", callArgs},
        }},
    };
    const QJsonObject deltaObj{{"tool_calls", QJsonArray{toolCall}}};
    const QJsonObject chunkObj{
        {"choices", QJsonArray{QJsonObject{{"delta", deltaObj}}}},
    };
    const QString chunk1 = QString::fromUtf8(QJsonDocument(chunkObj).toJson(QJsonDocument::Compact));
    MockScenario sc1;
    sc1.chunks = {QByteArray("data: " + chunk1.toUtf8() + "\n\n"),
                  QByteArray("data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
                             "data: [DONE]\n\n")};
    MockScenario sc2;
    sc2.chunks = {QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"已拒绝，任务结束\"}}]}\n\n"
                             "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                             "data: [DONE]\n\n")};
    MockScenario sc3;
    sc3.chunks = {QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"{}\"}}]}\n\n"
                             "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                             "data: [DONE]\n\n")};
    server.setScenarios({sc1, sc2, sc3});

    bool done = false;
    bool failedFlag = false;
    bool denySeen = false;
    QString denyText;
    QObject::connect(&loop, &AgentLoop::loopFinished, [&](bool, const QString&) { done = true; });
    QObject::connect(&loop, &AgentLoop::loopFailed, [&](const QString&) { failedFlag = true; });
    QObject::connect(&loop, &AgentLoop::toolCallFinished,
                     [&](const QString& callId, bool ok, const QString& text, qint64) {
                         if (callId == QStringLiteral("call_1")) {
                             denySeen = true;
                             denyText = text;
                             CHECK_FALSE(ok); // 被拒的工具调用必须以失败回填
                         }
                     });

    loop.start(QStringLiteral("读取配置"));
    QElapsedTimer timer;
    timer.start();
    while (!done && !failedFlag && timer.elapsed() < 30000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    REQUIRE(done);
    CHECK_FALSE(failedFlag);
    REQUIRE(denySeen);
    // ③ 返回内容不含文件内容
    CHECK_FALSE(denyText.contains(kSecret));

    // ① 事件落表：read_denied，六元组齐全
    const auto rows = db.query(QStringLiteral(
        "SELECT ts, task_id, payload_json FROM events WHERE type='read_denied'"));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].value("ts").toLongLong() > 0);
    const QJsonObject payload = QJsonDocument::fromJson(
        rows[0].value("payload_json").toString().toUtf8()).object();
    CHECK(payload.value("actor").toString() == QStringLiteral("agent"));
    CHECK(payload.value("authorizer").toString() == QStringLiteral("permission_gate"));
    CHECK(payload.value("operation").toString() == QStringLiteral("read"));
    CHECK(payload.value("outcome").toString() == QStringLiteral("denied"));
    CHECK_FALSE(payload.value("reason").toString().isEmpty());
    CHECK(payload.value("target").toString().contains(QStringLiteral("appdata-protected")));
    CHECK(payload.contains(QStringLiteral("tool")));

    // ② 双写：JSONL 同步留痕
    QFile jsonl(fx.tmp.path() + QStringLiteral("/events.jsonl"));
    REQUIRE(jsonl.open(QIODevice::ReadOnly));
    CHECK(jsonl.readAll().contains("read_denied"));

    resetProtection();
}

} // TEST_SUITE
