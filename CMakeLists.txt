# Miderforge 主构建脚本：vcpkg manifest 模式；Qt6 预编译套件（C:/Qt，见 CMakePresets.json）+ libcurl + nlohmann-json + spdlog + doctest
cmake_minimum_required(VERSION 3.24)
project(Miderforge VERSION 0.1.0 LANGUAGES C CXX)

# CTest：README/CI 的验收门槛（ctest --preset 必须全绿）依赖这里注册测试；
# 缺了它 ctest 会"0 个测试"空转、门槛形同虚设（巡检发现并修复）
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    # /utf-8：源码与执行字符集统一 UTF-8（全链路 UTF-8 硬约束的编译期部分）
    add_compile_options(/utf-8 /W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)

# 核心静态库：QtCore 级依赖（无 UI），可 headless 复用与测试；单测只链接它（真正脱离 GUI）
add_library(mider_core STATIC
    src/core/AppContext.h      src/core/AppContext.cpp
    src/core/AdjudicationService.h src/core/AdjudicationService.cpp
    src/core/AgentLoop.h       src/core/AgentLoop.cpp
    src/core/Breaker.h
    src/core/Router.h          src/core/Router.cpp
    src/core/EventBus.h        src/core/EventBus.cpp
    src/core/Scheduler.h       src/core/Scheduler.cpp
    src/llm/SseParser.h        src/llm/SseParser.cpp
    src/llm/HttpClient.h       src/llm/HttpClient.cpp
    src/llm/ToolCallAccumulator.h src/llm/ToolCallAccumulator.cpp
    src/llm/ChatClient.h       src/llm/ChatClient.cpp
    src/llm/ProviderManager.h  src/llm/ProviderManager.cpp
    src/db/Database.h          src/db/Database.cpp
    src/memory/MemoryManager.h src/memory/MemoryManager.cpp
    src/skills/SkillManager.h  src/skills/SkillManager.cpp
    src/notify/EmailNotifier.h src/notify/EmailNotifier.cpp
    src/tools/ToolRegistry.h   src/tools/ToolRegistry.cpp
    src/tools/FileTools.h      src/tools/FileTools.cpp
    src/tools/CommandTools.h   src/tools/CommandTools.cpp
    src/tools/ExtraTools.h     src/tools/ExtraTools.cpp
    src/tools/PermissionGate.h src/tools/PermissionGate.cpp
    src/util/AppDirs.h         src/util/AppDirs.cpp
    src/util/Dpapi.h           src/util/Dpapi.cpp
    src/util/JsonExtract.h     src/util/JsonExtract.cpp
    src/util/Log.h             src/util/Log.cpp
    src/util/Tokens.h          src/util/Tokens.cpp
)
target_include_directories(mider_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite-amalgamation-3530400)
# SQLite amalgamation（内置源码）：FTS5 trigram 全文检索；sqlite-vec v1 仅加载不使用（规格 2）
target_sources(mider_core PRIVATE
    third_party/sqlite/sqlite-amalgamation-3530400/sqlite3.c
    third_party/sqlite-vec/sqlite-vec.c
)
set_source_files_properties(third_party/sqlite/sqlite-amalgamation-3530400/sqlite3.c PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_ENABLE_FTS5;SQLITE_ENABLE_COLUMN_METADATA;SQLITE_ENABLE_MATH_FUNCTIONS"
)
set_source_files_properties(third_party/sqlite-vec/sqlite-vec.c PROPERTIES
    # SQLITE_CORE：静态编译直连 sqlite3 API（不经扩展装载机制）；SQLITE_VEC_STATIC：免 dllexport
    COMPILE_DEFINITIONS "SQLITE_CORE;SQLITE_VEC_STATIC"
)
target_link_libraries(mider_core PUBLIC
    Qt6::Core Qt6::Network CURL::libcurl nlohmann_json::nlohmann_json spdlog::spdlog)
if(WIN32)
    # DPAPI（CryptProtectData/CryptUnprotectData）
    target_link_libraries(mider_core PUBLIC crypt32)
endif()

# UI 静态库：Qt Widgets 界面层（深色主题、六大面板、向导）
add_library(mider_ui STATIC
    src/app/Theme.h            src/app/Theme.cpp
    src/app/ChatWidgets.h      src/app/ChatWidgets.cpp
    src/app/SessionView.h      src/app/SessionView.cpp
    src/app/AuditLogView.h     src/app/AuditLogView.cpp
    src/app/MemoryView.h       src/app/MemoryView.cpp
    src/app/SkillView.h        src/app/SkillView.cpp
    src/app/TaskQueueView.h    src/app/TaskQueueView.cpp
    src/app/MainWindow.h       src/app/MainWindow.cpp
    src/app/FirstRunWizard.h   src/app/FirstRunWizard.cpp
    src/app/ProviderPanel.h    src/app/ProviderPanel.cpp
    src/app/SettingsDialog.h   src/app/SettingsDialog.cpp
)
target_link_libraries(mider_ui PUBLIC mider_core Qt6::Widgets)

# 主程序
qt_add_executable(miderforge WIN32 src/main.cpp)
target_link_libraries(miderforge PRIVATE mider_ui)

# 运行时 DLL 随 exe 就位（Qt 等）：免"PATH 没挂 Qt bin 就 0xc0000135"，ctest 与直接启动都可用
if(WIN32)
    add_custom_command(TARGET miderforge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:miderforge> $<TARGET_FILE_DIR:miderforge>
        COMMAND_EXPAND_LISTS)
endif()

# 单元测试（doctest）：只依赖 mider_core，不拉 Widgets
add_executable(mider_tests
    tests/test_main.cpp
    tests/test_sse.cpp
    tests/test_toolargs.cpp
    tests/test_mockstream.cpp
    tests/test_permission.cpp
    tests/test_breaker.cpp
    tests/test_command.cpp
    tests/test_memory.cpp
    tests/test_skill.cpp
    tests/test_router.cpp
    tests/test_jsonextract.cpp
    tests/test_adjudication.cpp
)
target_link_libraries(mider_tests PRIVATE mider_core Qt6::Network doctest::doctest)
if(MSVC)
    # 崩溃栈回溯（tests/test_main.cpp 的 dbghelp 符号解析）
    target_link_libraries(mider_tests PRIVATE dbghelp)
endif()
# 注册进 CTest：整个 doctest 套件作为一个测试用例（失败即非零退出码），
# 超时 5 分钟兜底防挂起（mock 流式重试类用例最坏耗时数十秒）
add_test(NAME mider_unit_tests COMMAND mider_tests)
set_tests_properties(mider_unit_tests PROPERTIES TIMEOUT 300)
if(WIN32)
    # 测试 exe 的运行时 DLL（Qt 等）随构建就位，ctest 无需手工挂 PATH
    add_custom_command(TARGET mider_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:mider_tests> $<TARGET_FILE_DIR:mider_tests>
        COMMAND_EXPAND_LISTS)
endif()
