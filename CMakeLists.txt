cmake_minimum_required(VERSION 3.16)

project(DendroLog VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Автоматическая обработка MOC, UIC и RCC
set(CMAKE_AUTOMOC ON)    # Добавляем автоматическую обработку MOC
set(CMAKE_AUTORCC ON)    # Для обработки .qrc файлов
set(CMAKE_AUTOUIC ON)    # Для обработки .ui файлов (если они есть)

# Найти Qt 6
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui Concurrent Svg Network)

# Исходники разложены по трём целям — каждый файл ровно в одной из них:
#   dendrolog_core    — данные, разбор, хранилище, модель, экспорт. Без Qt Widgets;
#   dendrolog_widgets — представление лога, тема, настройки, сканер каталога и
#                       общие элементы UI (всё, что тестам нужно из виджетов);
#   DendroLog         — оболочка приложения: окно, вкладки, панели, диалоги.
# Библиотеки статические и общие для приложения и тестов: каждый исходник
# компилируется один раз, а тест линкует только то, чем пользуется.

set(DENDROLOG_CORE_SOURCES
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
    src/filterruleset.h
    src/filterruleset.cpp
    src/textmatchhighlighter.h
    src/textmatchhighlighter.cpp
    src/viewexport.h
    src/viewexport.cpp
    src/searchresultscontroller.h
    src/searchresultscontroller.cpp
    src/fieldreextraction.h
    src/fieldreextraction.cpp
    src/quicksearch.h
    src/quicksearch.cpp
)

set(DENDROLOG_WIDGETS_SOURCES
    src/LogListView.h
    src/LogListView.cpp
    src/logviewwidget.h
    src/logviewwidget.cpp
    src/tabexpander.h
    src/linewrapper.h
    src/texttoken.h
    src/texttoken.cpp
    src/syntaxhighlighter.h
    src/syntaxhighlighter.cpp

    src/apptheme.h
    src/apptheme.cpp
    src/appsettings.h
    src/appsettings.cpp

    src/directoryscanner.h
    src/directoryscanner.cpp
    src/directoryscannerpanel.h
    src/directoryscannerpanel.cpp

    src/cardframe.h
    src/cardframe.cpp
    src/compactstyle.h
    src/compactstyle.cpp
    src/toggleswitch.h
    src/toggleswitch.cpp
    src/highlightpalette.h
)

set(PROJECT_SOURCES
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/mainwindow.ui

    src/logtabbar.h
    src/logtabbar.cpp
    src/logtabwidget.h
    src/logtabwidget.cpp
    src/welcomewidget.h
    src/welcomewidget.cpp

    src/schemastore.h
    src/schemastore.cpp
    src/conversionpatterndialog.h
    src/conversionpatterndialog.cpp
    src/conversionpatterndialog.ui
    src/patternblockcard.h
    src/patternblockcard.cpp
    src/separatornode.h
    src/separatornode.cpp

    src/shortcutmanager.h
    src/shortcutmanager.cpp

    src/settingsdialog.h
    src/settingsdialog.cpp
    src/settingsdialog.ui

    src/filterpanelwidget.h
    src/filterpanelwidget.cpp
    src/markerpanelwidget.h
    src/markerpanelwidget.cpp
    src/searchpanelwidget.h
    src/searchpanelwidget.cpp
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

add_library(dendrolog_core STATIC ${DENDROLOG_CORE_SOURCES})
target_include_directories(dendrolog_core PUBLIC src)
target_link_libraries(dendrolog_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Concurrent
)

add_library(dendrolog_widgets STATIC ${DENDROLOG_WIDGETS_SOURCES})
target_link_libraries(dendrolog_widgets
    PUBLIC
        dendrolog_core
        Qt6::Widgets
    PRIVATE
        Qt6::Svg
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

# Ресурсы (.qrc) остаются в исполняемом файле: из статической библиотеки их
# регистрирующий инициализатор линкер бы выбросил.
qt_add_executable(${PROJECT_NAME} WIN32 ${PROJECT_SOURCES})

# Подключение Qt модулей
target_link_libraries(${PROJECT_NAME} PRIVATE
    dendrolog_widgets
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
    add_subdirectory(tests)
endif()
