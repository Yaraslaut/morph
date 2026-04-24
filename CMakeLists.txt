cmake_minimum_required(VERSION 3.25)

project(asyncFramework
    VERSION 0.1.0
    DESCRIPTION "Bridge/Model/Executor async framework demo"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASYNC_FRAMEWORK_BUILD_TESTS    "Build tests"    ON)
option(ASYNC_FRAMEWORK_BUILD_EXAMPLES "Build examples" ON)

include(cmake/compiler_options.cmake)

# ── glaze ────────────────────────────────────────────────────────────────────
# On Windows vcpkg provides glaze. On Linux we fall back to FetchContent so
# that CI does not require vcpkg.
find_package(glaze CONFIG QUIET)
if(NOT glaze_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG        v7.4.0
        GIT_SHALLOW    TRUE
    )
    set(glaze_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glaze)
endif()

if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

# ── Header-only library target ───────────────────────────────────────────────
add_library(async_framework INTERFACE)
add_library(async_framework::async_framework ALIAS async_framework)

target_include_directories(async_framework INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(async_framework INTERFACE glaze::glaze)
if(NOT WIN32)
    target_link_libraries(async_framework INTERFACE Threads::Threads)
endif()

set_target_properties(async_framework PROPERTIES
    VERIFY_INTERFACE_HEADER_SETS ON
)

target_sources(async_framework
    INTERFACE
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/async_framework/logger.hpp
        include/async_framework/executor.hpp
        include/async_framework/strand.hpp
        include/async_framework/task.hpp
        include/async_framework/completion.hpp
        include/async_framework/model.hpp
        include/async_framework/registry.hpp
        include/async_framework/backend.hpp
        include/async_framework/remote.hpp
        include/async_framework/bridge.hpp
)

# ── Demo executable ──────────────────────────────────────────────────────────
if(ASYNC_FRAMEWORK_BUILD_EXAMPLES)
    add_executable(asyncFramework src/main.cpp)
    target_link_libraries(asyncFramework PRIVATE async_framework)
    apply_warnings(asyncFramework)

    if(DEFINED AF_SANITIZER)
        apply_sanitizers(asyncFramework ${AF_SANITIZER})
    endif()
    if(AF_COVERAGE)
        apply_coverage(asyncFramework)
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(ASYNC_FRAMEWORK_BUILD_TESTS)
    # Under MSan every library Catch2 touches must also be MSan-instrumented.
    # The system apt package is not, so we always build from source in that mode.
    set(_catch2_use_fetch OFF)
    find_package(Catch2 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        set(_catch2_use_fetch ON)
    endif()

    if(_catch2_use_fetch)
        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.8.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    enable_testing()
    add_subdirectory(tests)
endif()

# ── Qt WebSocket backend (optional) ─────────────────────────────────────────
option(ASYNC_FRAMEWORK_BUILD_QT "Build Qt6 WebSocket backend and tests" OFF)

if(ASYNC_FRAMEWORK_BUILD_QT)
    if(WIN32)
        include(cmake/FindQt.cmake)
    endif()
    find_package(Qt6 COMPONENTS WebSockets REQUIRED)

    # Header-only interface for consumers
    add_library(async_framework_qt INTERFACE)
    add_library(async_framework::qt ALIAS async_framework_qt)
    target_link_libraries(async_framework_qt INTERFACE async_framework Qt6::WebSockets)
    target_sources(async_framework_qt
        INTERFACE
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/async_framework/qt/qt_executor.hpp
            include/async_framework/qt/qt_websocket_server.hpp
            include/async_framework/qt/qt_websocket_backend.hpp
    )

    # Compiled implementation (QObject subclass needs MOC via a .cpp file)
    add_library(async_framework_qt_impl STATIC
        src/qt/qt_websocket_server.cpp
        src/qt/qt_websocket_backend.cpp
        include/async_framework/qt/qt_websocket_server.hpp
    )
    target_link_libraries(async_framework_qt_impl PUBLIC async_framework_qt)
    set_target_properties(async_framework_qt_impl PROPERTIES AUTOMOC ON)

    if(ASYNC_FRAMEWORK_BUILD_TESTS)
        add_subdirectory(tests/qt)
    endif()

endif()
