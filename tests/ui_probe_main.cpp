// UI 几何探针（开发者诊断工具，不参与发布）：
// 在无法目视界面的环境下，用离屏 QPA 构造**真实 MainWindow**，打印控件树、几何、可见性与
// 最小尺寸，把"布局问题"变成可读数字。用法：
//   build\Release\mider_ui_probe.exe -platform offscreen [宽 高]
// 安全：QStandardPaths 测试模式 + QTemporaryDir，绝不触碰用户真实数据目录。
// 输出统一走 say()（QString 拼接 + fputs），不用 printf：MSVC 在本目标把格式串告警当错误。
#include "app/MainWindow.h"
#include "app/PreviewPane.h"
#include "app/Theme.h"
#include "core/AgentLoop.h"
#include "core/AppContext.h"
#include "core/EventBus.h"
#include "db/Database.h"
#include "llm/ChatClient.h"
#include "llm/ProviderManager.h"
#include "memory/MemoryManager.h"
#include "notify/EmailNotifier.h"
#include "skills/SkillManager.h"
#include "tools/ExtraTools.h"
#include "tools/FileTools.h"
#include "tools/ToolRegistry.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QToolButton>
#include <cstdio>
#include <functional>

using miderforge::AgentLoop;
using miderforge::AppContext;
using miderforge::ChatClient;
using miderforge::Database;
using miderforge::EmailNotifier;
using miderforge::EventBus;
using miderforge::MainWindow;
using miderforge::MemoryManager;
using miderforge::ProviderManager;
using miderforge::SkillManager;
using miderforge::ToolRegistry;
namespace theme = miderforge::theme;

namespace {

// 同时写 stdout 与报告文件：本环境下 GUI 进程的 stdout 重定向不可靠，
// 报告文件每行 flush 落盘，是探针的权威输出通道
QFile* g_report = nullptr;

void say(const QString& s) {
    const QByteArray line = s.toUtf8() + "\n";
    std::fputs(line.constData(), stdout);
    std::fflush(stdout);
    if (g_report) {
        g_report->write(line);
        g_report->flush();
    }
}

const char* cls(QWidget* w) { return w->metaObject()->className(); }

QString rect(const QRect& r) {
    return QStringLiteral("x=%1 y=%2 w=%3 h=%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

void dumpTree(QWidget* w, int depth, int maxDepth) {
    if (!w || depth > maxDepth)
        return;
    QString extra;
    if (auto* lw = qobject_cast<QListWidget*>(w))
        extra = QStringLiteral("  items=%1").arg(lw->count());
    if (auto* sw = qobject_cast<QStackedWidget*>(w))
        extra = QStringLiteral("  pages=%1 current=%2").arg(sw->count()).arg(sw->currentIndex());
    if (auto* b = qobject_cast<QPushButton*>(w))
        extra = QStringLiteral("  text=\"%1\"").arg(b->text());
    if (auto* l = qobject_cast<QLabel*>(w)) {
        QString t = l->text();
        t.replace(QLatin1Char('\n'), QLatin1Char('/'));
        extra = QStringLiteral("  text=\"%1\"").arg(t.left(24));
    }
    say(QStringLiteral("%1%2  %3  vis=%4  %5  minH=%6%7")
            .arg(QString(depth * 2, QLatin1Char(' ')))
            .arg(QString::fromLatin1(cls(w)), -20)
            .arg(w->objectName().isEmpty() ? QStringLiteral("-") : w->objectName(), -26)
            .arg(w->isVisible() ? 1 : 0)
            .arg(rect(w->geometry()))
            .arg(w->minimumSizeHint().height())
            .arg(extra));
    for (QObject* c : w->children())
        if (auto* cw = qobject_cast<QWidget*>(c))
            dumpTree(cw, depth + 1, maxDepth);
}

template <typename T>
T* findByText(QWidget* root, const std::function<bool(T*)>& pred) {
    if (auto* t = qobject_cast<T*>(root); t && pred(t))
        return t;
    for (QObject* c : root->children())
        if (auto* cw = qobject_cast<QWidget*>(c))
            if (auto* hit = findByText<T>(cw, pred))
                return hit;
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    QFile report(QStringLiteral("ui_probe_report.txt"));
    if (report.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_report = &report;
    say(QStringLiteral("[0] 进程启动，准备构造 QApplication"));
    QApplication app(argc, argv);
    say(QStringLiteral("[0b] QApplication 构造完成"));
    QApplication::setOrganizationName(QStringLiteral("Miderforge"));
    QApplication::setApplicationName(QStringLiteral("Miderforge"));
    QStandardPaths::setTestModeEnabled(true); // 隔离：只用 qttest 目录
    theme::apply(app);

    int w = 1440;
    int h = 900;
    if (argc >= 3) {
        w = QString::fromUtf8(argv[1]).toInt();
        h = QString::fromUtf8(argv[2]).toInt();
    }

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        say(QStringLiteral("临时目录创建失败"));
        return 2;
    }
    AppContext::instance().workspaceRoot = tmp.path();

    // 预置可查看文件：一个 Markdown、一个带脚本的 HTML（查看器必须净化脚本）
    {
        QFile md(tmp.path() + QStringLiteral("/说明.md"));
        if (md.open(QIODevice::WriteOnly))
            md.write(QStringLiteral("# 查看器标题\n\n- 第一条\n\n```cpp\nint a=1;\n```").toUtf8());
        QFile html(tmp.path() + QStringLiteral("/page.html"));
        if (html.open(QIODevice::WriteOnly))
            html.write(QStringLiteral("<h1>HTML 标题</h1><script>alert(1)</script><p>正文段</p>").toUtf8());
    }
    say(QStringLiteral("[1] QApplication 就绪"));
    Database db;
    if (!db.open(tmp.path() + QStringLiteral("/probe.db"))) {
        say(QStringLiteral("数据库打开失败"));
        return 2;
    }
    say(QStringLiteral("[2] 数据库就绪"));
    // 预置会话：1 条活动 + 1 条已归档——用来端到端验证左栏列表的归档过滤接线
    db.execute(QStringLiteral("INSERT INTO sessions(title,created_at,updated_at) "
                              "VALUES('探针·活动会话',1,1000)"),
               {});
    db.execute(QStringLiteral("INSERT INTO sessions(title,created_at,updated_at,archived_at) "
                              "VALUES('探针·已归档会话',1,2000,2000)"),
               {});
    EventBus bus(tmp.path() + QStringLiteral("/events.jsonl"));
    bus.setDatabase(&db);
    MemoryManager mem(&db, tmp.path() + QStringLiteral("/core.md"));
    say(QStringLiteral("[3] 记忆管理器就绪"));
    SkillManager skills(&db, tmp.path() + QStringLiteral("/skills"));
    say(QStringLiteral("[4] 技能管理器就绪"));
    ProviderManager providers;
    providers.initializeFromTemplate();
    say(QStringLiteral("[5] 供应商模板就绪"));
    ToolRegistry tools;
    miderforge::FileTools::registerAll(tools);
    miderforge::ExtraTools::registerAll(tools, &db, nullptr);
    say(QStringLiteral("[6] 工具注册就绪"));
    ChatClient chat;
    EmailNotifier mail(&bus);
    AgentLoop loop({&chat, &tools, &providers, &bus, &db, &mem, &skills});
    say(QStringLiteral("[7] AgentLoop 就绪 → 构造 MainWindow"));
    MainWindow win(&providers, &loop, &bus, &db, &mem, &skills, &mail);
    say(QStringLiteral("[8] MainWindow 构造完成"));

    win.resize(w, h);
    win.show();
    say(QStringLiteral("[9] show() 返回"));
    app.processEvents();
    say(QStringLiteral("[10] processEvents 返回"));

    if (auto* scr = QGuiApplication::primaryScreen()) {
        const QRect av = scr->availableGeometry();
        say(QStringLiteral("[屏幕] 可用区 %1  DPR=%2").arg(rect(av)).arg(scr->devicePixelRatio()));
        say(QStringLiteral("[窗口] %1  最小尺寸提示 w=%2 h=%3  （最小宽超屏：%4）")
                .arg(rect(win.geometry()))
                .arg(win.minimumSizeHint().width())
                .arg(win.minimumSizeHint().height())
                .arg(win.minimumSizeHint().width() > av.width() ? QStringLiteral("是 ← 会被推出屏幕")
                                                               : QStringLiteral("否")));
    }
    // ---- ⑥ 内置查看器：打开 md / html，断言富文本真的渲染出来 ----
    if (auto* preview = findByText<QWidget>(&win, [](QWidget* w) {
            return QString::fromLatin1(w->metaObject()->className())
                .contains(QStringLiteral("PreviewPane"));
        })) {
        auto* pane = static_cast<miderforge::PreviewPane*>(preview);
        say(QStringLiteral("\n===== ⑥ 内置查看器 ====="));
        pane->setWorkspaceRoot(AppContext::instance().workspaceRoot);
        const auto lists = preview->findChildren<QListWidget*>();
        say(QStringLiteral("  工作区可查看文件数（应有 2）: %1").arg(lists.isEmpty() ? -1 : lists.first()->count()));
        auto* view = preview->findChild<QTextBrowser*>();
        const bool mdOk = pane->openFile(tmp.path() + QStringLiteral("/说明.md"));
        const QString mdText = view ? view->toPlainText() : QString();
        say(QStringLiteral("  打开 Markdown: ok=%1 | 可见标题=%2 | 含列表项=%3 | 含代码=%4")
                .arg(mdOk ? 1 : 0)
                .arg(mdText.contains(QStringLiteral("查看器标题")) ? 1 : 0)
                .arg(mdText.contains(QStringLiteral("第一条")) ? 1 : 0)
                .arg(mdText.contains(QStringLiteral("int a=1;")) ? 1 : 0));
        const bool htmlOk = pane->openFile(tmp.path() + QStringLiteral("/page.html"));
        const QString htmlText = view ? view->toPlainText() : QString();
        say(QStringLiteral("  打开 HTML: ok=%1 | 可见正文=%2 | 脚本未渲染=%3")
                .arg(htmlOk ? 1 : 0)
                .arg(htmlText.contains(QStringLiteral("HTML 标题")) ? 1 : 0)
                .arg(htmlText.contains(QStringLiteral("alert(1)")) ? 0 : 1));
    } else {
        say(QStringLiteral("\n[!] 未找到 PreviewPane"));
    }

    say(QStringLiteral("[字号] UI 基准 %1pt  等宽 %2pt")
            .arg(theme::uiFont().pointSize())
            .arg(theme::monoFont().pointSize()));

    say(QStringLiteral("\n===== ① 初始（会话页）控件树 ====="));
    dumpTree(&win, 0, 3);

    // 会话归档接线端到端：默认隐藏归档项，点「📦 归档」后应出现
    {
        // 必须定向到左栏自己的列表：findChild 会先命中技能页的 QListWidget
        auto* sidebar = findByText<QWidget>(&win, [](QWidget* w) {
            return QString::fromLatin1(w->metaObject()->className())
                .contains(QStringLiteral("SidebarView"));
        });
        auto* list = sidebar ? sidebar->findChild<QListWidget*>() : nullptr;
        auto* toggle = findByText<QToolButton>(&win, [](QToolButton* b) {
            return b->text().contains(QStringLiteral("归档"));
        });
        say(QStringLiteral("\n===== ⑤ 会话归档接线 ====="));
        say(QStringLiteral("  左栏列表项数（默认，应=1）: %1").arg(list ? list->count() : -1));
        if (toggle) {
            toggle->setChecked(true);
            app.processEvents();
            say(QStringLiteral("  点「%1」后项数（应=2）: %2")
                    .arg(toggle->text())
                    .arg(list ? list->count() : -1));
            toggle->setChecked(false);
            app.processEvents();
            say(QStringLiteral("  取消开关后项数（应=1）: %1").arg(list ? list->count() : -1));
        } else {
            say(QStringLiteral("  [!] 未找到归档开关"));
        }
    }

    auto* settingsBtn = findByText<QPushButton>(&win, [](QPushButton* b) {
        return b->text().contains(QStringLiteral("设置"));
    });
    if (!settingsBtn) {
        say(QStringLiteral("[!] 未找到设置按钮"));
        return 3;
    }
    say(QStringLiteral("\n[操作] 点击 \"%1\"").arg(settingsBtn->text()));
    settingsBtn->click();
    app.processEvents();

    say(QStringLiteral("\n===== ② 点击设置后控件树 ====="));
    dumpTree(&win, 0, 2);

    // 设置页内部：逐页最小宽度（找出是谁在要求超宽、把窗口撑出屏幕）
    auto* settings = findByText<QWidget>(&win, [](QWidget* w) {
        return QString::fromLatin1(w->metaObject()->className()).contains(QStringLiteral("Settings"));
    });
    if (settings) {
        say(QStringLiteral("\n===== ③ 设置页内部分页最小宽度（谁在要宽度）====="));
        for (auto* sw : settings->findChildren<QStackedWidget*>()) {
            for (int i = 0; i < sw->count(); ++i) {
                QWidget* page = sw->widget(i);
                say(QStringLiteral("  页 %1: %2  minW=%3 minH=%4")
                        .arg(i)
                        .arg(QString::fromLatin1(cls(page)), -34)
                        .arg(page->minimumSizeHint().width())
                        .arg(page->minimumSizeHint().height()));
            }
        }
        // 长文本 QLabel 是不换行时"最小宽度=整行文本宽"的常见元凶
        say(QStringLiteral("\n===== ④ 未换行长标签（宽度元凶候选）====="));
        for (auto* l : settings->findChildren<QLabel*>()) {
            if (l->wordWrap() || l->text().isEmpty())
                continue;
            const int need = l->minimumSizeHint().width();
            if (need >= 420)
                say(QStringLiteral("  minW=%1  \"%2\"")
                        .arg(need)
                        .arg(l->text().left(58).replace(QLatin1Char('\n'), QLatin1Char('/'))));
        }
    }

    QWidget* cw = win.centralWidget();
    say(QStringLiteral("\n[判定] 中央区 %1").arg(rect(cw->geometry())));
    int idx = 0;
    for (QObject* c : cw->children()) {
        if (auto* child = qobject_cast<QWidget*>(c))
            say(QStringLiteral("   直接子控件 %1: %2 vis=%3 %4 minW=%5")
                    .arg(idx++)
                    .arg(QString::fromLatin1(cls(child)), -18)
                    .arg(child->isVisible() ? 1 : 0)
                    .arg(rect(child->geometry()))
                    .arg(child->minimumSizeHint().width()));
    }
    return 0;
}
