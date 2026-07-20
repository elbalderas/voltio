# toolchain-mingw64.cmake  —  Cross-compile macOS -> Windows x64
#
# Install:  brew install mingw-w64
#
# Usage:
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake
#   cmake --build build-win
#
# Output: build-win/bin/voltio.exe  and  build-win/bin/voltio.dll

set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(MINGW_GCC  NAMES x86_64-w64-mingw32-gcc  PATHS /opt/homebrew/bin /usr/local/bin REQUIRED)
find_program(MINGW_GXX  NAMES x86_64-w64-mingw32-g++  PATHS /opt/homebrew/bin /usr/local/bin REQUIRED)
find_program(MINGW_RC   NAMES x86_64-w64-mingw32-windres PATHS /opt/homebrew/bin /usr/local/bin)

set(CMAKE_C_COMPILER   ${MINGW_GCC})
set(CMAKE_CXX_COMPILER ${MINGW_GXX})
if(MINGW_RC)
    set(CMAKE_RC_COMPILER ${MINGW_RC})
endif()

# Static link MinGW runtime — self-contained binary, no extra DLLs needed
add_link_options(-static-libgcc -static)

set(CMAKE_FIND_ROOT_PATH
    /opt/homebrew/opt/mingw-w64/toolchain-x86_64
    /usr/local/opt/mingw-w64/toolchain-x86_64
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
