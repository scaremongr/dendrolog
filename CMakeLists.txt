cmake_minimum_required(VERSION 3.16)

project(DendroLog VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Автоматическая обработка MOC, UIC и RCC
set(CMAKE_AUTOMOC ON)    # Добавляем автоматическую обработку MOC
set(CMAKE_AUTORCC ON)    # Для обработки .qrc файлов
set(CMAKE_AUTOUIC ON)    # Для обработки .ui файлов (если они есть)

# Найти Qt 6
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui Concurrent Svg Network)

# Добавить исходные файлы
set(PROJECT_SOURCES
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/mainwindow.ui

    src/logviewwidget.h
    src/logviewwidget.cpp

    src/logtabbar.h
    src/logtabbar.cpp
    src/logtabwidget.h
    src/logtabwidget.cpp

    src/logentry.h
    src/logfield.h
    src/logfile.h
    src/logfile.cpp
    src/logmodel.h
    src/logmodel.cpp
    src/logstore.h
    src/residentlogstore.h
    src/residentlogstore.cpp
    src/indexedlogstore.h
    src/indexedlogstore.cpp
    src/textchunkcache.h
    src/textchunkcache.cpp
    src/sequentiallinereader.h
    src/stdinspooler.h
    src/stdinspooler.cpp
    src/logscan.h
    src/logscan.cpp
    src/lineclassifier.h
    src/lineclassifier.cpp
    src/lineindex.h
    src/lineindex.cpp
    src/logindexer.h
    src/logindexer.cpp
    src/logparser.h
    src/logparser.cpp
    src/filechangedetector.h
    src/filechangedetector.cpp
    src/logpattern.h
    src/logpattern.cpp
    src/patternheuristics.h
    src/patternheuristics.cpp
    src/schemastore.h
    src/schemastore.cpp

    src/conversionpatterndialog.h
    src/conversionpatterndialog.cpp
    src/conversionpatterndialog.ui
    src/patternblockcard.h
    src/patternblockcard.cpp
    src/separatornode.h
    src/separatornode.cpp

    src/apptheme.cpp

    src/appsettings.h
    src/appsettings.cpp

    src/shortcutmanager.h
    src/shortcutmanager.cpp

    src/settingsdialog.h
    src/settingsdialog.cpp
    src/settingsdialog.ui

    src/directoryscanner.h
    src/directoryscanner.cpp
    src/directoryscannerpanel.h
    src/directoryscannerpanel.cpp

    src/LogListView.h
    src/LogListView.cpp
    src/tabexpander.h
    src/linewrapper.h

    src/texttoken.h
    src/texttoken.cpp
    src/syntaxhighlighter.h
    src/syntaxhighlighter.cpp

    src/cardframe.h
    src/cardframe.cpp
    src/toggleswitch.h
    src/toggleswitch.cpp
    src/textmatchhighlighter.h
    src/textmatchhighlighter.cpp
    src/filterruleset.h
    src/filterruleset.cpp
    src/highlightpalette.h
    src/filterpanelwidget.h
    src/filterpanelwidget.cpp
    src/markerpanelwidget.h
    src/markerpanelwidget.cpp
    src/entrydetailspanel.h
    src/entrydetailspanel.cpp
    src/statisticspanel.h
    src/statisticspanel.cpp
    src/timelinehistogramwidget.h
    src/timelinehistogramwidget.cpp

    src/singleinstance.h
    src/singleinstance.cpp
    src/updatechecker.h
    src/updatechecker.cpp

    resources/resources.qrc
)

# Иконка и метаданные версии для проводника Windows. Версия подставляется
# из project(VERSION), поэтому .rc генерируется при конфигурации.
if(WIN32)
    set(DENDROLOG_ICO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/dendrolog.ico")
    set(DENDROLOG_DOC_ICO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/dendrolog-log.ico")
    configure_file(resources/windows/dendrolog.rc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/dendrolog.rc @ONLY)
    # .rc не пересобирается сам при правке .ico — укажем их явной зависимостью.
    set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/dendrolog.rc APPEND PROPERTY
                 OBJECT_DEPENDS "${DENDROLOG_ICO_PATH};${DENDROLOG_DOC_ICO_PATH}")
    list(APPEND PROJECT_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/dendrolog.rc)
endif()

qt_add_executable(${PROJECT_NAME} WIN32 ${PROJECT_SOURCES})

# Подключение Qt модулей
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Core
    Qt6::Gui
    Qt6::Concurrent
    Qt6::Svg
    Qt6::Network
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

# Версия приложения — единственный источник правды здесь, в project(VERSION).
target_compile_definitions(${PROJECT_NAME} PRIVATE
    DENDROLOG_VERSION="${PROJECT_VERSION}"
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests/pattern_smoke)
    add_subdirectory(tests/lineindex_smoke)
endif()
