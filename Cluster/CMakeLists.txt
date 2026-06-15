cmake_minimum_required(VERSION 3.16)
project(Cluster VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)          # safer than 17 on QCC
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTORCC ON)
set(QML_IMPORT_PATH "${CMAKE_SOURCE_DIR}/qml" CACHE STRING "Extra QML Import Paths" FORCE)

find_package(Qt6 REQUIRED COMPONENTS Quick)
qt_standard_project_setup(REQUIRES 6.9)

# ─── QNX: strip -lrt and -lpthread from Qt's interface libs ───────────────
# On QNX, shm_*, sem_*, pthread_* and clock_* all live in libc — there are
# no separate librt or libpthread to link. Some Qt builds inject them, so
# scrub them out here, AFTER find_package() so the targets exist.
if(QNX)
    foreach(_qt_target IN ITEMS
            Qt6::Core Qt6::Gui Qt6::Qml Qt6::QmlModels
            Qt6::QmlWorkerScript Qt6::Network Qt6::Quick)
        if(TARGET ${_qt_target})
            get_target_property(_libs ${_qt_target} INTERFACE_LINK_LIBRARIES)
            if(_libs)
                list(REMOVE_ITEM _libs rt pthread)
                set_target_properties(${_qt_target} PROPERTIES
                    INTERFACE_LINK_LIBRARIES "${_libs}")
            endif()
        endif()
    endforeach()
endif()

qt_add_executable(appCluster
    main.cpp
)
qt_add_qml_module(appCluster
    URI Cluster

    QML_FILES
        Main.qml

    SOURCES
        cluster.h
        cluster.cpp
        SpiReader.h
        SpiReader.cpp

    RESOURCES
        images.qrc
)

# ─── Force-disable QML AOT cache generation ────────────────────────────────
# The QNX Qt 6.9.3 headers are missing some AOT runtime APIs that the host
# qmlcachegen generates calls to, so disable AOT entirely. QML files will be
# interpreted at runtime — fine for a 10 Hz dashboard.
set_target_properties(appCluster PROPERTIES
    QT_QML_MODULE_NO_CACHEGEN ON
    QT_QMLCACHEGEN_DIRECT_CALLS OFF
)
set_source_files_properties(Main.qml PROPERTIES
    QT_QML_SKIP_CACHEGEN ON
)

set_target_properties(appCluster PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appCluster
    PRIVATE Qt6::Quick
)

# ─── QNX-specific ─────────────────────────────────────────────────────────
if(QNX)
    target_compile_definitions(appCluster PRIVATE __QNXNTO__)

    target_compile_options(appCluster PRIVATE
        -V${QNX_TARGET_ARCH}
        -Wc,-std=c++14
    )

    target_link_libraries(appCluster PRIVATE
        -lsocket
        # No rt, no pthread — those symbols live in libc on QNX
    )

    install(TARGETS appCluster
        RUNTIME DESTINATION /apps/cluster
    )
else()
    install(TARGETS appCluster
        BUNDLE  DESTINATION .
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()