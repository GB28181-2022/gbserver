cmake_minimum_required(VERSION 3.14)
project(gb_service CXX)

set(CMAKE_CXX_STANDARD 17)

#打开链路追踪
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  

# ZLToolKit: 使用 third_party 源码，关闭可选依赖以简化骨架
set(ENABLE_MYSQL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(ZLToolKit_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ZLToolKit")
if(NOT EXISTS "${ZLToolKit_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "ZLToolKit not found at ${ZLToolKit_DIR}, run: git clone --depth 1 https://github.com/ZLMediaKit/ZLToolKit.git third_party/ZLToolKit")
endif()
add_subdirectory(${ZLToolKit_DIR} ${CMAKE_BINARY_DIR}/_deps/zltoolkit-build)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# PJSIP：使用 PJSIP 实现 SIP 5060（GB28181 注册/心跳）
set(PJSIP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/pjproject/install" CACHE PATH "PJSIP install prefix")
if(NOT EXISTS "${PJSIP_ROOT}/include/pjsip.h")
  message(FATAL_ERROR "PJSIP not found at ${PJSIP_ROOT}, run: ./scripts/build_pjproject.sh")
endif()

add_executable(gb_service
  src/main.cpp
  src/infra/HttpSession.cpp
  src/infra/CatalogGroupService.cpp
  src/infra/CatalogGroupEncoding.cpp
  src/infra/UpstreamPlatformService.cpp
  src/infra/UpstreamRegistrar.cpp
  src/infra/UpstreamInviteBridge.cpp
  src/infra/AuthHelper.cpp
  src/infra/DbUtil.cpp
  src/infra/SipDigest.cpp
  src/infra/SipHandler.cpp
  src/infra/SipServerPjsip.cpp
  src/infra/SipCatalog.cpp
  src/infra/SipDeviceCache.cpp
  src/infra/MediaService.cpp
)

target_include_directories(gb_service PRIVATE ${PJSIP_ROOT}/include)
target_link_directories(gb_service PRIVATE ${PJSIP_ROOT}/lib)

# pjproject 安装的库名带平台后缀；链接顺序：pjsip -> pjlib-util -> pj，并加系统依赖
file(GLOB _libpjsip "${PJSIP_ROOT}/lib/libpjsip-*.a")
file(GLOB _libpjsip_ua "${PJSIP_ROOT}/lib/libpjsip-ua-*.a")
file(GLOB _libpjlib_util "${PJSIP_ROOT}/lib/libpjlib-util-*.a")
file(GLOB _libpj "${PJSIP_ROOT}/lib/libpj-*.a")
if(_libpjsip AND _libpjsip_ua AND _libpjlib_util AND _libpj)
  target_link_libraries(gb_service PRIVATE ${_libpjsip} ${_libpjsip_ua} ${_libpjlib_util} ${_libpj} uuid ZLToolKit CURL::libcurl OpenSSL::SSL OpenSSL::Crypto pq Threads::Threads)
else()
  target_link_libraries(gb_service PRIVATE pjsip pjsip-ua pjlib-util pj uuid ZLToolKit CURL::libcurl OpenSSL::SSL OpenSSL::Crypto pq Threads::Threads)
endif()

target_include_directories(gb_service PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  /usr/include/postgresql
)

# ---- 单元测试：编组国标纯逻辑（无第三方依赖）----
option(GB_SERVICE_BUILD_TESTS "Build catalog_group_encoding_test" ON)
if(GB_SERVICE_BUILD_TESTS)
  add_executable(catalog_group_encoding_test
    tests/catalog_group_encoding_test.cpp
    src/infra/CatalogGroupEncoding.cpp
  )
  target_include_directories(catalog_group_encoding_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  enable_testing()
  add_test(NAME catalog_group_encoding_test COMMAND catalog_group_encoding_test)
endif()
