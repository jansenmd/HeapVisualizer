cmake_minimum_required(VERSION 3.20)

project(HeapVisualizer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(DEFINED ENV{QT_ROOT} AND NOT "$ENV{QT_ROOT}" STREQUAL "")
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT}")
endif()

function(heapvisualizer_qt_not_found_message out_var)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        string(CONCAT message_text
            "Qt Widgets was not found for the Windows build target.\n"
            "\n"
            "Install the required Windows Qt resources before building.\n"
            "\n"
            "Windows native build:\n"
            "  1. Install Qt with the Qt Online Installer.\n"
            "  2. Include a Desktop MinGW 64-bit Qt kit, such as Qt 6.x.x MinGW 64-bit.\n"
            "  3. Point CMake at that kit, for example:\n"
            "       cmake -S . -B build/windows-mingw-release -DCMAKE_PREFIX_PATH=C:/Qt/6.x.x/mingw_64\n"
            "\n"
            "Windows cross-build from Linux/WSL:\n"
            "  1. Install the MinGW-w64 cross compiler:\n"
            "       sudo apt update && sudo apt install mingw-w64\n"
            "  2. Install Qt for Windows with the Qt Online Installer and include the MinGW 64-bit kit.\n"
            "  3. Set QT_ROOT to the Qt MinGW kit and run a Windows build script:\n"
            "       export QT_ROOT='C:/Qt/6.x.x/mingw_64'\n"
            "       ./scripts/build-windows-release.sh\n"
            "\n"
            "QT_ROOT or CMAKE_PREFIX_PATH must point to a Windows MinGW Qt kit, not a Linux Qt install."
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        string(CONCAT message_text
            "Qt Widgets was not found for the Linux build target.\n"
            "\n"
            "Install the required Linux Qt development resources before building.\n"
            "\n"
            "Ubuntu/Debian/WSL, Qt 6:\n"
            "  sudo apt update && sudo apt install cmake g++ qt6-base-dev\n"
            "\n"
            "Ubuntu/Debian/WSL, Qt 5:\n"
            "  sudo apt update && sudo apt install cmake g++ qtbase5-dev\n"
            "\n"
            "Fedora, Qt 6:\n"
            "  sudo dnf install cmake gcc-c++ qt6-qtbase-devel\n"
            "\n"
            "Arch, Qt 6:\n"
            "  sudo pacman -S cmake gcc qt6-base\n"
            "\n"
            "If Qt is installed in a non-standard location, set QT_ROOT or CMAKE_PREFIX_PATH to the Qt kit path."
        )
    else()
        string(CONCAT message_text
            "Qt Widgets was not found.\n"
            "\n"
            "Install Qt 6 or Qt 5 Widgets development files for your target OS, then set QT_ROOT or CMAKE_PREFIX_PATH if CMake cannot find them automatically."
        )
    endif()

    set(${out_var} "${message_text}" PARENT_SCOPE)
endfunction()

find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6_FOUND)
    set(QT_PACKAGE Qt6)
else()
    find_package(Qt5 COMPONENTS Widgets QUIET)
    if(Qt5_FOUND)
        set(QT_PACKAGE Qt5)
    else()
        heapvisualizer_qt_not_found_message(qt_error_message)
        message(FATAL_ERROR "${qt_error_message}")
    endif()
endif()

string(TIMESTAMP HEAPVISUALIZER_BUILD_DATE "%Y-%m-%d")
configure_file(
    src/build_info.hpp.in
    generated/build_info.hpp
    @ONLY
)

set(app_sources
    src/AllocationStreamReader.cpp
    src/appstyle.cpp
    src/main.cpp
    src/mainwindow.cpp
)

set(test_app_sources
    src/appstyle.cpp
    src/allocationtest_main.cpp
    src/allocationtestwindow.cpp
)

set(app_headers
    src/AllocationStreamReader.hpp
    src/appstyle.hpp
    src/mainwindow.hpp
)

set(test_app_headers
    src/appstyle.hpp
    src/allocationtestwindow.hpp
)

if(WIN32)
    add_executable(HeapVisualizer WIN32 ${app_sources} ${app_headers})
    add_executable(HeapAllocationTest WIN32 ${test_app_sources} ${test_app_headers})
else()
    add_executable(HeapVisualizer ${app_sources} ${app_headers})
    add_executable(HeapAllocationTest ${test_app_sources} ${test_app_headers})
endif()

add_subdirectory(src/Preload)

set(gui_targets
    HeapVisualizer
    HeapAllocationTest
)

foreach(target_name ${gui_targets})
    target_include_directories(${target_name}
        PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )

    target_link_libraries(${target_name} PRIVATE ${QT_PACKAGE}::Widgets)

    target_compile_options(${target_name}
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
            $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
endforeach()

if(MINGW)
    foreach(target_name ${gui_targets})
        target_link_options(${target_name} PRIVATE -static-libgcc -static-libstdc++)
    endforeach()
endif()

if(WIN32)
    target_link_libraries(HeapVisualizer PRIVATE ws2_32)
endif()
