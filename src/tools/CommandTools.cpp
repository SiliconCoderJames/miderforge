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

// 子进程环境白名单：绝不继承父进程全环境。
// 保留原则：只保留编译工具链【功能性依赖】的变量（PATH/系统根/临时目录/MSVC/Qt/SDK 路径）。
// 身份信息披露最小化：USERNAME/HOMEDRIVE/HOMEPATH/PROGRAMDATA 已剔除——
// 恶意构造的构建脚本可借它们上报用户名与目录结构（API Key 本就走 HTTP 头，不在此通道）。
// USERPROFILE/APPDATA/LOCALAPPDATA 保留：git 全局配置与部分 MSVC 工具的硬依赖，剔除会破坏 git/cmake 工作流
QProcessEnvironment sanitizedEnvironment() {
    static const QStringList kKeep = {
        QStringLiteral("PATH"),        QStringLiteral("SYSTEMROOT"),  QStringLiteral("SYSTEMDRIVE"),
        QStringLiteral("TEMP"),        QStringLiteral("TMP"),         QStringLiteral("COMSPEC"),
        QStringLiteral("PATHEXT"),     QStringLiteral("USERPROFILE"),
        QStringLiteral("APPDATA"),     QStringLiteral("LOCALAPPDATA"), QStringLiteral("PROGRAMFILES"),
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
    def.handler = [&reg](const QJsonObject& args, QString* err) -> QString {
        // M5 P3：取消令牌入口快检——用户已取消则不启动子进程
        if (reg.toolCancelRequested()) {
            if (err) *err = QStringLiteral("已被用户取消");
            return envelope(QJsonObject{{"ok", false}, {"cancelled", true}});
        }
        const QString command = args.value("command").toString().trimmed();
        int timeoutSec = int(args.value("timeout_sec").toInt(120));
        timeoutSec = qBound(1, timeoutSec, 600);
        if (command.isEmpty()) {
            if (err) *err = QStringLiteral("command 参数为空");
            return {};
        }
        // 纵深防御：白名单首词二次校验（权限门已查一次）
        const QStringList parts = QProcess::splitCommand(command);
        if (parts.isEmpty()) {
            if (err) *err = QStringLiteral("command 参数为空");
            return {};
        }
        const QString first = parts.first().toLower();
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
        proc.start(parts.first(), parts.mid(1)); // 首词为程序，其余为参数（不做 shell 解析）
        if (!proc.waitForStarted(5000)) {
            if (err) *err = QStringLiteral("进程启动失败：%1").arg(proc.errorString());
            return {};
        }
#ifdef Q_OS_WIN
        jobGuard.assign(proc.processId()); // 启动后立即入 Job
#endif

        // 嵌套事件循环等待结束：保持 UI 消息泵（权限卡片/流式渲染）可响应。
        // ExcludeUserInputEvents：抑制用户输入事件重入（嵌套 QEventLoop 的已知反模式风险），
        // socket/定时器事件仍分发以维持流式渲染与超时计时
        QElapsedTimer clock;
        clock.start();
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool cancelledByUser = false;
        QObject::connect(&proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                         &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
            proc.kill();
            loop.quit();
        });
        QObject::connect(&proc, &QProcess::errorOccurred, &loop, [&] { loop.quit(); });
        // M5 P3：取消令牌轮询（150ms）——工具执行中用户取消原来到不了这里，现在直达 kill
        QTimer cancelPoll;
        cancelPoll.setInterval(150);
        QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&] {
            if (reg.toolCancelRequested()) {
                cancelledByUser = true;
                proc.kill();
                loop.quit();
            }
        });
        timeout.start(timeoutSec * 1000);
        cancelPoll.start();
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        cancelPoll.stop();

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
        const bool ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0
                        && !timedOut && !cancelledByUser;
        // 同时用 *err 上报失败：ToolRegistry 以 err.isEmpty() 判定 ExecResult::ok，
        // 而本工具把成败只写在 envelope 里 → 编译失败会被当成"工具调用成功"，
        // 于是同一个编译错误反复重试也永远不触发熔断与升档，卡片还显示 ✓。
        // 保留完整 envelope 作为文本（模型需要 exit_code/output）。
        if (!ok && err) {
            *err = timedOut                       ? QStringLiteral("命令超时（%1s）").arg(timeoutSec)
                   : cancelledByUser              ? QStringLiteral("命令被用户取消")
                   : proc.exitStatus() != QProcess::NormalExit
                       ? QStringLiteral("命令异常终止（崩溃或被 Job 沙箱终止）")
                       : QStringLiteral("命令退出码 %1").arg(proc.exitCode());
        }

        return envelope(QJsonObject{
            {"ok", ok},
            {"exit_code", proc.exitCode()},
            {"timed_out", timedOut},
            {"cancelled", cancelledByUser},
            {"duration_ms", qint64(clock.elapsed())},
            {"output", text},
        });
    };
    reg.add(std::move(def));
}

} // namespace miderforge::CommandTools
