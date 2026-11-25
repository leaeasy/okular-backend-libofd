# SPDX-FileCopyrightText: 2026 OFD plugin contributors
# SPDX-License-Identifier: BSD-3-Clause
cmake_minimum_required(VERSION 3.22)
project(okular_ofd VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ECM 6.13 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(CTest)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Xml Widgets)
find_package(KF6Archive 6.13 REQUIRED)
find_package(KF6I18n 6.13 REQUIRED)
find_package(KF6CoreAddons 6.13 REQUIRED)
find_package(Okular6 CONFIG REQUIRED)

# Mixed SDK exports can contain different Core ABIs for Debug and NOCONFIG.
# Loading both into the viewer breaks Core's singleton state. Require an
# explicit selection rather than silently choosing by this plugin's build type.
set(OKULAR_OFD_CORE_CONFIGURATION "" CACHE STRING "Okular SDK configuration matching the host (e.g. NOCONFIG)")
get_target_property(_ofd_core_configs Okular::Core IMPORTED_CONFIGURATIONS)
set(_ofd_core_sonames)
foreach(_ofd_config IN LISTS _ofd_core_configs)
    get_target_property(_ofd_soname Okular::Core "IMPORTED_SONAME_${_ofd_config}")
    if(_ofd_soname)
        list(APPEND _ofd_core_sonames "${_ofd_soname}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _ofd_core_sonames)
list(LENGTH _ofd_core_sonames _ofd_core_abi_count)
if(OKULAR_OFD_CORE_CONFIGURATION)
    string(TOUPPER "${OKULAR_OFD_CORE_CONFIGURATION}" _ofd_selected_config)
    if(NOT _ofd_selected_config IN_LIST _ofd_core_configs)
        message(FATAL_ERROR "Unknown Okular SDK configuration: ${_ofd_selected_config}. Available: ${_ofd_core_configs}")
    endif()
    set_property(TARGET Okular::Core PROPERTY IMPORTED_CONFIGURATIONS "${_ofd_selected_config}")
    set(_ofd_build_configs DEBUG RELEASE RELWITHDEBINFO MINSIZEREL ${CMAKE_CONFIGURATION_TYPES} "${CMAKE_BUILD_TYPE}")
    foreach(_ofd_config IN LISTS _ofd_build_configs)
        string(TOUPPER "${_ofd_config}" _ofd_config)
        set_property(TARGET Okular::Core PROPERTY "MAP_IMPORTED_CONFIG_${_ofd_config}" "${_ofd_selected_config}")
    endforeach()
    get_target_property(_ofd_core_location Okular::Core "IMPORTED_LOCATION_${_ofd_selected_config}")
    message(STATUS "OFD plugin Okular Core: ${_ofd_core_location} (${_ofd_selected_config})")
elseif(_ofd_core_abi_count GREATER 1)
    message(FATAL_ERROR "Okular SDK exports multiple Core ABIs: ${_ofd_core_sonames}. Set -DOKULAR_OFD_CORE_CONFIGURATION=<configuration> to match the host. Available: ${_ofd_core_configs}")
endif()

add_subdirectory(src)

if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    include(ECMAddTests)
    add_subdirectory(tests)
endif()

install(PROGRAMS data/okularApplication_ofd.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(PROGRAMS data/org.kde.mobile.okular_ofd.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES data/org.kde.okular-ofd.metainfo.xml DESTINATION ${KDE_INSTALL_METAINFODIR})
install(FILES data/okular-ofd-mime.xml DESTINATION ${KDE_INSTALL_MIMEDIR})

configure_file(cmake/UpdateMimeDatabase.cmake.in UpdateMimeDatabase.cmake @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/UpdateMimeDatabase.cmake")
