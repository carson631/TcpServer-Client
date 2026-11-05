cmake_minimum_required(VERSION 3.19)
project(TCPServer LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network Sql LinguistTools)

qt_standard_project_setup()

qt_add_executable(TCPServer
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    mythread.cpp
    mythread.h
    mytcpserver.cpp
    mytcpserver.h
    database.cpp
    database.h
)

qt_add_translations(
    TARGETS TCPServer
    TS_FILES TCPServer_zh_CN.ts
)

target_link_libraries(TCPServer
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
        Qt::Sql
)

install(TARGETS TCPServer
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# ---------------------------------------------------------------------------
# Windows 构建后自动部署 MySQL 运行时。
#
# 为什么必须这么做：
#   Qt 的插件（qsqlmysql.dll）是用「受限 DLL 搜索路径」加载的 —— 只查
#   ①插件自身目录 ②exe 所在目录 ③System32，**不查 PATH**。所以哪怕
#   D:\Qt\...\bin 在 PATH 里，只要 exe 旁边没有 libmysql.dll，插件就加载
#   失败，程序只会报 "Driver not loaded"。
#   同理，官方 Qt 的插件目录是靠 bin\qt.conf 里 Prefix=.. 相对于 Qt6Core.dll
#   的位置解析的；一旦 Qt6Core.dll 被拷到别处，插件目录就找不到，连
#   QSQLITE 都不会出现。把插件拷到 exe 旁边的 sqldrivers\ 才是一劳永逸的做法。
# ---------------------------------------------------------------------------
if(WIN32)
    get_target_property(_qt_core_loc Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_core_loc)
        get_target_property(_qt_core_loc Qt6::Core IMPORTED_LOCATION)
    endif()
    get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
    get_filename_component(_qt_prefix  "${_qt_bin_dir}" DIRECTORY)
    set(_qt_sqldrivers "${_qt_prefix}/plugins/sqldrivers")

    set(_mysql_dlls "")
    foreach(_dll libmysql.dll libcrypto-3-x64.dll libssl-3-x64.dll)
        if(EXISTS "${_qt_bin_dir}/${_dll}")
            list(APPEND _mysql_dlls "${_qt_bin_dir}/${_dll}")
        endif()
    endforeach()

    set(_deploy_cmds
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TCPServer>/sqldrivers"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Sql>" "$<TARGET_FILE_DIR:TCPServer>"
    )
    if(EXISTS "${_qt_sqldrivers}/qsqlmysql.dll")
        list(APPEND _deploy_cmds
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_qt_sqldrivers}/qsqlmysql.dll" "$<TARGET_FILE_DIR:TCPServer>/sqldrivers")
    endif()
    if(_mysql_dlls)
        list(APPEND _deploy_cmds
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_mysql_dlls} "$<TARGET_FILE_DIR:TCPServer>")
    endif()

    add_custom_command(TARGET TCPServer POST_BUILD
        ${_deploy_cmds}
        COMMENT "部署 MySQL 运行时（qsqlmysql + libmysql/libcrypto/libssl）到 exe 目录"
    )
endif()

qt_generate_deploy_app_script(
    TARGET TCPServer
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
