# Windows x64 cross build with clang (build-winclang). Same MinGW-w64 headers,
# libraries and GNU ld as xboxrecomp/cmake/mingw-w64-x86_64.cmake, but clang
# gives native TLS (gs:[0x58] + _tls_index) for the per-thread guest registers
# where MinGW GCC 13 only has emutls: a __emutls_get_address call on every
# register access in the generated code. docs/windows-build.md.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MINGW_TRIPLET x86_64-w64-mingw32)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${MINGW_TRIPLET})
set(CMAKE_CXX_COMPILER_TARGET ${MINGW_TRIPLET})
set(CMAKE_RC_COMPILER ${MINGW_TRIPLET}-windres)
set(CMAKE_AR /usr/bin/${MINGW_TRIPLET}-ar)
set(CMAKE_RANLIB /usr/bin/${MINGW_TRIPLET}-ranlib)
set(MINGW_SYSROOT /usr/${MINGW_TRIPLET})
set(CMAKE_FIND_ROOT_PATH ${MINGW_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# MinGW GCC links winpthreads implicitly (13-posix); clang needs -pthread,
# which puts -lpthread after libstdc++ / libgcc_eh.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-pthread")
