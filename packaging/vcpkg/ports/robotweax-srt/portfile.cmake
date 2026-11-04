vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/Robotweax/srt/archive/838a022b75cfa95137bb7e40f575d0a65f6bd551.tar.gz"
    FILENAME "robotweax-srt-0.3.2.tar.gz"
    SHA512 0c8fe50aed576628c84a677a51009438365ed1d6bec63b84ac0c632903e97a8ba2bb6f8bfd3ced1f6109f2522e31db9d74d859b6f99d50b006d93e0a5a1090b3
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" ROBOTWEAX_SHARED)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED_LIBS=${ROBOTWEAX_SHARED}
        -DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced
        -DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=OFF
        -DROBOTWEAX_SRT_BUILD_TESTS=OFF
        -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF
        -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
        -DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF
        -DROBOTWEAX_SRT_WARNINGS_AS_ERRORS=OFF
        -DROBOTWEAX_SRT_CRYPTO_BACKEND=openssl
        -DENABLE_AEAD_API_PREVIEW=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME RobotweaxSRT CONFIG_PATH lib/cmake/RobotweaxSRT)
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
