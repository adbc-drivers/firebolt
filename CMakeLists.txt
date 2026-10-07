# Copyright (c) 2026 ADBC Drivers Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.20)
# The driver's own version — distinct from the ADBC specification version it
# implements (1.1.0, reported separately as ADBC_INFO_DRIVER_ADBC_VERSION).
# This is the single source of truth: it feeds the shared library soname and
# the ADBC_INFO_DRIVER_VERSION string via src/Version.h.in.  Bump it in the
# same commit as the release tag.
project(adbc_driver_firebolt VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# compile_commands.json is what clang-tidy (scripts/clang-tidy.sh) reads.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FIREBOLT_ADBC_BUILD_TESTS "Build unit tests" OFF)
option(FIREBOLT_ADBC_STATIC_LIBSTDCXX "Statically link libstdc++/libgcc into adbc_driver_firebolt" ON)
option(FIREBOLT_ADBC_STRIP_DEP_DEBUG "Strip debug info from dependency artifacts" ON)
option(WITH_SSL "Build curl with SSL support (what ships)" ON)
# FB2 SaaS (Legacy) mode: src/fb2/, always built.
set(_fb2_sources
    src/fb2/Fb2LegacyMode.cpp
    src/fb2/Fb2Http.cpp
    src/fb2/Fb2TokenClient.cpp
    src/fb2/Fb2EngineResolver.cpp
)

# All non-system dependencies must live under adbc/submodule.
set(ADBC_SUBMODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/submodule" CACHE PATH
    "Root directory that contains all adbc dependency submodules.")

function(require_dir path description)
    if (NOT IS_DIRECTORY "${path}")
        message(FATAL_ERROR "adbc_driver_firebolt: missing ${description}: ${path}")
    endif ()
endfunction()

function(require_static_target target_name)
    if (NOT TARGET "${target_name}")
        message(FATAL_ERROR "adbc_driver_firebolt: required target not found: ${target_name}")
    endif ()
    get_target_property(_target_type "${target_name}" TYPE)
    if (NOT _target_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "adbc_driver_firebolt: target ${target_name} must be static, got ${_target_type}")
    endif ()
endfunction()

set(_curl_src      "${ADBC_SUBMODULE_DIR}/curl")
set(_boringssl_src "${ADBC_SUBMODULE_DIR}/boringssl")
set(_boringssl_build "${CMAKE_CURRENT_BINARY_DIR}/submodule/boringssl")
set(_nanoarrow_src "${ADBC_SUBMODULE_DIR}/nanoarrow")
set(_gtest_src     "${ADBC_SUBMODULE_DIR}/googletest")
# nlohmann/json is header-only: no build, just its single-header include directory.
set(_json_include  "${ADBC_SUBMODULE_DIR}/json/single_include")
# Keep explicit archive paths for curl's OpenSSL detection hints.
set(_boringssl_ssl_archive    "${_boringssl_build}/libssl.a")
set(_boringssl_crypto_archive "${_boringssl_build}/libcrypto.a")

require_dir("${ADBC_SUBMODULE_DIR}" "ADBC submodule root")
require_dir("${_curl_src}" "curl source submodule")
require_dir("${_nanoarrow_src}" "nanoarrow source submodule")
require_dir("${_json_include}" "nlohmann/json source submodule")
set(_use_boringssl OFF)
if (WITH_SSL AND NOT WIN32)
    set(_use_boringssl ON)
    require_dir("${_boringssl_src}" "BoringSSL source submodule")
endif ()

# Binary size.  Every function and data object gets its own section, here and in
# every dependency added below, so the link can drop what nothing reaches
# (--gc-sections on adbc_driver_firebolt).  The version script exports two symbols, so
# most of BoringSSL, curl and libstdc++ is unreachable.  No effect on speed.
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-ffunction-sections -fdata-sections)
endif ()

if (_use_boringssl)
    # Build BoringSSL from source with regular CMake dependency graph wiring.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Disable BoringSSL shared library build" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "Disable BoringSSL tests" FORCE)
    # Drops large precomputed tables (P-256 multiples, among others): about 0.27 MB
    # smaller, at the cost of slower public-key operations, i.e. a slightly slower
    # TLS handshake once per connection.  Bulk encryption keeps its AES-NI assembly.
    set(OPENSSL_SMALL ON CACHE BOOL "Smaller BoringSSL: fewer precomputed tables" FORCE)
    add_subdirectory("${_boringssl_src}" "${_boringssl_build}")
    if (NOT TARGET ssl OR NOT TARGET crypto)
        message(FATAL_ERROR "adbc_driver_firebolt: BoringSSL ssl/crypto static targets were not generated")
    endif ()
    require_static_target(ssl)
    require_static_target(crypto)

    # BoringSSL only enforces PIC itself in shared-library mode; force it for our static-into-shared use case.
    set_target_properties(ssl crypto PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if (FIREBOLT_ADBC_STRIP_DEP_DEBUG)
        # Ensure BoringSSL object code is built without debug sections.
        get_property(_boringssl_targets DIRECTORY "${_boringssl_build}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach (_boringssl_target IN LISTS _boringssl_targets)
            get_target_property(_boringssl_target_type "${_boringssl_target}" TYPE)
            if (_boringssl_target_type STREQUAL "STATIC_LIBRARY" OR _boringssl_target_type STREQUAL "OBJECT_LIBRARY")
                target_compile_options("${_boringssl_target}" PRIVATE -g0)
            endif ()
        endforeach ()
    endif ()

    set(_ssl_lib_for_link "${_boringssl_ssl_archive}")
    set(_crypto_lib_for_link "${_boringssl_crypto_archive}")

    add_library(adbc_ssl STATIC IMPORTED GLOBAL)
    set_target_properties(adbc_ssl PROPERTIES
        IMPORTED_LOCATION "${_ssl_lib_for_link}"
    )

    add_library(adbc_crypto STATIC IMPORTED GLOBAL)
    set_target_properties(adbc_crypto PROPERTIES
        IMPORTED_LOCATION "${_crypto_lib_for_link}"
    )

    # Ensure curl can always resolve OpenSSL imported targets in fresh builds.
    if (NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
        set_target_properties(OpenSSL::Crypto PROPERTIES
            IMPORTED_LOCATION "${_crypto_lib_for_link}"
            INTERFACE_INCLUDE_DIRECTORIES "${_boringssl_src}/include"
        )
    endif ()
    if (NOT TARGET OpenSSL::SSL)
        add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
        set_target_properties(OpenSSL::SSL PROPERTIES
            IMPORTED_LOCATION "${_ssl_lib_for_link}"
            INTERFACE_INCLUDE_DIRECTORIES "${_boringssl_src}/include"
            INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
        )
    endif ()
endif ()

# Build curl from source (static-only, minimal HTTP(S) profile).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Disable curl shared library build" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Enable curl static library build" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "Disable curl executable build" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable curl tests" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Disable curl examples" FORCE)
set(CURL_BUILD_EVERYTHING OFF CACHE BOOL "Disable optional curl targets" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "Disable curl install" FORCE)
# getaddrinfo on a helper thread, so names resolve as everywhere else on the
# host: nsswitch, systemd-resolved, VPN split DNS, /etc/resolver.
set(ENABLE_ARES OFF CACHE BOOL "Disable c-ares in curl" FORCE)
set(ENABLE_THREADED_RESOLVER ON CACHE BOOL "Enable threaded resolver in curl" FORCE)
set(CURL_ENABLE_SSL ${WITH_SSL} CACHE BOOL "Enable SSL support in curl" FORCE)
set(CURL_USE_OPENSSL ${_use_boringssl} CACHE BOOL "Use BoringSSL via curl OpenSSL backend" FORCE)
set(CURL_USE_SCHANNEL OFF CACHE BOOL "Use Windows Schannel" FORCE)
# curl 8.15 removed Secure Transport, so macOS uses BoringSSL too.  SecTrust
# verifies against the system keychain whenever CURLOPT_CAINFO is unset.
set(USE_APPLE_SECTRUST ${_use_boringssl} CACHE BOOL "Verify peers with Apple SecTrust" FORCE)
if (WITH_SSL AND WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows Schannel" FORCE)
elseif (_use_boringssl)
    set(OPENSSL_INCLUDE_DIR "${_boringssl_src}/include" CACHE PATH "BoringSSL include directory" FORCE)
    set(OPENSSL_SSL_LIBRARY "${_ssl_lib_for_link}" CACHE FILEPATH "BoringSSL SSL static library" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY "${_crypto_lib_for_link}" CACHE FILEPATH "BoringSSL crypto static library" FORCE)
    # The archive paths above already force static BoringSSL. Keep this false so
    # CMake's FindOpenSSL does not assume OpenSSL's optional zlib dependency;
    # BoringSSL does not use it, and curl compression is disabled below.
    set(OPENSSL_USE_STATIC_LIBS FALSE CACHE BOOL "BoringSSL archive paths are explicit" FORCE)
else ()
    unset(OPENSSL_INCLUDE_DIR CACHE)
    unset(OPENSSL_SSL_LIBRARY CACHE)
    unset(OPENSSL_CRYPTO_LIBRARY CACHE)
    unset(OPENSSL_USE_STATIC_LIBS CACHE)
endif ()
# No baked-in CA location: configure would record the *build machine's* bundle
# path, which is wrong on hosts laid out differently.  DatabaseInit picks one at
# run time instead (src/TlsConfig.h).
set(CURL_CA_BUNDLE "none" CACHE STRING "No compiled-in CA bundle; chosen at run time" FORCE)
set(CURL_CA_PATH "none" CACHE STRING "No compiled-in CA directory; chosen at run time" FORCE)
set(HTTP_ONLY ON CACHE BOOL "Disable non-HTTP protocols in curl" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "Disable HTTP/2 in curl" FORCE)
set(USE_NGTCP2 OFF CACHE BOOL "Disable HTTP/3 ngtcp2 in curl" FORCE)
set(USE_QUICHE OFF CACHE BOOL "Disable HTTP/3 quiche in curl" FORCE)
set(CURL_DISABLE_PROXY ON CACHE BOOL "Disable proxy support in curl" FORCE)
set(CURL_DISABLE_COOKIES ON CACHE BOOL "Disable cookie support in curl" FORCE)
set(CURL_DISABLE_DOH ON CACHE BOOL "Disable DoH support in curl" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "Disable libpsl in curl" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "Disable libssh2 in curl" FORCE)
set(CURL_USE_LIBSSH OFF CACHE BOOL "Disable libssh in curl" FORCE)
set(CURL_USE_GSSAPI OFF CACHE BOOL "Disable GSSAPI in curl" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "Disable libidn2 in curl" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "Disable zlib support in curl" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "Disable brotli support in curl" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "Disable zstd support in curl" FORCE)
set(CURL_DROP_UNUSED ON CACHE BOOL "Drop unused curl code and data" FORCE)
# Features the driver never uses.  It sets the Authorization header itself, so
# none of curl's HTTP auth schemes is needed; ingest uses the MIME API, which
# stays, not the deprecated curl_formadd API.
foreach (_curl_unused
         HTTP_AUTH NTLM KERBEROS_AUTH NEGOTIATE_AUTH DIGEST_AUTH BASIC_AUTH BEARER_AUTH AWS
         HSTS ALTSVC NETRC FORM_API GETOPTIONS)
    set(CURL_DISABLE_${_curl_unused} ON CACHE BOOL "Not used by adbc_driver_firebolt" FORCE)
endforeach ()

add_subdirectory("${_curl_src}" "${CMAKE_CURRENT_BINARY_DIR}/submodule/curl")
if (TARGET CURL::libcurl_static)
    set(_curl_target CURL::libcurl_static)
elseif (TARGET libcurl_static)
    set(_curl_target libcurl_static)
elseif (TARGET CURL::libcurl)
    set(_curl_target CURL::libcurl)
else ()
    message(FATAL_ERROR "adbc_driver_firebolt: curl static target not found")
endif ()
require_static_target(${_curl_target})
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    set(_curl_warning_suppressions
        -Wno-deprecated-declarations
        -Wno-tautological-type-limit-compare
    )
    if (TARGET libcurl_object)
        target_compile_options(libcurl_object PRIVATE ${_curl_warning_suppressions})
    endif ()
    if (TARGET libcurl_static)
        target_compile_options(libcurl_static PRIVATE ${_curl_warning_suppressions})
    endif ()
endif ()
if (FIREBOLT_ADBC_STRIP_DEP_DEBUG)
    if (TARGET libcurl_object)
        target_compile_options(libcurl_object PRIVATE -g0)
    endif ()
    if (TARGET libcurl_static)
        target_compile_options(libcurl_static PRIVATE -g0)
    endif ()
endif ()

set(NANOARROW_IPC ON CACHE BOOL "Build nanoarrow IPC support" FORCE)
set(NANOARROW_BUILD_TESTS OFF CACHE BOOL "Disable nanoarrow tests" FORCE)
set(NANOARROW_BUILD_BENCHMARKS OFF CACHE BOOL "Disable nanoarrow benchmarks" FORCE)
add_subdirectory("${_nanoarrow_src}" "${CMAKE_CURRENT_BINARY_DIR}/submodule/nanoarrow")
if (TARGET nanoarrow_static)
    set(_nanoarrow_target nanoarrow_static)
elseif (TARGET nanoarrow)
    set(_nanoarrow_target nanoarrow)
else ()
    message(FATAL_ERROR "adbc_driver_firebolt: neither nanoarrow_static nor nanoarrow target exists")
endif ()

if (TARGET nanoarrow_ipc_static)
    set(_nanoarrow_ipc_target nanoarrow_ipc_static)
elseif (TARGET nanoarrow_ipc)
    set(_nanoarrow_ipc_target nanoarrow_ipc)
else ()
    message(FATAL_ERROR "adbc_driver_firebolt: neither nanoarrow_ipc_static nor nanoarrow_ipc target exists")
endif ()

require_static_target(${_nanoarrow_target})
require_static_target(${_nanoarrow_ipc_target})
if (FIREBOLT_ADBC_STRIP_DEP_DEBUG)
    if (TARGET nanoarrow_static)
        target_compile_options(nanoarrow_static PRIVATE -g0)
    endif ()
    if (TARGET nanoarrow_ipc_static)
        target_compile_options(nanoarrow_ipc_static PRIVATE -g0)
    endif ()
    if (TARGET flatccrt)
        target_compile_options(flatccrt PRIVATE -g0)
    endif ()
endif ()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
    @ONLY
)

add_library(adbc_driver_firebolt SHARED
    src/FireboltAdbcDriver.cpp
    src/FireboltAdbcMetadata.cpp
    src/HttpClient.cpp
    src/HttpHeaderParse.cpp
    src/ArrowIpcStream.cpp
    src/IngestSqlBuilder.cpp
    src/QueryParameters.cpp
    src/DescribeParameters.cpp
    src/TlsConfig.cpp
    src/FireboltUri.cpp
    ${_fb2_sources}
)

set_target_properties(adbc_driver_firebolt PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(adbc_driver_firebolt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# SYSTEM so nlohmann/json's own headers are exempt from this project's warnings.
target_include_directories(adbc_driver_firebolt SYSTEM PRIVATE ${_json_include})

target_link_libraries(adbc_driver_firebolt PRIVATE
    ${_curl_target}
    ${_nanoarrow_target}
    ${_nanoarrow_ipc_target}
)
if (UNIX)
    target_link_libraries(adbc_driver_firebolt PRIVATE m pthread)
endif ()
if (_use_boringssl)
    target_link_libraries(adbc_driver_firebolt PRIVATE
        adbc_ssl
        adbc_crypto
    )
endif ()

if (UNIX AND NOT APPLE)
    if (FIREBOLT_ADBC_STATIC_LIBSTDCXX)
        # Remove dynamic dependency on libstdc++.so/libgcc_s.so where supported.
        target_link_options(adbc_driver_firebolt PRIVATE -static-libstdc++ -static-libgcc)
    endif ()

    target_link_options(adbc_driver_firebolt PRIVATE
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/adbc_driver_firebolt.version
        -Wl,--as-needed
        -Wl,--gc-sections
    )
    set_property(TARGET adbc_driver_firebolt APPEND PROPERTY
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/adbc_driver_firebolt.version
    )

    # lld, when the compiler can use it, also folds identical functions.  Asked of
    # the compiler rather than set through CMAKE_LINKER, which the compiler driver
    # ignores when it links.
    include(CheckLinkerFlag)
    check_linker_flag(CXX "-fuse-ld=lld" FIREBOLT_ADBC_HAVE_LLD)
    if (FIREBOLT_ADBC_HAVE_LLD)
        target_link_options(adbc_driver_firebolt PRIVATE -fuse-ld=lld -Wl,--icf=all)
    else ()
        message(STATUS "adbc_driver_firebolt: lld not available; linking without identical code folding")
    endif ()

    # Print dynamic dependencies after build:
    add_custom_command(TARGET adbc_driver_firebolt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "$<TARGET_FILE:adbc_driver_firebolt> .so dependencies:"
        COMMAND /usr/bin/env bash -c "readelf -d \"$<TARGET_FILE:adbc_driver_firebolt>\" 2>/dev/null | awk '/\\(NEEDED\\)/ { gsub(/\\[/, \"\", $5); gsub(/\\]/, \"\", $5); print $5 }' || true"
        VERBATIM
    )
elseif (APPLE)
    # Mach-O has no GNU version scripts. Keep symbols from statically linked
    # curl/nanoarrow objects out of the public driver ABI with an allowlist.
    target_link_options(adbc_driver_firebolt PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/adbc_driver_firebolt.exports"
    )
    set_property(TARGET adbc_driver_firebolt APPEND PROPERTY
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/adbc_driver_firebolt.exports
    )
endif ()

if (FIREBOLT_ADBC_BUILD_TESTS)
    require_dir("${_gtest_src}" "googletest source submodule")
    set(INSTALL_GTEST OFF CACHE BOOL "Disable gtest install" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "Disable gmock build" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime" FORCE)
    add_subdirectory("${_gtest_src}" "${CMAKE_CURRENT_BINARY_DIR}/submodule/googletest")

    if (TARGET GTest::gtest_main)
        set(_gtest_main_target GTest::gtest_main)
    elseif (TARGET gtest_main)
        set(_gtest_main_target gtest_main)
    else ()
        message(FATAL_ERROR "adbc_driver_firebolt: gtest_main target not found in googletest submodule")
    endif ()

    if (TARGET gtest)
        require_static_target(gtest)
    endif ()
    if (TARGET gtest_main)
        require_static_target(gtest_main)
    endif ()

    set(_fb2_tests
        tests/unit/fb2_legacy_test.cpp
        tests/unit/fb2_token_client_test.cpp
        tests/unit/fb2_engine_resolver_test.cpp
        src/fb2/Fb2Http.cpp
        src/fb2/Fb2TokenClient.cpp
        src/fb2/Fb2EngineResolver.cpp
    )
    add_executable(adbc_driver_tests
        tests/unit/adbc_driver_test.cpp
        tests/unit/session_state_test.cpp
        tests/unit/tls_config_test.cpp
        tests/unit/firebolt_uri_test.cpp
        src/FireboltUri.cpp
        ${_fb2_tests}
        src/ArrowIpcStream.cpp
        src/IngestSqlBuilder.cpp
        src/HttpHeaderParse.cpp
        src/QueryParameters.cpp
        src/DescribeParameters.cpp
        src/TlsConfig.cpp
    )

    target_include_directories(adbc_driver_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_include_directories(adbc_driver_tests SYSTEM PRIVATE ${_json_include})

    target_compile_definitions(adbc_driver_tests PRIVATE
        FIREBOLT_ADBC_DRIVER_LIBRARY_PATH="$<TARGET_FILE:adbc_driver_firebolt>"
    )

    target_link_libraries(adbc_driver_tests PRIVATE
        ${_gtest_main_target}
        adbc_driver_firebolt
        ${_nanoarrow_target}
        ${_nanoarrow_ipc_target}
        ${_curl_target}
        ${CMAKE_DL_LIBS}
    )
    if (UNIX)
        target_link_libraries(adbc_driver_tests PRIVATE m)
    endif ()

    if (WIN32)
        add_custom_command(TARGET adbc_driver_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:adbc_driver_firebolt>
                $<TARGET_FILE_DIR:adbc_driver_tests>
        )
    endif ()

    enable_testing()
    add_test(NAME adbc_driver_tests COMMAND adbc_driver_tests)
endif ()

install(TARGETS adbc_driver_firebolt LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES adbc.h DESTINATION include/adbc_driver_firebolt)
