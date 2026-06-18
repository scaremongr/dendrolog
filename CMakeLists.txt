cmake_minimum_required(VERSION 3.16)

project(LogViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Автоматическая обработка MOC, UIC и RCC
set(CMAKE_AUTOMOC ON)    # Добавляем автоматическую обработку MOC
set(CMAKE_AUTORCC ON)    # Для обработки .qrc файлов
set(CMAKE_AUTOUIC ON)    # Для обработки .ui файлов (если они есть)

# Найти Qt 6
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Core5Compat Gui Concurrent Svg)

# Добавить исходные файлы
set(PROJECT_SOURCES
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/mainwindow.ui
    
    src/logviewwidget.h
    src/logviewwidget.cpp
    
    src/logentry.h
    src/logfield.h
    src/logfile.h
    src/logfile.cpp
    src/logmodel.h
    src/logmodel.cpp
    src/logparser.h
    src/logparser.cpp
    src/filechangedetector.h
    src/filechangedetector.cpp
    src/logpattern.h
    src/logpattern.cpp
    src/patternheuristics.h
    src/patternheuristics.cpp

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
    src/textmatchhighlighter.h
    src/textmatchhighlighter.cpp
    src/filterruleset.h
    src/filterruleset.cpp
    src/highlightpalette.h
    src/filterpanelwidget.h
    src/filterpanelwidget.cpp
    src/markerpanelwidget.h
    src/markerpanelwidget.cpp
    resources/resources.qrc
)

qt_add_executable(${PROJECT_NAME} WIN32 ${PROJECT_SOURCES})

# Подключение Qt модулей
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Core
    Qt6::Core5Compat
    Qt6::Gui
    Qt6::Concurrent
    Qt6::Svg
)

target_include_directories(${PROJECT_NAME} PRIVATE src)