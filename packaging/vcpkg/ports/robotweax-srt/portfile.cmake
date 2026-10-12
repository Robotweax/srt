vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/Robotweax/srt/archive/80873657469344becb9c35f5306d0050ec5a566d.tar.gz"
    FILENAME "robotweax-srt-0.2.9.tar.gz"
    SHA512 284bcac0ccc2c910af25d781dd10a4e725283100a7e33fab3cfa8cea8883c97c2c67c5006ffddeae5fcba3c48d0ceff1c62fa29b5ac4e4702b5a6285a547d056
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
