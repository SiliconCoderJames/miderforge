// 命令类工具实现：沙箱第 2/3 层——QProcess 环境剥离 + Job Object（退出连带终止/内存上限/超时）
#include "tools/CommandTools.h"
#include "core/AppContext.h"
#include "tools/PermissionGate.h"
#include "tools/ToolRegistry.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace miderforge::CommandTools {

namespace {

QString envelope(const QJsonObject& obj) {
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 显式白名单构造子进程环境：绝不继承父进程全环境（防泄漏 API Key，踩坑清单 #10）
QProcessEnvironment sanitizedEnvironment() {
    static const QStringList kKeep = {
        QStringLiteral("PATH"),        QStringLiteral("SYSTEMROOT"),  QStringLiteral("SYSTEMDRIVE"),
        QStringLiteral("TEMP"),        QStringLiteral("TMP"),         QStringLiteral("COMSPEC"),
        QStringLiteral("PATHEXT"),     QStringLiteral("USERNAME"),    QStringLiteral("USERPROFILE"),
        QStringLiteral("HOMEDRIVE"),   QStringLiteral("HOMEPATH"),    QStringLiteral("APPDATA"),
        QStringLiteral("LOCALAPPDATA"), QStringLiteral("PROGRAMDATA"), QStringLiteral("PROGRAMFILES"),
        // 编译环境（MSVC/Qt 开发者命令行下的常见变量；存在才透传）
        QStringLiteral("INCLUDE"),     QStringLiteral("LIB"),         QStringLiteral("LIBPATH"),
        QStringLiteral("VCINSTALLDIR"), QStringLiteral("VSINSTALLDIR"), QStringLiteral("WindowsSdkDir"),
        QStringLiteral("QTDIR"),       QStringLiteral("CMAKE_PREFIX_PATH"),
    };
    QProcessEnvironment parent = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment sanitized;
    for (const QString& key : kKeep) {
        if (parent.contains(key))
            sanitized.insert(key, parent.value(key));
    }
    return sanitized;
}

#ifdef Q_OS_WIN
// Job Object 句柄 RAII：Miderforge 退出 → 全部子孙进程连带终止（踩坑清单 #11）
struct JobGuard {
    HANDLE job = nullptr;
    JobGuard() {
        job = CreateJobObjectW(nullptr, nullptr);
        if (!job)
            return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limit.ProcessMemoryLimit = 2ull * 1024 * 1024 * 1024; // 2GB 内存上限（规格默认）
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
    }
    ~JobGuard() {
        if (job)
            CloseHandle(job); // KILL_ON_JOB_CLOSE：连带终止全部子进程
    }
    bool assign(DWORD pid) const {
        if (!job)
            return false;
        HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);
        if (!proc)
            return false;
        const BOOL ok = AssignProcessToJobObject(job, proc);
        CloseHandle(proc);
        return ok != FALSE;
    }
};
#endif

} // namespace

void CommandTools::registerAll(ToolRegistry& reg) {
    ToolDef def;
    def.name = QStringLiteral("run_command");
    def.description = QStringLiteral("在工作区内执行白名单命令（cmake/ninja/msbuild/git/cl/clang-format 等），"
                                     "输出与退出码会回填；默认超时 120 秒");
    def.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{
                           {"command", QJsonObject{
                                           {"type", "string"},
                                           {"description", "要执行的命令全文"},
                                       }},
                           {"timeout_sec", QJsonObject{
                                               {"type", "integer"},
                                               {"description", "可选：超时秒数（默认 120，上限 600）"},
                                           }},
                       }},
        {"required", QJsonArray{"command"}},
    };
    def.handler = [](const QJsonObject& args, QString* err) -> QString {
        const QString command = args.value("command").toString().trimmed();
        int timeoutSec = int(args.value("timeout_sec").toInt(120));
        timeoutSec = qBound(1, timeoutSec, 600);
        if (command.isEmpty()) {
            if (err) *err = QStringLiteral("command 参数为空");
            return {};
        }
        // 纵深防御：白名单首词二次校验（权限门已查一次）
        const QString first = command.split(QLatin1Char(' ')).value(0).toLower();
        if (!PermissionGate::commandWhitelist().contains(first)) {
            if (err) *err = QStringLiteral("命令不在白名单内：%1").arg(first);
            return {};
        }

        QProcess proc;
        proc.setWorkingDirectory(AppContext::instance().workspaceRoot); // 工作目录钉死工作区
        proc.setProcessEnvironment(sanitizedEnvironment());             // 环境白名单

#ifdef Q_OS_WIN
        JobGuard jobGuard;
#endif
        proc.start(command, {}); // 决策: 整串交由 shell 解析？否——cmd.exe 攻击面大，直接执行首词
        if (!proc.waitForStarted(5000)) {
            if (err) *err = QStringLiteral("进程启动失败：%1").arg(proc.errorString());
            return {};
        }
#ifdef Q_OS_WIN
        jobGuard.assign(proc.processId()); // 启动后立即入 Job
#endif

        // 嵌套事件循环等待结束：保持 UI 消息泵（权限卡片/流式渲染）可响应
        QElapsedTimer clock;
        clock.start();
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                         &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
            proc.kill();
            loop.quit();
        });
        QObject::connect(&proc, &QProcess::errorOccurred, &loop, [&] { loop.quit(); });
        timeout.start(timeoutSec * 1000);
        loop.exec();

        const bool timedOut = clock.elapsed() >= timeoutSec * 1000 && proc.state() != QProcess::NotRunning;
        if (timedOut)
            proc.kill();
        proc.waitForFinished(3000);

        const QByteArray out = proc.readAllStandardOutput();
        const QByteArray errOut = proc.readAllStandardError();
        QByteArray merged = out;
        if (!errOut.isEmpty()) {
            if (!merged.isEmpty())
                merged += QByteArrayLiteral("\n[stderr]\n");
            merged += errOut;
        }
        if (merged.size() > 256 * 1024)
            merged = merged.left(256 * 1024); // 决策: 输出封顶 256KB
        // UTF-8 全链路；MSVC 工具链 GBK 输出场景由模型自行请求 chcp 适配（v1 从简）
        const QString text = QString::fromUtf8(merged);

        return envelope(QJsonObject{
            {"ok", proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0 && !timedOut},
            {"exit_code", proc.exitCode()},
            {"timed_out", timedOut},
            {"duration_ms", qint64(clock.elapsed())},
            {"output", text},
        });
    };
    reg.add(std::move(def));
}

} // namespace miderforge::CommandTools
