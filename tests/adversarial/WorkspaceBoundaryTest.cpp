// P0-1 对抗测试：工作区写边界 vs reparse point 越界（真实创建 junction / symlink）。
// 每个越界形态两层断言：① 写入口拒绝（权限门 Denied）② 目标文件在磁盘上不存在
//（canary 语义：若写真的发生了，目标路径必然出现文件）。
// 环境不支持（如无符号链接特权）时显式 FAIL，绝不静默跳过——对抗测试是资产。
// 另含纵深防御用例：绕过权限门直接调 write_file handler，落盘前真实路径复核必须独立拦截。
#include "core/AppContext.h"
#include "tools/FileTools.h"
#include "tools/PermissionGate.h"
#include "tools/ToolRegistry.h"
#include "util/PathReal.h"
#include <doctest/doctest.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using miderforge::AppContext;
using miderforge::PermissionGate;
using miderforge::PermissionMode;
using miderforge::ToolRegistry;

#ifdef Q_OS_WIN
namespace {

// junction（目录联接，无需特权）：mklink 是 cmd 内建命令，经 setNativeArguments 传原文，
// 避开 QProcess 的 Windows 引号重排与 cmd 的引号吞吃
bool makeJunction(const QString& link, const QString& target, QString* err) {
    QProcess p;
    p.setProgram(QStringLiteral("cmd.exe"));
    p.setNativeArguments(QStringLiteral("/c mklink /J \"%1\" \"%2\"")
                             .arg(QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)));
    p.start();
    if (!p.waitForStarted(5000) || !p.waitForFinished(20000)) {
        if (err) *err = QStringLiteral("mklink /J 进程未正常结束");
        return false;
    }
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput() + p.readAllStandardError());
    if (p.exitCode() != 0 || !QDir(link).exists()) {
        if (err)
            *err = QStringLiteral("mklink /J 失败（exit=%1）：%2").arg(p.exitCode()).arg(out.trimmed());
        return false;
    }
    return true;
}

// 目录符号链接：需要开发者模式或管理员特权；失败必须让用例显式 FAIL（不许静默跳过）
bool makeSymlinkDir(const QString& link, const QString& target, QString* err) {
    const std::wstring wLink = QDir::toNativeSeparators(link).toStdWString();
    const std::wstring wTarget = QDir::toNativeSeparators(target).toStdWString();
    if (!::CreateSymbolicLinkW(wLink.c_str(), wTarget.c_str(),
                               SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
        && ::GetLastError() == ERROR_INVALID_PARAMETER
        && !::CreateSymbolicLinkW(wLink.c_str(), wTarget.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        if (err)
            *err = QStringLiteral("CreateSymbolicLinkW(目录) 失败 GetLastOSError=%1"
                                  "（目录符号链接需要开发者模式或管理员特权）").arg(::GetLastError());
        return false;
    }
    return true;
}

// 文件符号链接（可悬空：目标故意不存在——写入会跟随链接把文件创建到目标处，正是要封的通道）
bool makeSymlinkFile(const QString& link, const QString& target, QString* err) {
    const std::wstring wLink = QDir::toNativeSeparators(link).toStdWString();
    const std::wstring wTarget = QDir::toNativeSeparators(target).toStdWString();
    if (!::CreateSymbolicLinkW(wLink.c_str(), wTarget.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
        && ::GetLastError() == ERROR_INVALID_PARAMETER
        && !::CreateSymbolicLinkW(wLink.c_str(), wTarget.c_str(), 0)) {
        if (err)
            *err = QStringLiteral("CreateSymbolicLinkW(文件) 失败 GetLastOSError=%1"
                                  "（符号链接需要开发者模式或管理员特权）").arg(::GetLastError());
        return false;
    }
    return true;
}

} // namespace
#endif // Q_OS_WIN

namespace {

struct AdversarialEnv {
    QTemporaryDir tmp;
    QString ws;      // 真实工作区根
    QString outside; // 工作区外的真实目录（junction 目标与 canary 所在）
    AdversarialEnv() {
        ws = tmp.path() + QStringLiteral("/workspace");
        outside = tmp.path() + QStringLiteral("/outside");
        QDir().mkpath(ws);
        QDir().mkpath(outside);
    }
};

struct WriteAttempt {
    bool gateDenied = false;
    bool handlerOk = false;
    bool handlerRan = false;
    QString gateText;
    QString handlerText;
};

// 经真实运行时"写入口"尝试一次写：与 AgentLoop 同序——resolveWorkspacePath 归一 → 权限门判定；
// 放行则继续跑 write_file handler（与 ToolRegistry::execute 同一条执行路径）
WriteAttempt attemptWrite(const AdversarialEnv& env, const QString& rawPath, PermissionMode mode) {
    WriteAttempt r;
    const QString path = miderforge::FileTools::resolveWorkspacePath(rawPath, env.ws);
    PermissionGate gate;
    const auto d = gate.evaluate(mode, PermissionGate::Kind::WriteFile, path, env.ws);
    r.gateText = d == PermissionGate::Decision::Allowed ? QStringLiteral("Allowed")
                 : d == PermissionGate::Decision::NeedsConfirm ? QStringLiteral("NeedsConfirm")
                                                               : QStringLiteral("Denied");
    r.gateDenied = d == PermissionGate::Decision::Denied;
    if (r.gateDenied)
        return r; // 入口已拒绝：不会执行任何落盘，目标文件必然不存在
    AppContext::instance().workspaceRoot = env.ws;
    ToolRegistry reg;
    miderforge::FileTools::registerAll(reg);
    const QString args = QString::fromUtf8(QJsonDocument(QJsonObject{
                                               {"path", path},
                                               {"content", QStringLiteral("canary-content")},
                                           }).toJson(QJsonDocument::Compact));
    const auto res = reg.execute(QStringLiteral("write_file"), args);
    r.handlerRan = true;
    r.handlerOk = res.ok;
    r.handlerText = res.text;
    return r;
}

// 越界写的统一断言：入口拒绝 + canary 不存在；若入口误放行，落盘前复核也必须拦下
void expectEscapeRejected(const AdversarialEnv& env, const QString& path,
                          const QString& canary, PermissionMode mode) {
    const WriteAttempt r = attemptWrite(env, path, mode);
    CHECK_MESSAGE(r.gateDenied,
                  QStringLiteral("写入口必须拒绝越界写：%1（判定=%2）").arg(path, r.gateText).toStdString());
    if (!r.gateDenied) {
        CHECK_MESSAGE(!r.handlerOk,
                      QStringLiteral("落盘前复核必须拒绝越界写：%1（%2）")
                          .arg(path, r.handlerText.left(200)).toStdString());
    }
    CHECK_MESSAGE(!QFile::exists(canary),
                  QStringLiteral("canary 被写到越界目标：%1").arg(canary).toStdString());
}

} // namespace

TEST_SUITE("adversarial.workspace_boundary")
{

TEST_CASE("junction 越界写在写入口被拒且 canary 不落盘") {
    AdversarialEnv env;
    REQUIRE(env.tmp.isValid());
    const QString victim = env.outside + QStringLiteral("/victim");
    REQUIRE(QDir().mkpath(victim));
    QString err;
    REQUIRE_MESSAGE(makeJunction(env.ws + QStringLiteral("/link"), victim, &err), err.toStdString());

    // 测试自证：junction 真实解析后必须落在 victim，否则本用例测的不是越界
    const QString realLink = miderforge::pathreal::resolveReal(env.ws + QStringLiteral("/link"));
    MESSAGE("junction 真实路径解析：", realLink.toStdString());
    REQUIRE_MESSAGE(realLink.compare(miderforge::pathreal::resolveReal(victim), Qt::CaseInsensitive) == 0,
                    "junction 未按 reparse point 解析，测试基座失效");

    expectEscapeRejected(env, env.ws + QStringLiteral("/link/evil.txt"),
                         victim + QStringLiteral("/evil.txt"), PermissionMode::AutoEdit);
    expectEscapeRejected(env, env.ws + QStringLiteral("/link/evil.txt"),
                         victim + QStringLiteral("/evil.txt"), PermissionMode::FullAccess);
    // 大小写混淆引用 junction（link ↔ LINK）依然解析到真实目标
    expectEscapeRejected(env, env.ws + QStringLiteral("/LINK/Evil.txt"),
                         victim + QStringLiteral("/Evil.txt"), PermissionMode::FullAccess);
    // 相对路径写法（运行时 Agent 会用相对路径）同样被封
    expectEscapeRejected(env, QStringLiteral("link/rel.txt"),
                         victim + QStringLiteral("/rel.txt"), PermissionMode::FullAccess);
}

TEST_CASE("junction + 多层 ..：词法折叠伪装的越界被拒") {
    AdversarialEnv env;
    REQUIRE(env.tmp.isValid());
    const QString deep = env.outside + QStringLiteral("/deep");
    REQUIRE(QDir().mkpath(deep));
    QString err;
    REQUIRE_MESSAGE(makeJunction(env.ws + QStringLiteral("/j"), deep, &err), err.toStdString());
    // ws/j/../evil.txt 词法折叠成 ws/evil.txt（区内）；真实路径是 outside/deep/../evil.txt → 区外
    expectEscapeRejected(env, env.ws + QStringLiteral("/j/../evil.txt"),
                         env.outside + QStringLiteral("/evil.txt"), PermissionMode::FullAccess);

    // 链式 junction：ws/jc → chain-a，chain-a/jc2 → chain-b
    const QString victimA = env.outside + QStringLiteral("/chain-a");
    const QString victimB = env.outside + QStringLiteral("/chain-b");
    REQUIRE(QDir().mkpath(victimA));
    REQUIRE(QDir().mkpath(victimB));
    REQUIRE_MESSAGE(makeJunction(env.ws + QStringLiteral("/jc"), victimA, &err), err.toStdString());
    REQUIRE_MESSAGE(makeJunction(victimA + QStringLiteral("/jc2"), victimB, &err), err.toStdString());
    expectEscapeRejected(env, env.ws + QStringLiteral("/jc/jc2/evil.txt"),
                         victimB + QStringLiteral("/evil.txt"), PermissionMode::FullAccess);
}

TEST_CASE("多层 .. 上穿工作区根（无 junction 的词法形式）") {
    AdversarialEnv env;
    REQUIRE(env.tmp.isValid());
    REQUIRE(QDir().mkpath(env.ws + QStringLiteral("/a/b")));
    expectEscapeRejected(env, env.ws + QStringLiteral("/a/b/../../../above.txt"),
                         env.tmp.path() + QStringLiteral("/above.txt"), PermissionMode::AutoEdit);
    // 混合分隔符 + 冗余段：真实语义不变
    expectEscapeRejected(env, env.ws + QStringLiteral("/a\\b\\../../..\\above2.txt"),
                         env.tmp.path() + QStringLiteral("/above2.txt"), PermissionMode::AutoEdit);
}

TEST_CASE("\\?\\ 与 \\?\\UNC 设备路径形式越界被拒") {
    AdversarialEnv env;
    REQUIRE(env.tmp.isValid());
    // 字面设备路径直指工作区外（C:\Windows）
    expectEscapeRejected(env, QStringLiteral("\\\\?\\C:\\Windows\\evil.txt"),
                         QStringLiteral("C:/Windows/evil.txt"), PermissionMode::FullAccess);
    // UNC 设备路径（127.0.0.1 管理共享；可达与否都 fail-closed：解析失败同样按越界）
    expectEscapeRejected(env, QStringLiteral("\\\\?\\UNC\\127.0.0.1\\C$\\Users\\Public\\mider_p0_canary.txt"),
                         QStringLiteral("C:/Users/Public/mider_p0_canary.txt"), PermissionMode::FullAccess);
}

TEST_CASE("symlink 越界写被拒（无特权 → 显式失败不跳过）") {
    AdversarialEnv env;
    REQUIRE(env.tmp.isValid());
    const QString victim = env.outside + QStringLiteral("/symvictim");
    REQUIRE(QDir().mkpath(victim));
    QString err;
    // 目录符号链接：创建失败（无特权）即 FAIL——对抗测试不许静默跳过
    REQUIRE_MESSAGE(makeSymlinkDir(env.ws + QStringLiteral("/slink"), victim, &err), err.toStdString());
    expectEscapeRejected(env, env.ws + QStringLiteral("/slink/evil.txt"),
                         victim + QStringLiteral("/evil.txt"), PermissionMode::FullAccess);

    // 悬空文件符号链接：写入会跟随链接在目标处创建文件——正是要封死的通道
    const QString danglingTarget = env.outside + QStringLiteral("/dangling/evil.txt");
    REQUIRE_MESSAGE(makeSymlinkFile(env.ws + QStringLiteral("/dangle.txt"), danglingTarget, &err),
                    err.toStdString());
    expectEscapeRejected(env, env.ws + QStringLiteral("/dangle.txt"), danglingTarget,
                         PermissionMode::AutoEdit);
}

TEST_CASE("纵深防御：绕过权限门直调 write_file handler 仍被真实路径复核拦下") {
    AdversarialEnv env;
    REQUIRE(env.tmp.isValid());
    const QString victim = env.outside + QStringLiteral("/guardvictim");
    REQUIRE(QDir().mkpath(victim));
    QString err;
    REQUIRE_MESSAGE(makeJunction(env.ws + QStringLiteral("/glink"), victim, &err), err.toStdString());
    AppContext::instance().workspaceRoot = env.ws;
    ToolRegistry reg;
    miderforge::FileTools::registerAll(reg);
    const QString args = QString::fromUtf8(QJsonDocument(QJsonObject{
                                               {"path", env.ws + QStringLiteral("/glink/evil.txt")},
                                               {"content", QStringLiteral("canary-content")},
                                           }).toJson(QJsonDocument::Compact));
    const auto res = reg.execute(QStringLiteral("write_file"), args);
    CHECK_MESSAGE(!res.ok,
                  QStringLiteral("handler 层复核未拦截越界写：%1").arg(res.text.left(200)).toStdString());
    CHECK_FALSE(QFile::exists(victim + QStringLiteral("/evil.txt")));
}

TEST_CASE("反过度封锁：工作区内正常写保持放行") {
    AdversarialEnv env;
    REQUIRE(env.tmp.isValid());
    AppContext::instance().workspaceRoot = env.ws;
    PermissionGate gate;

    // 区内普通写
    CHECK(gate.evaluate(PermissionMode::AutoEdit, PermissionGate::Kind::WriteFile,
                        env.ws + QStringLiteral("/ok.txt"), env.ws)
          == PermissionGate::Decision::Allowed);
    // 深层新目录（组件尚不存在：靠词法尾段拼接，不得误拒）
    CHECK(gate.evaluate(PermissionMode::AutoEdit, PermissionGate::Kind::WriteFile,
                        env.ws + QStringLiteral("/new1/new2/f.txt"), env.ws)
          == PermissionGate::Decision::Allowed);
    // 区内经由 .. 的路径（真实父回退后仍在区内）
    REQUIRE(QDir().mkpath(env.ws + QStringLiteral("/sub")));
    CHECK(gate.evaluate(PermissionMode::AutoEdit, PermissionGate::Kind::WriteFile,
                        env.ws + QStringLiteral("/sub/../ok2.txt"), env.ws)
          == PermissionGate::Decision::Allowed);
    // 工作区根大小写混淆的区内写（根按解析后真实路径归一）
    CHECK(gate.evaluate(PermissionMode::AutoEdit, PermissionGate::Kind::WriteFile,
                        env.ws + QStringLiteral("/case.txt"), env.ws.toUpper())
          == PermissionGate::Decision::Allowed);

    // 真落盘验证（经 handler）
    ToolRegistry reg;
    miderforge::FileTools::registerAll(reg);
    const QString args = QString::fromUtf8(QJsonDocument(QJsonObject{
                                               {"path", env.ws + QStringLiteral("/new1/new2/f.txt")},
                                               {"content", QStringLiteral("hello")},
                                           }).toJson(QJsonDocument::Compact));
    const auto res = reg.execute(QStringLiteral("write_file"), args);
    CHECK_MESSAGE(res.ok, res.text.left(200).toStdString());
    CHECK(QFile::exists(env.ws + QStringLiteral("/new1/new2/f.txt")));
}

} // TEST_SUITE
