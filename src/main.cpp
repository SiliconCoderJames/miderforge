// Miderforge 入口：控制台 UTF-8 → 数据目录 → 单实例 → 日志 → 主题 → 供应商 → 主窗口
#include "app/MainWindow.h"
#include "app/FirstRunWizard.h"
#include "app/Theme.h"
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "llm/ChatClient.h"
#include "llm/ProviderManager.h"
#include "tools/FileTools.h"
#include "tools/ToolRegistry.h"
#include "util/AppDirs.h"
#include "util/Log.h"
#include <QApplication>
#include <QLockFile>
#include <QMessageBox>
#include <QThread>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

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
    }

    // 工具注册表 + Agent 循环
    ToolRegistry tools;
    FileTools::registerAll(tools);
    ChatClient chat;
    AgentLoop loop({&chat, &tools, &providers});

    MainWindow win(&providers, &loop);
    win.show();
    const int rc = app.exec();

    if (auto lg = logutil::logger())
        lg->info("Miderforge 退出，rc={}", rc);
    return rc;
}
