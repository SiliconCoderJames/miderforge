// 日志实现：spdlog 滚动文件 + qInstallMessageHandler 重定向
#include "util/Log.h"
#include <QLoggingCategory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace miderforge::logutil {

static std::shared_ptr<spdlog::logger> s_logger;

static void qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    if (!s_logger) {
        fputs(qFormatLogMessage(type, ctx, msg).toLocal8Bit().constData(), stderr);
        return;
    }
    const QString line = QStringLiteral("[%1:%2] %3")
                             .arg(ctx.file ? ctx.file : "qt")
                             .arg(ctx.line)
                             .arg(msg);
    switch (type) {
    case QtDebugMsg: s_logger->debug(line.toStdString()); break;
    case QtInfoMsg: s_logger->info(line.toStdString()); break;
    case QtWarningMsg: s_logger->warn(line.toStdString()); break;
    case QtCriticalMsg:
    case QtFatalMsg: s_logger->error(line.toStdString()); break;
    }
}

void init(const QString& logDir) {
    if (s_logger)
        return;
    const QString path = logDir + QStringLiteral("/miderforge.log");
    try {
        // 单文件 5MB，保留 3 份
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path.toLocal8Bit().toStdString(), 5 * 1024 * 1024, 3);
        s_logger = std::make_shared<spdlog::logger>("miderforge", sink);
        s_logger->set_level(spdlog::level::info);
        s_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(s_logger);
        static bool handlerInstalled = false;
        if (!handlerInstalled) {
            handlerInstalled = true;
            qInstallMessageHandler(qtMessageHandler);
        }
    } catch (const std::exception&) {
        // 日志初始化失败不致命：静默降级为无文件日志
    }
}

std::shared_ptr<spdlog::logger> logger() { return s_logger; }

} // namespace miderforge::logutil
