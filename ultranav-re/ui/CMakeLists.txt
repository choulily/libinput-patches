cmake_minimum_required(VERSION 3.20)
project(ultranav-ui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(KF6 REQUIRED COMPONENTS I18n CoreAddons)

add_executable(ultranav-settings main.cpp)
target_link_libraries(ultranav-settings PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets KF6::I18n KF6::CoreAddons)

install(TARGETS ultranav-settings DESTINATION ${KDE_INSTALL_BINDIR})
install(FILES ui_tree.json strings_en.json DESTINATION ${KDE_INSTALL_DATADIR}/ultranav)
install(DIRECTORY assets/bitmaps DESTINATION ${KDE_INSTALL_DATADIR}/ultranav/assets)
