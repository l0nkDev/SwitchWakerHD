# shader_precompile (host): the shader manifest (shadercache_manifest.bin) + the game's programs -> the GLSL each
# combination translates to, as a WGS1 shadercache_gl.bin for tools/switch/dksh_cache. Uses the vendored Cemu
# decompiler in its OpenGL mode, as the deko3d renderer does. Built by build.sh in the dksh_cache container.
cmake_minimum_required(VERSION 3.20)
project(shader_precompile C CXX)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(WWHD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(CEMU_DIR ${WWHD_ROOT}/runtime/third_party/cemu)
file(GLOB_RECURSE CEMU_SOURCES ${CEMU_DIR}/*.cpp)
list(FILTER CEMU_SOURCES EXCLUDE REGEX "LatteDecompilerEmitMSL[^/]*\\.cpp$")
find_package(ZLIB REQUIRED)
add_executable(shader_precompile shader_precompile.cpp ${WWHD_ROOT}/runtime/src/gx2/fetch_shader_compact.cpp ${CEMU_SOURCES})
target_include_directories(shader_precompile PRIVATE ${CEMU_DIR} ${CEMU_DIR}/Cafe ${WWHD_ROOT}/runtime/third_party/fmt/include
                           ${WWHD_ROOT}/runtime/include ${WWHD_ROOT}/runtime/src ${WWHD_ROOT}/runtime/src/gx2)
target_compile_definitions(shader_precompile PRIVATE ENABLE_OPENGL=1 WWHD_HAS_DEKO3D=1)
target_compile_options(shader_precompile PRIVATE "SHELL:-include ${CEMU_DIR}/cemu_shim.h" -w -O2)
set_target_properties(shader_precompile PROPERTIES CXX_STANDARD 20)
target_link_libraries(shader_precompile PRIVATE ZLIB::ZLIB)
