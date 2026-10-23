vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/Robotweax/srt/archive/003a5f8046000c4801b2695f8b316ce05976bbbb.tar.gz"
    FILENAME "robotweax-srt-0.3.1.tar.gz"
    SHA512 c143231c9370d6af2838e5cdba0f1e52aefdb79156924fae642ad15a0a190523eb647d37f36ed56c8ca8fa651f9c73a7c2e0bc37b5eaad1dc6883072cce90e02
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
