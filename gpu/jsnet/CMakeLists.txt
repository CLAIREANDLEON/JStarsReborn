# JStarsReborn online: shadPS4 v0.20.0's network, NP and shadNet client sources (vendor/shadps4, as vendored by bbport with
# its fixes: vendor-changes-bbport.diff) plus jsnet_glue.cpp. GPL-2.0-or-later. See docs/ADR-008-online-shadnet.md.
#
# Built as its own static library with clang++ for x86_64-w64-windows-gnu against mingw-w64 GCC 13's libstdc++ (ABI
# compatible with the GCC-built renderer and runtime): GCC 13 for mingw cannot compile SysV-ABI functions that contain
# exception cleanup (docs/ADR-006), and nearly every NP entry point is one. Use tools/build_jsnet.sh.
cmake_minimum_required(VERSION 3.20)
project(jstars_net C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JSNET_DEPS "" CACHE PATH "prefix with mingw builds of protobuf 3.21.12, OpenSSL 3, miniupnpc, nlohmann_json")
set(JSNET_PROTOC protoc CACHE FILEPATH "host protoc, same version as the protobuf library (3.21.12)")
set(JSNET_BOOST_INCLUDE "" CACHE PATH "directory that contains boost/")
set(G ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(V ${CMAKE_CURRENT_SOURCE_DIR}/vendor/shadps4)
set(R ${G}/vendor)
set(PROTO_OUT ${CMAKE_CURRENT_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${PROTO_OUT})
add_custom_command(OUTPUT ${PROTO_OUT}/shadnet.pb.cc ${PROTO_OUT}/shadnet.pb.h
    COMMAND ${JSNET_PROTOC} --proto_path=${V}/shadnet --cpp_out=${PROTO_OUT} ${V}/shadnet/shadnet.proto
    DEPENDS ${V}/shadnet/shadnet.proto)

file(GLOB_RECURSE JSNET_SOURCES CONFIGURE_DEPENDS
    ${V}/core/libraries/net/*.cpp ${V}/core/libraries/network/*.cpp ${V}/core/libraries/np/*.cpp
    ${V}/core/net/*.cpp ${V}/shadnet/*.cpp)
# Trophies, commerce and the profile and Facebook dialogs stay in the C runtime (as in bbport).
list(FILTER JSNET_SOURCES EXCLUDE REGEX "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")

add_library(jstars_net STATIC ${JSNET_SOURCES} ${PROTO_OUT}/shadnet.pb.cc jsnet_glue.cpp third_party/wepoll/wepoll.c)
target_include_directories(jstars_net PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR} ${G}/shim ${V} ${V}/shadnet ${R}/shadps4/src ${PROTO_OUT}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wepoll
    ${R}/externals/fmt/include ${R}/externals/magic_enum/include ${R}/externals/vulkan-headers/include
    ${JSNET_DEPS}/include ${JSNET_DEPS}/include/miniupnpc)
if(JSNET_BOOST_INCLUDE)
    target_include_directories(jstars_net SYSTEM PRIVATE ${JSNET_BOOST_INCLUDE})
endif()
target_compile_definitions(jstars_net PRIVATE BOOST_ASIO_STANDALONE NDEBUG MINIUPNP_STATICLIB FMT_HEADER_ONLY=0
    CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT CPPHTTPLIB_DISABLE_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE
    NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00 PROTOBUF_USE_EXCEPTIONS=1)
target_compile_options(jstars_net PRIVATE -w -fno-strict-aliasing
    $<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/bb_win_compat.h>)
