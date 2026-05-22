# Toolchain file pinning Homebrew LLVM (keg-only) for this project.
# Apple Clang is left alone as the system default.
#
# Use: cmake -S . -B build --toolchain cmake/toolchain-llvm.cmake
# Or via CMakePresets.json (preferred).

set(LLVM_PREFIX "/opt/homebrew/opt/llvm" CACHE PATH "Homebrew LLVM prefix")

if(NOT EXISTS "${LLVM_PREFIX}/bin/clang++")
    message(FATAL_ERROR
        "Homebrew LLVM not found at ${LLVM_PREFIX}. "
        "Install with: brew install llvm")
endif()

set(CMAKE_C_COMPILER   "${LLVM_PREFIX}/bin/clang"   CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${LLVM_PREFIX}/bin/clang++" CACHE FILEPATH "" FORCE)
set(CMAKE_AR           "${LLVM_PREFIX}/bin/llvm-ar"      CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${LLVM_PREFIX}/bin/llvm-ranlib"  CACHE FILEPATH "" FORCE)

# Use Homebrew LLVM's libc++ (newer than Apple's — has <expected>, <flat_map>, <print>).
# Per brew caveats, libc++ features beyond system require _LIBCPP_DISABLE_AVAILABILITY.
add_compile_options(-stdlib=libc++ -D_LIBCPP_DISABLE_AVAILABILITY)
add_link_options(
    -stdlib=libc++
    -L${LLVM_PREFIX}/lib/c++
    -L${LLVM_PREFIX}/lib/unwind
    -lunwind
    -Wl,-rpath,${LLVM_PREFIX}/lib/c++
)
