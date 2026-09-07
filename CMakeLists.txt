# Miderforge 主构建脚本：vcpkg manifest 模式；Qt6 预编译套件（C:/Qt，见 CMakePresets.json）+ libcurl + nlohmann-json + spdlog + doctest
cmake_minimum_required(VERSION 3.24)
project(Miderforge VERSION 0.1.0 LANGUAGES C CXX)

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

# 核心静态库：业务逻辑与 UI 可复用；单测直接链接它（单测可脱离 GUI 运行）
add_library(mider_core STATIC
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
    src/core/AppContext.h      src/core/AppContext.cpp
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
    Qt6::Widgets CURL::libcurl nlohmann_json::nlohmann_json spdlog::spdlog)
if(WIN32)
    # DPAPI（CryptProtectData/CryptUnprotectData）
    target_link_libraries(mider_core PUBLIC crypt32)
endif()

# 主程序
qt_add_executable(miderforge WIN32 src/main.cpp)
target_link_libraries(miderforge PRIVATE mider_core)

# 单元测试（doctest）
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
)
target_link_libraries(mider_tests PRIVATE mider_core Qt6::Network doctest::doctest)
if(MSVC)
    # 崩溃栈回溯（tests/test_main.cpp 的 dbghelp 符号解析）
    target_link_libraries(mider_tests PRIVATE dbghelp)
endif()
