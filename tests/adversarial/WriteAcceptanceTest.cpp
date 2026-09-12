// 写文件能力验收（端到端）：用真实 AgentLoop + PermissionGate + FileTools，经 mock SSE 驱动
// 模型发起 write_file 工具调用，断言文件真的落到磁盘（内容逐字节一致）。
// 放在 tests/adversarial/ 只为复用该目录的 mock-SSE/AgentLoop 基建；本套件是产品验收，
// 非安全对抗（故 suite 名为 acceptance.write_file，ctest 项同名）。
// 背景：实机审计显示"对话未生成文件"是知识问答（零工具调用）+ 早前 zhipu 未配 Key，
// 写通道从未被真实验证过——本套件补上这一课。
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "core/EventBus.h"
#include "db/Database.h"
#include "llm/ChatClient.h"
#include "llm/ProviderManager.h"
#include "mock_sse_server.h"
#include "tools/ExtraTools.h"
#include "tools/FileTools.h"
#include "tools/ToolRegistry.h"
#include "util/AppDirs.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QVariantMap>
#include <doctest/doctest.h>
#include <vector>

using miderforge::AgentLoop;
using miderforge::AppContext;
using miderforge::ChatClient;
using miderforge::Database;
using miderforge::EventBus;
using miderforge::PermissionMode;
using miderforge::ProviderManager;
using miderforge::ToolRegistry;

namespace {

std::vector<QVariantMap> eventsOfType(Database& db, const QString& type) {
    return db.query(QStringLiteral("SELECT ts, type, task_id, payload_json FROM events WHERE type=?"),
                    {type});
}

QString toolCallChunk(const QString& id, const QString& name, const QString& argsJson) {
    const QJsonObject toolCall{
        {"index", 0},
        {"id", id},
        {"type", QStringLiteral("function")},
        {"function", QJsonObject{{"name", name}, {"arguments", argsJson}}},
    };
    const QJsonObject chunkObj{
        {"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"tool_calls", QJsonArray{toolCall}}}}}}},
    };
    return QString::fromUtf8(QJsonDocument(chunkObj).toJson(QJsonDocument::Compact));
}

QString contentChunk(const QString& text) {
    const QJsonObject chunkObj{
        {"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"content", text}}}}}},
    };
    return QString::fromUtf8(QJsonDocument(chunkObj).toJson(QJsonDocument::Compact));
}

QString finishChunk(const char* reason) {
    const QJsonObject chunkObj{
        {"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{}}, {"finish_reason", reason}}}},
    };
    return QStringLiteral("data: ")
         + QString::fromUtf8(QJsonDocument(chunkObj).toJson(QJsonDocument::Compact))
         + QStringLiteral("\n\n") + QStringLiteral("data: [DONE]\n\n");
}

MockScenario scenarioToolCall(const QString& id, const QString& name, const QString& argsJson) {
    MockScenario sc;
    sc.chunks = {QByteArray("data: " + toolCallChunk(id, name, argsJson).toUtf8() + "\n\n"),
                 QByteArray("data: " + finishChunk("tool_calls").toUtf8())};
    return sc;
}

MockScenario scenarioStop(const QString& text) {
    MockScenario sc;
    sc.chunks = {QByteArray("data: " + contentChunk(text).toUtf8() + "\n\n"),
                 QByteArray("data: " + finishChunk("stop").toUtf8())};
    return sc;
}

bool runToCompletion(AgentLoop& loop, const QString& goal) {
    bool finished = false;
    QObject::connect(&loop, &AgentLoop::loopFinished, [&](bool, const QString&) { finished = true; });
    QObject::connect(&loop, &AgentLoop::loopFailed, [&](const QString&) { finished = true; });
    loop.start(goal);
    QElapsedTimer guard;
    guard.start();
    while (!finished && guard.elapsed() < 90000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return finished;
}

QString readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

} // namespace

TEST_SUITE("acceptance.write_file")
{

TEST_CASE("Auto Edit：模型发起 write_file → 文件落盘且内容逐字节一致（自动建父目录）") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    AppContext::instance().workspaceRoot = tmp.path();
    AppContext::instance().permissionMode = PermissionMode::AutoEdit; // 工作区内写自动
    AppContext::instance().protectedReadRoots.clear();

    Database db;
    REQUIRE(db.open(tmp.path() + QStringLiteral("/events.db")));
    EventBus bus(tmp.path() + QStringLiteral("/events.jsonl"));
    bus.setDatabase(&db);
    ToolRegistry reg;
    miderforge::FileTools::registerAll(reg);
    miderforge::ExtraTools::registerAll(reg, &db, nullptr);

    MockSseServer server;
    REQUIRE(server.start());
    ProviderManager providers;
    REQUIRE(providers.initializeFromTemplate());
    REQUIRE(providers.saveKey(QStringLiteral("zhipu"), QStringLiteral("sk-mock-write")));
    REQUIRE(providers.setBaseUrl(QStringLiteral("zhipu"),
                                 QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port())));
    REQUIRE(providers.setActive(QStringLiteral("zhipu")));

    ChatClient chat;
    AgentLoop loop({&chat, &reg, &providers, &bus, &db, nullptr, nullptr});

    // 目标路径放在**尚不存在**的子目录里：顺带验证"自动建父目录"
    const QString target = tmp.path() + QStringLiteral("/交付/说明.md");
    const QString payload = QStringLiteral("# 验收文件\n中文内容：你好，Miderforge！\nEOF\n");
    const QString args = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"path", target},
        {"content", payload},
    }).toJson(QJsonDocument::Compact));
    const QString finalizeJson = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"result_summary", QStringLiteral("已写入说明.md")},
        {"session_summary", QStringLiteral("验收写文件")},
    }).toJson(QJsonDocument::Compact));
    server.setScenarios({scenarioToolCall(QStringLiteral("call_w1"), QStringLiteral("write_file"), args),
                         scenarioStop(QStringLiteral("文件已写入")),
                         scenarioStop(finalizeJson)});

    bool toolOk = false;
    QString toolText;
    QObject::connect(&loop, &AgentLoop::toolCallFinished,
                     [&](const QString&, bool ok, const QString& text, qint64) {
                         toolOk = ok;
                         toolText = text;
                     });

    REQUIRE(runToCompletion(loop, QStringLiteral("在工作区新建一个说明文件")));

    // ① 物理结果：文件存在且内容逐字节一致
    CHECK(QFile::exists(target));
    CHECK(readAll(target) == payload);
    CHECK(QFileInfo(target).size() == payload.toUtf8().size());
    // ② 工具回执成功
    CHECK(toolOk);
    CHECK(toolText.contains(QStringLiteral("写入成功")));
    // ③ 审计：tool_result 记录且 ok=true
    const auto results = eventsOfType(db, QStringLiteral("tool_result"));
    REQUIRE(!results.empty());
    bool sawWrite = false;
    for (const auto& r : results) {
        const QJsonObject p = QJsonDocument::fromJson(
                                  r.value(QStringLiteral("payload_json")).toString().toUtf8()).object();
        if (p.value(QStringLiteral("tool")).toString() == QStringLiteral("write_file")) {
            sawWrite = true;
            CHECK(p.value(QStringLiteral("ok")).toBool());
        }
    }
    CHECK(sawWrite);
}

TEST_CASE("Suggest：写操作先要确认——未确认前磁盘上不出现文件，确认后才落盘") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    AppContext::instance().workspaceRoot = tmp.path();
    AppContext::instance().permissionMode = PermissionMode::Suggest; // 写=提案待确认
    AppContext::instance().protectedReadRoots.clear();

    Database db;
    REQUIRE(db.open(tmp.path() + QStringLiteral("/events.db")));
    EventBus bus(tmp.path() + QStringLiteral("/events.jsonl"));
    bus.setDatabase(&db);
    ToolRegistry reg;
    miderforge::FileTools::registerAll(reg);
    miderforge::ExtraTools::registerAll(reg, &db, nullptr);

    MockSseServer server;
    REQUIRE(server.start());
    ProviderManager providers;
    REQUIRE(providers.initializeFromTemplate());
    REQUIRE(providers.saveKey(QStringLiteral("zhipu"), QStringLiteral("sk-mock-write")));
    REQUIRE(providers.setBaseUrl(QStringLiteral("zhipu"),
                                 QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port())));
    REQUIRE(providers.setActive(QStringLiteral("zhipu")));

    ChatClient chat;
    AgentLoop loop({&chat, &reg, &providers, &bus, &db, nullptr, nullptr});

    const QString target = tmp.path() + QStringLiteral("/needs-confirm.txt");
    const QString payload = QStringLiteral("确认后才会出现的内容");
    const QString args = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"path", target},
        {"content", payload},
    }).toJson(QJsonDocument::Compact));
    server.setScenarios({scenarioToolCall(QStringLiteral("call_w2"), QStringLiteral("write_file"), args),
                         scenarioStop(QStringLiteral("已确认写入")),
                         scenarioStop(QStringLiteral("{}"))});

    QString pendingCallId;
    bool confirmAsked = false;
    bool finished = false;
    QObject::connect(&loop, &AgentLoop::toolAwaitingConfirm,
                     [&](const QString& callId, const QString&, const QString&, const QString&) {
                         confirmAsked = true;
                         pendingCallId = callId;
                     });
    QObject::connect(&loop, &AgentLoop::loopFinished, [&](bool, const QString&) { finished = true; });
    QObject::connect(&loop, &AgentLoop::loopFailed, [&](const QString&) { finished = true; });

    loop.start(QStringLiteral("把结论写进文件"));
    QElapsedTimer guard;
    guard.start();
    // 阶段一：等确认卡片；在确认前磁盘上不应有文件
    while (!confirmAsked && guard.elapsed() < 30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    CHECK(confirmAsked);
    CHECK_FALSE(QFile::exists(target)); // 未确认 = 绝不落盘

    // 阶段二：模拟用户点"允许"→ 继续跑完
    if (confirmAsked)
        loop.resumePermission(pendingCallId, 0);
    while (!finished && guard.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    CHECK(finished);
    CHECK(QFile::exists(target)); // 确认后落盘
    CHECK(readAll(target) == payload);
    AppContext::instance().permissionMode = PermissionMode::Suggest; // 还原
}

} // TEST_SUITE
