# SPDX-License-Identifier: MIT
# Crypto-only discovery also works with distro packages lacking CMake metadata.
find_path(MbedTLS_INCLUDE_DIR NAMES mbedtls/version.h
    HINTS "${MbedTLS_ROOT}" PATH_SUFFIXES include)
find_library(MbedTLS_CRYPTO_LIBRARY NAMES mbedcrypto
    HINTS "${MbedTLS_ROOT}" PATH_SUFFIXES lib lib64)
if(MbedTLS_INCLUDE_DIR)
    file(STRINGS "${MbedTLS_INCLUDE_DIR}/mbedtls/build_info.h" _mbedtls_version_line
        REGEX "^#define MBEDTLS_VERSION_STRING[ \t]+\"")
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" MbedTLS_VERSION
        "${_mbedtls_version_line}")
endif()
set(MbedTLS_SUPPORTED_VERSION FALSE)
if(MbedTLS_VERSION VERSION_GREATER_EQUAL 3.6.7 AND MbedTLS_VERSION VERSION_LESS 3.7)
    set(MbedTLS_SUPPORTED_VERSION TRUE)
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MbedTLS
    REQUIRED_VARS MbedTLS_INCLUDE_DIR MbedTLS_CRYPTO_LIBRARY MbedTLS_SUPPORTED_VERSION
    VERSION_VAR MbedTLS_VERSION
    REASON_FAILURE_MESSAGE "Robotweax supports Mbed TLS 3.6.7+ in the 3.6.x family, 4.x/TF-PSA-Crypto is unsupported")
if(MbedTLS_FOUND)
    # Prefer upstream exports, including static-library platform dependencies.
    find_package(MbedTLS ${MbedTLS_VERSION} EXACT CONFIG QUIET)
    set(MbedTLS_FOUND TRUE)
endif()
if(MbedTLS_FOUND AND NOT TARGET MbedTLS::mbedcrypto)
    add_library(MbedTLS::mbedcrypto UNKNOWN IMPORTED)
    set_target_properties(MbedTLS::mbedcrypto PROPERTIES
        IMPORTED_LOCATION "${MbedTLS_CRYPTO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MbedTLS_INCLUDE_DIR}")
    # The legacy entropy module uses the Windows system RNG.
    if(WIN32)
        set_property(TARGET MbedTLS::mbedcrypto PROPERTY
            INTERFACE_LINK_LIBRARIES bcrypt)
    endif()
endif()
mark_as_advanced(MbedTLS_INCLUDE_DIR MbedTLS_CRYPTO_LIBRARY)
