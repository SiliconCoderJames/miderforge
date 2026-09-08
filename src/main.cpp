// Miderforge 入口：控制台 UTF-8 → 数据目录 → 单实例 → 日志 → 主题 → 供应商 → 主窗口
#include "app/MainWindow.h"
#include "app/FirstRunWizard.h"
#include "app/Theme.h"
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "core/EventBus.h"
#include "core/Scheduler.h"
#include "db/Database.h"
#include "llm/ChatClient.h"
#include "llm/ProviderManager.h"
#include "memory/MemoryManager.h"
#include "notify/EmailNotifier.h"
#include "skills/SkillManager.h"
#include "tools/CommandTools.h"
#include "tools/ExtraTools.h"
#include "tools/FileTools.h"
#include "tools/ToolRegistry.h"
#include "util/AppDirs.h"
#include "util/Log.h"
#include <QApplication>
#include <QLibraryInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QThread>
#include <QTranslator>
#include <spdlog/spdlog.h>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace miderforge;

int main(int argc, char* argv[]) {
#ifdef Q_OS_WIN
    SetConsoleOutputCP(65001); // 规格硬约束：main 首行；GUI 子系统下无害
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Miderforge"));
    QApplication::setOrganizationName(QStringLiteral("Miderforge"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    // 数据目录与日志
    if (!appdirs::ensureLayout()) {
        QMessageBox::warning(nullptr, QStringLiteral("Miderforge"),
                             QStringLiteral("应用数据目录创建失败：%1").arg(appdirs::root()));
        return 1;
    }
    logutil::init(appdirs::file(QStringLiteral("logs")));

    // 单实例（决策: QLockFile 最简实现；规格要求 Windows 单实例驻留）
    QLockFile lock(appdirs::file(QStringLiteral("miderforge.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(200)) {
        QMessageBox::warning(nullptr, QStringLiteral("Miderforge"),
                             QStringLiteral("Miderforge 已在运行（单实例）。"));
        return 0;
    }

    theme::apply(app);

    // Qt 内置控件文案中文化（QDialogButtonBox 的 Save/Cancel 等默认是英文）
    QTranslator qtTranslator;
    if (qtTranslator.load(QStringLiteral("qtbase_zh_CN.qm"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslator);

    AppContext::instance().workspaceRoot = appdirs::workspaceRoot();

    // 供应商配置：不存在或无可用 Key → 首次配置向导
    ProviderManager providers;
    bool loaded = providers.load();
    if (!loaded)
        providers.initializeFromTemplate();
    const ProviderConfig* active = providers.activeProvider();
    if (!active || !active->configured) {
        FirstRunWizard wizard(&providers);
        wizard.exec();
        active = providers.activeProvider();
        if (!active || !active->configured) {
            // 向导未完成配置不进主循环：半配置状态下 GUI 起来后每次发消息都会失败，状态混乱
            QMessageBox::information(nullptr, QStringLiteral("Miderforge"),
                                     QStringLiteral("尚未完成大模型配置，Miderforge 已退出。"
                                                    "下次启动将重新进入配置向导。"));
            return 0;
        }
    }

    // 工具注册表（v1 全集 7 工具）+ 事件审计 + Agent 循环
    ToolRegistry tools;
    FileTools::registerAll(tools);
    CommandTools::registerAll(tools);
    ExtraTools::registerAll(tools);

    EventBus events(appdirs::file(QStringLiteral("logs/events.jsonl")));
    Database db;
    if (!db.open(appdirs::file(QStringLiteral("miderforge.db")))) {
        QMessageBox::warning(nullptr, QStringLiteral("Miderforge"),
                             QStringLiteral("数据库打开失败：%1").arg(db.lastError()));
        return 1;
    }
    db.loadVecExtension(); // v1：仅要求加载成功，不使用向量检索
    events.setDatabase(&db); // 事件双写 events 表
    MemoryManager memory(&db, appdirs::file(QStringLiteral("memory/core.md")));
    if (memory.loadL1().isEmpty())
        memory.saveL1(QStringLiteral("# 用户画像\n\n# 编码偏好\n\n# 禁区\n\n# 活跃项目状态\n"));
    SkillManager skills(&db, appdirs::file(QStringLiteral("skills")));
    skills.ensureSeedSkill(); // 规格交付物：首技能种子 cpp-cmake-qt-build

    ChatClient chat;
    AgentLoop loop({&chat, &tools, &providers, &events, &db, &memory, &skills});
    Scheduler scheduler(&db, &loop);
    EmailNotifier mail(&events);
    mail.loadConfig();

    MainWindow win(&providers, &loop, &events, &db, &memory, &skills, &mail);
    win.show();
    const int rc = app.exec();

    if (auto lg = logutil::logger())
        lg->info("Miderforge 退出，rc={}", rc);
    return rc;
}
