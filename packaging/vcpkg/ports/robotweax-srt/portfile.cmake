vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/Robotweax/srt/archive/f5948a4b059aa1c8e7aa5e932053f92ff37eb6ff.tar.gz"
    FILENAME "robotweax-srt-0.3.3.tar.gz"
    SHA512 873bcd6a239e4f34156c1f0bbb1dc0cb24deb0d30a8446e6098fe0df5db9824c6b2e26bc41cd27bc671b60dfdb7ce0bb2a8e5e9301c0764b514630f741a14ec8
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
