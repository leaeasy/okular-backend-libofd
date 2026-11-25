# SPDX-FileCopyrightText: 2026 OFD plugin contributors
# SPDX-License-Identifier: BSD-3-Clause
ecm_add_test(ofdtest.cpp
    TEST_NAME ofdtest
    LINK_LIBRARIES ofd_document Okular::Core Qt6::Test KF6::Archive KF6::CoreAddons
)
target_compile_definitions(ofdtest PRIVATE
    GENERATOR_PATH="$<TARGET_FILE:okularGenerator_ofd>"
    OFD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    OFD_CMAKE_COMMAND="${CMAKE_COMMAND}"
    OFD_BUILD_DIR="${PROJECT_BINARY_DIR}"
)
add_dependencies(ofdtest okularGenerator_ofd)
