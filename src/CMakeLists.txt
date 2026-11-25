# SPDX-FileCopyrightText: 2026 OFD plugin contributors
# SPDX-License-Identifier: BSD-3-Clause
add_library(ofd_document STATIC document.cpp geometry.cpp)
set_target_properties(ofd_document PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ofd_document PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ofd_document PUBLIC Qt6::Gui PRIVATE Qt6::Xml KF6::Archive)

kcoreaddons_add_plugin(okularGenerator_ofd
    INSTALL_NAMESPACE "okular_generators"
    SOURCES generator_ofd.cpp
)
target_link_libraries(okularGenerator_ofd PRIVATE ofd_document Okular::Core KF6::I18n)
target_compile_definitions(okularGenerator_ofd PRIVATE TRANSLATION_DOMAIN="okular_ofd")
