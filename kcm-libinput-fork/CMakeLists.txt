cmake_minimum_required(VERSION 3.20)

project(libinputfork-kcm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
set(QT_MIN_VERSION 6.6.0)
set(KF6_MIN_VERSION 6.0.0)

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Widgets DBus)
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS
    CoreAddons
    Config
    ConfigWidgets
    I18n
    KCMUtils
)

ki18n_wrap_ui(libinputfork-kcm_SRCS ui/main.ui)

add_library(kcm_libinputfork MODULE
    src/kcm.cpp
    src/kcm.h
    ${libinputfork-kcm_SRCS}
)

target_link_libraries(kcm_libinputfork PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::DBus
    KF6::CoreAddons
    KF6::ConfigCore
    KF6::ConfigWidgets
    KF6::I18n
    KF6::KCMUtils
)

install(TARGETS kcm_libinputfork DESTINATION ${KDE_INSTALL_PLUGINDIR}/plasma/kcms/systemsettings)

install(FILES kcm_libinputfork.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

install(FILES src/kcm_libinputfork.json
    DESTINATION ${KDE_INSTALL_DATADIR}/plasma/kcms/systemsettings
)

# Helper that reconciles the settings file with the running compositor.
install(PROGRAMS scripts/libinput-fork-apply
    DESTINATION ${KDE_INSTALL_BINDIR}
)
