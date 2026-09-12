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
    src/llm/EmbeddingClient.h  src/llm/EmbeddingClient.cpp
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
    src/util/DiffUtil.h        src/util/DiffUtil.cpp
    src/util/Dpapi.h           src/util/Dpapi.cpp
    src/util/JsonExtract.h     src/util/JsonExtract.cpp
    src/util/Log.h             src/util/Log.cpp
    src/util/NetGuard.h        src/util/NetGuard.cpp
    src/util/PathReal.h        src/util/PathReal.cpp
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
    src/app/CommandPalette.h   src/app/CommandPalette.cpp
    src/app/ChatWidgets.h      src/app/ChatWidgets.cpp
    src/app/SessionView.h      src/app/SessionView.cpp
    src/app/SidebarView.h      src/app/SidebarView.cpp
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
target_sources(miderforge PRIVATE resources/assets/app.rc) # exe 外壳图标（资源管理器/任务栏/快捷方式）
qt_add_resources(miderforge "appicons" PREFIX "/" FILES resources/assets/miderforge-256.png) # 窗口图标
target_link_libraries(miderforge PRIVATE mider_ui)

# 运行时 Qt 完整部署（含 platforms/qwindows.dll 插件）：TARGET_RUNTIME_DLLS 只拷核心 DLL，
# 缺平台插件会让 exe 脱离开发环境（无 Qt PATH）后启动即报 "no Qt platform plugin"
# ——桌面快捷方式启动触发的实机 bug，windeployqt 按 exe 依赖自动部署全部运行时
if(WIN32)
    get_target_property(_qmake_exe Qt6::qmake LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    add_custom_command(TARGET miderforge POST_BUILD
        COMMAND "${_qt_bin_dir}/windeployqt.exe"
                $<IF:$<CONFIG:Debug>,--debug,--release>
                --no-translations --no-system-d3d-compiler --no-opengl-sw --no-quick-import
                $<TARGET_FILE:miderforge>
        COMMENT "windeployqt: 部署 Qt DLL 与平台插件到输出目录"
        VERBATIM)
endif()

# UI 几何探针（开发者诊断工具，不参与发布）：离屏构造真实 MainWindow 并打印
# 控件树/几何/可见性/最小尺寸，让"看不到界面"时的布局问题变成可读数字
add_executable(mider_ui_probe tests/ui_probe_main.cpp)
target_link_libraries(mider_ui_probe PRIVATE mider_ui)
if(WIN32)
    add_custom_command(TARGET mider_ui_probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:mider_ui_probe> $<TARGET_FILE_DIR:mider_ui_probe>
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
    tests/test_embedding.cpp
    tests/test_theme.cpp
    tests/test_chatwidgets.cpp
    tests/test_commandpalette.cpp
    tests/test_scheduler.cpp
)
target_link_libraries(mider_tests PRIVATE mider_core Qt6::Network doctest::doctest)
# 主题/聊天控件纯逻辑测试需要 mider_ui（app/Theme、app/ChatWidgets）；
# formatSummary 是纯函数、QSS 是纯字符串构建，均无需 QApplication 实例
target_link_libraries(mider_tests PRIVATE mider_ui)
if(MSVC)
    # 崩溃栈回溯（tests/test_main.cpp 的 dbghelp 符号解析）
    target_link_libraries(mider_tests PRIVATE dbghelp)
endif()
# 注册进 CTest：整个 doctest 套件作为一个测试用例（失败即非零退出码），
# 超时 5 分钟兜底防挂起（mock 流式重试类用例最坏耗时数十秒）
add_test(NAME mider_unit_tests COMMAND mider_tests)
set_tests_properties(mider_unit_tests PROPERTIES TIMEOUT 300)

# P0 对抗测试（安全回归资产，tests/adversarial/ 用例不许删、不许 skip）：独立可执行、
# 按套件拆独立 ctest 项（名字一律含 adversarial，供 ctest -R adversarial 收口）
add_executable(mider_adversarial_tests
    tests/adversarial/adversarial_main.cpp
    tests/adversarial/mock_sse_server.h
    tests/adversarial/WorkspaceBoundaryTest.cpp
    tests/adversarial/ReadChannelTest.cpp
    tests/adversarial/CommandWhitelistTest.cpp
    tests/adversarial/PersistencePoisoningTest.cpp
    tests/adversarial/WriteAcceptanceTest.cpp
)
target_link_libraries(mider_adversarial_tests PRIVATE mider_core doctest::doctest)
add_test(NAME adversarial.workspace_boundary
         COMMAND mider_adversarial_tests --test-suite=adversarial.workspace_boundary)
set_tests_properties(adversarial.workspace_boundary PROPERTIES TIMEOUT 300)
add_test(NAME adversarial.read_channel
         COMMAND mider_adversarial_tests --test-suite=adversarial.read_channel)
set_tests_properties(adversarial.read_channel PROPERTIES TIMEOUT 300)
add_test(NAME adversarial.command_whitelist
         COMMAND mider_adversarial_tests --test-suite=adversarial.command_whitelist)
set_tests_properties(adversarial.command_whitelist PROPERTIES TIMEOUT 300)
add_test(NAME adversarial.persistence_poisoning
         COMMAND mider_adversarial_tests --test-suite=adversarial.persistence_poisoning)
set_tests_properties(adversarial.persistence_poisoning PROPERTIES TIMEOUT 300)
# 产品验收（复用同一二进制的 mock-SSE/AgentLoop 基建）：写文件能力端到端
add_test(NAME acceptance.write_file
         COMMAND mider_adversarial_tests --test-suite=acceptance.write_file)
set_tests_properties(acceptance.write_file PROPERTIES TIMEOUT 300)
if(WIN32)
    # 测试 exe 的运行时 DLL（Qt 等）随构建就位，ctest 无需手工挂 PATH
    add_custom_command(TARGET mider_adversarial_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:mider_adversarial_tests> $<TARGET_FILE_DIR:mider_adversarial_tests>
        COMMAND_EXPAND_LISTS)
endif()
if(WIN32)
    # 测试 exe 的运行时 DLL（Qt 等）随构建就位，ctest 无需手工挂 PATH
    add_custom_command(TARGET mider_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:mider_tests> $<TARGET_FILE_DIR:mider_tests>
        COMMAND_EXPAND_LISTS)
endif()
