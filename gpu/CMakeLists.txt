# JStarsReborn GPU library: vendored shadPS4 (b4e7ae77) video core + shader recompiler + GnmDriver +
# VideoOut + event queues, with the JStarsReborn integration layer (shim/, jgpu*.cpp).
# GPL-2.0-or-later. Builds a static library `jstars_gpu`.
cmake_minimum_required(VERSION 3.20)
project(jstars_gpu C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

set(JGPU_BOOST_INCLUDE "" CACHE PATH "directory that contains boost/ (header-only Boost.ICL/Asio)")
find_program(GLSLANG glslangValidator REQUIRED)

set(V ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(STRINGS sources.txt CPP_SOURCES)
file(STRINGS sources_c.txt C_SOURCES)
list(FILTER CPP_SOURCES EXCLUDE REGEX "^#")

# Host shaders (SPIR-V embedded as headers), same set as upstream's host_shaders/CMakeLists.txt.
add_custom_command(
    OUTPUT ${GEN}/video_core/host_shaders/fs_tri_vert.h
    COMMAND ${CMAKE_COMMAND} -E env GLSLANG=${GLSLANG} bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_host_shaders.sh ${GEN}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_host_shaders.sh
    COMMENT "Generating host shaders")
add_custom_target(jgpu_host_shaders DEPENDS ${GEN}/video_core/host_shaders/fs_tri_vert.h)

if(WIN32)
    set(JGPU_WINDOW jgpu_window_win32.cpp)
else()
    set(JGPU_WINDOW jgpu_window_x11.cpp)
endif()

add_library(jstars_gpu STATIC ${CPP_SOURCES} ${C_SOURCES} jgpu.cpp jgpu_kernel.cpp jgpu_stb.cpp jgpu_fsr.cpp ${JGPU_WINDOW})
add_dependencies(jstars_gpu jgpu_host_shaders)
target_include_directories(jstars_gpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim ${V}/shadps4 ${V}/shadps4/src ${GEN}
    ${V}/externals/fmt/include ${V}/externals/vulkan-headers/include ${V}/externals/vma/include
    ${V}/externals/sirit/include ${V}/externals/sirit/src ${V}/externals/sirit/externals/SPIRV-Headers/include
    ${V}/externals/robin-map/include ${V}/externals/xbyak ${V}/externals/magic_enum/include
    ${V}/externals/half/include ${V}/externals/gcn/include ${V}/externals/xxhash
    ${V}/externals/stb ${V}/externals/renderdoc ${V}/externals/ffx-api
    ${V}/externals/zydis/include ${V}/externals/zydis/src ${V}/externals/zydis/dependencies/zycore/include)
if(JGPU_BOOST_INCLUDE)
    target_include_directories(jstars_gpu SYSTEM PRIVATE ${JGPU_BOOST_INCLUDE})
endif()
target_include_directories(jstars_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(jstars_gpu PRIVATE FMT_HEADER_ONLY=0 ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD
    VULKAN_HPP_NO_EXCEPTIONS VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_NO_CONSTRUCTORS
    VULKAN_HPP_NO_STRUCT_CONSTRUCTORS VK_NO_PROTOTYPES VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=0 XXH_INLINE_ALL BOOST_ASIO_STANDALONE)
if(WIN32)
    target_compile_definitions(jstars_gpu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN NTDDI_VERSION=0x0A000006 _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS)
else()
    target_link_libraries(jstars_gpu PUBLIC X11 pthread)
endif()
target_compile_options(jstars_gpu PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-invalid-offsetof -w -fno-strict-aliasing>
    $<$<COMPILE_LANGUAGE:C>:-w>)

# GCC 13 for mingw-w64 cannot compile SysV-ABI functions that contain exception cleanup (assembler error
# ".seh_handlerdata used outside of .seh_proc block" / ICE in choose_baseaddr). The translation units that
# define such console-ABI entry points are therefore compiled with clang (same target, same libstdc++ headers,
# same SEH personality routine) and linked into the library as objects; see docs/ADR-006.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    find_program(JGPU_CLANGXX NAMES clang++-18 clang++ REQUIRED)
    set(JGPU_GXX_INC /usr/lib/gcc/x86_64-w64-mingw32/13-posix/include/c++ CACHE PATH "mingw libstdc++ headers")
    set(JGPU_CLANG_SRCS
        ${V}/shadps4/src/core/libraries/gnmdriver/gnmdriver.cpp
        ${V}/shadps4/src/core/libraries/videoout/video_out.cpp
        ${V}/shadps4/src/core/libraries/kernel/equeue.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/jgpu_kernel.cpp)
    set_source_files_properties(${JGPU_CLANG_SRCS} PROPERTIES HEADER_FILE_ONLY TRUE)
    set(JGPU_CLANG_OBJS)
    foreach(src ${JGPU_CLANG_SRCS})
        get_filename_component(nm ${src} NAME_WE)
        set(obj ${CMAKE_CURRENT_BINARY_DIR}/clang-objs/${nm}.cpp.obj)
        add_custom_command(OUTPUT ${obj}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/clang-objs
            COMMAND ${JGPU_CLANGXX} --target=x86_64-w64-windows-gnu --sysroot=/usr/x86_64-w64-mingw32
                    -nostdinc++ -isystem ${JGPU_GXX_INC} -isystem ${JGPU_GXX_INC}/x86_64-w64-mingw32
                    -std=gnu++23 -O3 -w -fno-strict-aliasing
                    "$<$<BOOL:$<TARGET_PROPERTY:jstars_gpu,COMPILE_DEFINITIONS>>:-D$<JOIN:$<TARGET_PROPERTY:jstars_gpu,COMPILE_DEFINITIONS>,;-D>>"
                    "$<$<BOOL:$<TARGET_PROPERTY:jstars_gpu,INCLUDE_DIRECTORIES>>:-I$<JOIN:$<TARGET_PROPERTY:jstars_gpu,INCLUDE_DIRECTORIES>,;-I>>"
                    -c ${src} -o ${obj}
            DEPENDS ${src}
            COMMAND_EXPAND_LISTS VERBATIM
            COMMENT "clang (mingw-gcc13 SysV EH workaround): ${nm}.cpp")
        list(APPEND JGPU_CLANG_OBJS ${obj})
    endforeach()
    set_source_files_properties(${JGPU_CLANG_OBJS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    target_sources(jstars_gpu PRIVATE ${JGPU_CLANG_OBJS})
endif()
