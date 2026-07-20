cmake_minimum_required(VERSION 3.20)
project(voltio C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W4 /WX)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Platform serial backend
# ---------------------------------------------------------------------------
if(WIN32)
    set(SERIAL_IMPL src/serial_win32.c)
else()
    set(SERIAL_IMPL src/serial_posix.c)
endif()

# ---------------------------------------------------------------------------
# Core driver  (OBJECT library)
# ---------------------------------------------------------------------------
add_library(voltio_core OBJECT
    src/voltio.c
    ${SERIAL_IMPL}
)
target_include_directories(voltio_core PUBLIC src)

# ---------------------------------------------------------------------------
# Target 1: voltio.dll  — Windows only
# voltio_dll.c uses Win32 headers so it cannot be built on macOS.
# On macOS only the CLI is built; copy the whole project to Windows
# to produce the DLL for CANoe.
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(voltio_lib SHARED src/voltio_dll.c)
    target_link_libraries(voltio_lib PRIVATE voltio_core)
    target_include_directories(voltio_lib PUBLIC src)
    set_target_properties(voltio_lib PROPERTIES
        OUTPUT_NAME              "voltio"
        PREFIX                   ""
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

# ---------------------------------------------------------------------------
# Target 2: voltio  (CLI — builds on all platforms)
# ---------------------------------------------------------------------------
add_executable(voltio_exe src/voltio_cli.c)
target_link_libraries(voltio_exe PRIVATE voltio_core)
target_include_directories(voltio_exe PRIVATE src)
set_target_properties(voltio_exe PROPERTIES
    OUTPUT_NAME              "voltio"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
