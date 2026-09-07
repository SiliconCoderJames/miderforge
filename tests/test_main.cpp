// doctest 主入口：QCoreApplication（支撑带事件循环的流式测试）+ 工作线程崩溃栈回溯
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <QCoreApplication>
#include <cstdio>
#include <windows.h>
#include <dbghelp.h>

namespace {

LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    fprintf(stderr, "\n*** 未处理异常 0x%08lX @ %p，线程栈：\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    void* frames[62];
    const WORD n = CaptureStackBackTrace(0, 62, frames, nullptr);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    for (WORD i = 0; i < n; ++i) {
        char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 off = 0;
        if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(frames[i]), &off, sym))
            fprintf(stderr, "  #%02u %s +0x%llx\n", i, sym->Name, (unsigned long long)off);
        else
            fprintf(stderr, "  #%02u %p\n", i, frames[i]);
    }
    fflush(stderr);
    _exit(3);
}

struct CrashHook {
    CrashHook() { SetUnhandledExceptionFilter(crashHandler); }
} g_crashHook;

} // namespace

// 供测试用例在运行期重装过滤器（覆盖 doctest::Context::run 装的那个）
void installCrashHandler() { SetUnhandledExceptionFilter(crashHandler); }

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    const int rc = ctx.run();
    return rc;
}
