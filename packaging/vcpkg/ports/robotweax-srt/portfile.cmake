vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/Robotweax/srt/archive/b19c6a88b6257c749e16dc1e53d803cc3c792e32.tar.gz"
    FILENAME "robotweax-srt-0.3.2.tar.gz"
    SHA512 db5ff293fc8f77279025de0d78598c8e44c5fabcdc668fe4067219f836c4641e3cf98cc549a288e6b2905d71c47803f98faa7a4f31ecdc5fd9d79f690714ff28
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
