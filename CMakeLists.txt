cmake_minimum_required(VERSION 3.20)
project(firebolt_adbc VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(FIREBOLT_ADBC_BUILD_TESTS "Build unit tests" OFF)
option(FIREBOLT_ADBC_STATIC_LIBSTDCXX "Statically link libstdc++/libgcc into firebolt_adbc" ON)
option(FIREBOLT_ADBC_STRIP_DEP_DEBUG "Strip debug info from dependency artifacts" ON)
option(WITH_SSL "Build curl with SSL support via BoringSSL" OFF)

# All non-system dependencies must live under adbc/submodule.
set(ADBC_SUBMODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/submodule" CACHE PATH
    "Root directory that contains all adbc dependency submodules.")

function(require_dir path description)
    if (NOT IS_DIRECTORY "${path}")
        message(FATAL_ERROR "firebolt_adbc: missing ${description}: ${path}")
    endif ()
endfunction()

function(require_static_target target_name)
    if (NOT TARGET "${target_name}")
        message(FATAL_ERROR "firebolt_adbc: required target not found: ${target_name}")
    endif ()
    get_target_property(_target_type "${target_name}" TYPE)
    if (NOT _target_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "firebolt_adbc: target ${target_name} must be static, got ${_target_type}")
    endif ()
endfunction()

set(_curl_src      "${ADBC_SUBMODULE_DIR}/curl")
set(_cares_src     "${ADBC_SUBMODULE_DIR}/c-ares")
set(_boringssl_src "${ADBC_SUBMODULE_DIR}/boringssl")
set(_boringssl_build "${CMAKE_CURRENT_BINARY_DIR}/submodule/boringssl")
set(_nanoarrow_src "${ADBC_SUBMODULE_DIR}/nanoarrow")
set(_gtest_src     "${ADBC_SUBMODULE_DIR}/googletest")
# Keep explicit archive paths for curl's OpenSSL detection hints.
set(_boringssl_ssl_archive    "${_boringssl_build}/libssl.a")
set(_boringssl_crypto_archive "${_boringssl_build}/libcrypto.a")

require_dir("${ADBC_SUBMODULE_DIR}" "ADBC submodule root")
require_dir("${_curl_src}" "curl source submodule")
require_dir("${_cares_src}" "c-ares source submodule")
require_dir("${_nanoarrow_src}" "nanoarrow source submodule")
if (WITH_SSL)
    require_dir("${_boringssl_src}" "BoringSSL source submodule")
endif ()

if (WITH_SSL)
    # Build BoringSSL from source with regular CMake dependency graph wiring.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Disable BoringSSL shared library build" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "Disable BoringSSL tests" FORCE)
    add_subdirectory("${_boringssl_src}" "${_boringssl_build}")
    if (NOT TARGET ssl OR NOT TARGET crypto)
        message(FATAL_ERROR "firebolt_adbc: BoringSSL ssl/crypto static targets were not generated")
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

# Build c-ares from source (static-only, minimal).
set(CARES_STATIC ON CACHE BOOL "Build c-ares as static library" FORCE)
set(CARES_SHARED OFF CACHE BOOL "Disable c-ares shared library" FORCE)
set(CARES_STATIC_PIC ON CACHE BOOL "Build c-ares static library as PIC" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "Disable c-ares tests" FORCE)
set(CARES_BUILD_CONTAINER_TESTS OFF CACHE BOOL "Disable c-ares container tests" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "Disable c-ares tools" FORCE)
set(CARES_INSTALL OFF CACHE BOOL "Disable c-ares install" FORCE)
add_subdirectory("${_cares_src}" "${CMAKE_CURRENT_BINARY_DIR}/submodule/c-ares")

if (TARGET c-ares::cares_static)
    set(_cares_target c-ares::cares_static)
elseif (TARGET c-ares_static)
    set(_cares_target c-ares_static)
elseif (TARGET c-ares::cares)
    set(_cares_target c-ares::cares)
else ()
    message(FATAL_ERROR "firebolt_adbc: c-ares static target not found")
endif ()
require_static_target(${_cares_target})
if (FIREBOLT_ADBC_STRIP_DEP_DEBUG AND TARGET c-ares_static)
    target_compile_options(c-ares_static PRIVATE -g0)
endif ()

# Build curl from source (static-only, minimal HTTP(S) profile).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Disable curl shared library build" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Enable curl static library build" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "Disable curl executable build" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable curl tests" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Disable curl examples" FORCE)
set(CURL_BUILD_EVERYTHING OFF CACHE BOOL "Disable optional curl targets" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "Disable curl install" FORCE)
set(ENABLE_ARES ON CACHE BOOL "Enable c-ares in curl" FORCE)
set(ENABLE_THREADED_RESOLVER OFF CACHE BOOL "Disable threaded resolver in curl" FORCE)
set(CURL_ENABLE_SSL ${WITH_SSL} CACHE BOOL "Enable SSL support in curl" FORCE)
set(CURL_USE_OPENSSL ${WITH_SSL} CACHE BOOL "Use BoringSSL via curl OpenSSL backend" FORCE)
if (WITH_SSL)
    set(OPENSSL_INCLUDE_DIR "${_boringssl_src}/include" CACHE PATH "BoringSSL include directory" FORCE)
    set(OPENSSL_SSL_LIBRARY "${_ssl_lib_for_link}" CACHE FILEPATH "BoringSSL SSL static library" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY "${_crypto_lib_for_link}" CACHE FILEPATH "BoringSSL crypto static library" FORCE)
    set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "Use static BoringSSL libraries" FORCE)
else ()
    unset(OPENSSL_INCLUDE_DIR CACHE)
    unset(OPENSSL_SSL_LIBRARY CACHE)
    unset(OPENSSL_CRYPTO_LIBRARY CACHE)
    unset(OPENSSL_USE_STATIC_LIBS CACHE)
endif ()
set(HTTP_ONLY ON CACHE BOOL "Disable non-HTTP protocols in curl" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "Disable HTTP/2 in curl" FORCE)
set(USE_NGTCP2 OFF CACHE BOOL "Disable HTTP/3 ngtcp2 in curl" FORCE)
set(USE_QUICHE OFF CACHE BOOL "Disable HTTP/3 quiche in curl" FORCE)
set(CURL_DISABLE_PROXY ON CACHE BOOL "Disable proxy support in curl" FORCE)
set(CURL_DISABLE_COOKIES ON CACHE BOOL "Disable cookie support in curl" FORCE)
set(CURL_DISABLE_DOH ON CACHE BOOL "Disable DoH support in curl" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "Disable libpsl in curl" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "Disable libidn2 in curl" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "Disable zlib support in curl" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "Disable brotli support in curl" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "Disable zstd support in curl" FORCE)
set(CURL_DROP_UNUSED ON CACHE BOOL "Drop unused curl code and data" FORCE)

# curl's FindCares.cmake consumes these hints when ENABLE_ARES=ON.
# c-ares generates ares_build.h into its binary dir, while ares.h lives in source include.
set(CARES_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/submodule/c-ares;${_cares_src}/include" CACHE STRING "c-ares include dirs for curl" FORCE)
set(CARES_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/submodule/c-ares/src/lib/libcares.a" CACHE FILEPATH "c-ares static library for curl" FORCE)
set(CARES_USE_STATIC_LIBS TRUE CACHE BOOL "Use static c-ares in curl" FORCE)

add_subdirectory("${_curl_src}" "${CMAKE_CURRENT_BINARY_DIR}/submodule/curl")
if (TARGET CURL::libcurl_static)
    set(_curl_target CURL::libcurl_static)
elseif (TARGET libcurl_static)
    set(_curl_target libcurl_static)
elseif (TARGET CURL::libcurl)
    set(_curl_target CURL::libcurl)
else ()
    message(FATAL_ERROR "firebolt_adbc: curl static target not found")
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
    message(FATAL_ERROR "firebolt_adbc: neither nanoarrow_static nor nanoarrow target exists")
endif ()

if (TARGET nanoarrow_ipc_static)
    set(_nanoarrow_ipc_target nanoarrow_ipc_static)
elseif (TARGET nanoarrow_ipc)
    set(_nanoarrow_ipc_target nanoarrow_ipc)
else ()
    message(FATAL_ERROR "firebolt_adbc: neither nanoarrow_ipc_static nor nanoarrow_ipc target exists")
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

add_library(firebolt_adbc SHARED
    src/FireboltAdbcDriver.cpp
    src/FireboltAdbcMetadata.cpp
    src/HttpClient.cpp
    src/ArrowIpcStream.cpp
)

set_target_properties(firebolt_adbc PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION 1.1.0
    SOVERSION 1
)

target_include_directories(firebolt_adbc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(firebolt_adbc PRIVATE
    ${_curl_target}
    ${_nanoarrow_target}
    ${_nanoarrow_ipc_target}
    m
    pthread
    z
)
if (WITH_SSL)
    target_link_libraries(firebolt_adbc PRIVATE
        adbc_ssl
        adbc_crypto
    )
endif ()

if (UNIX AND NOT APPLE)
    if (FIREBOLT_ADBC_STATIC_LIBSTDCXX)
        # Remove dynamic dependency on libstdc++.so/libgcc_s.so where supported.
        target_link_options(firebolt_adbc PRIVATE -static-libstdc++ -static-libgcc)
    endif ()

    target_link_options(firebolt_adbc PRIVATE
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/firebolt_adbc.version
        -Wl,--as-needed
    )

    # Print dynamic dependencies after build:
    add_custom_command(TARGET firebolt_adbc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "$<TARGET_FILE:firebolt_adbc> .so dependencies:"
        COMMAND /usr/bin/env bash -c "readelf -d \"$<TARGET_FILE:firebolt_adbc>\" 2>/dev/null | awk '/\\(NEEDED\\)/ { gsub(/\\[/, \"\", $5); gsub(/\\]/, \"\", $5); print $5 }' || true"
        VERBATIM
    )
endif ()

if (FIREBOLT_ADBC_BUILD_TESTS)
    require_dir("${_gtest_src}" "googletest source submodule")
    set(INSTALL_GTEST OFF CACHE BOOL "Disable gtest install" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "Disable gmock build" FORCE)
    add_subdirectory("${_gtest_src}" "${CMAKE_CURRENT_BINARY_DIR}/submodule/googletest")

    if (TARGET GTest::gtest_main)
        set(_gtest_main_target GTest::gtest_main)
    elseif (TARGET gtest_main)
        set(_gtest_main_target gtest_main)
    else ()
        message(FATAL_ERROR "firebolt_adbc: gtest_main target not found in googletest submodule")
    endif ()

    if (TARGET gtest)
        require_static_target(gtest)
    endif ()
    if (TARGET gtest_main)
        require_static_target(gtest_main)
    endif ()

    add_executable(adbc_driver_tests
        tests/unit/adbc_driver_test.cpp
        src/ArrowIpcStream.cpp
    )

    target_include_directories(adbc_driver_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(adbc_driver_tests PRIVATE
        ${_gtest_main_target}
        firebolt_adbc
        ${_nanoarrow_target}
        ${_nanoarrow_ipc_target}
        m
    )

    enable_testing()
    add_test(NAME adbc_driver_tests COMMAND adbc_driver_tests)
endif ()

install(TARGETS firebolt_adbc LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES adbc.h DESTINATION include/firebolt_adbc)
