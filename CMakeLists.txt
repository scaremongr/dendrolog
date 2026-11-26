cmake_minimum_required(VERSION 3.16)

project(DendroLog VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Санитайзеры (GCC/Clang): -DDENDROLOG_SANITIZER=address — ASan + UBSan,
# -DDENDROLOG_SANITIZER=thread — TSan. Флаги ставятся на весь проект, включая
# тесты; окружение прогона (опции рантайма, файлы подавлений из
# tests/sanitizers/, увеличенные таймауты) задаёт tests/CMakeLists.txt.
# По умолчанию выключено: обычная сборка и упаковка флагов не получают.
set(DENDROLOG_SANITIZER "" CACHE STRING
    "Собрать с санитайзером: address (ASan + UBSan) или thread (TSan)")
set_property(CACHE DENDROLOG_SANITIZER PROPERTY STRINGS "" address thread)
if(DENDROLOG_SANITIZER)
    if(MSVC)
        message(FATAL_ERROR "DENDROLOG_SANITIZER поддерживается только с GCC и Clang")
    endif()
    if(DENDROLOG_SANITIZER STREQUAL "address")
        set(_dendrolog_sanitize_flags -fsanitize=address,undefined)
    elseif(DENDROLOG_SANITIZER STREQUAL "thread")
        set(_dendrolog_sanitize_flags -fsanitize=thread)
    else()
        message(FATAL_ERROR
            "DENDROLOG_SANITIZER=${DENDROLOG_SANITIZER}: ожидается address или thread")
    endif()
    # Кадры стека и отладочная информация нужны для читаемых отчётов и для
    # того, чтобы подавления по имени функции находили свои кадры.
    add_compile_options(${_dendrolog_sanitize_flags} -fno-omit-frame-pointer -g)
    if(DENDROLOG_SANITIZER STREQUAL "thread")
        # Счётчик ссылок контейнеров Qt читается relaxed, и TSan не видит, что
        # запись «на месте» идёт после чтения воркером, отпустившим свою копию.
        # Заголовок подключается во все единицы трансляции раньше любого
        # заголовка Qt — подробности в нём самом.
        add_compile_options(
            "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/tests/sanitizers/tsan_qt_refcount.h")
    endif()
    add_link_options(${_dendrolog_sanitize_flags})
    message(STATUS "DendroLog: сборка с санитайзером '${DENDROLOG_SANITIZER}'")
endif()

# Автоматическая обработка MOC, UIC и RCC
set(CMAKE_AUTOMOC ON)    # Добавляем автоматическую обработку MOC
set(CMAKE_AUTORCC ON)    # Для обработки .qrc файлов
set(CMAKE_AUTOUIC ON)    # Для обработки .ui файлов (если они есть)

# Найти Qt 6
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui Concurrent Svg Network)

# Исходники разложены по четырём целям — каждый файл ровно в одной из них
# (плюс сторонний miniz — своей целью в third_party/miniz):
#   dendrolog_core    — данные, разбор, хранилище, модель, экспорт. Без Qt Widgets;
#   dendrolog_widgets — представление лога, тема, настройки, сканер каталога и
#                       общие элементы UI (всё, что тестам нужно из виджетов);
#   dendrolog_app     — оболочка: окно, вкладки, диалоги, один экземпляр,
#                       проверка обновлений (её создаёт tests/mainwindow_smoke);
#   DendroLog         — точка входа (main.cpp) и ресурсы.
# Библиотеки статические и общие для приложения и тестов: каждый исходник
# компилируется один раз, а тест линкует только то, чем пользуется.

set(DENDROLOG_CORE_SOURCES
    src/logentry.h
    src/logfield.h
    src/logfile.h
    src/logfile.cpp
    src/gzipdevice.h
    src/gzipdevice.cpp
    src/spooldir.h
    src/spooldir.cpp
    src/sourceunpacker.h
    src/sourceunpacker.cpp
    src/ziparchive.h
    src/ziparchive.cpp
    src/tararchive.h
    src/tararchive.cpp
    src/archiveregistry.h
    src/archiveregistry.cpp
    src/logmodel.h
    src/logmodel.cpp
    src/logstore.h
    src/recordid.h
    src/shortcount.h
    src/filelabels.h
    src/filelabels.cpp
    src/recordid.cpp
    src/recordlocator.h
    src/recordlocator.cpp
    src/bookmarkstore.h
    src/bookmarkstore.cpp
    src/locatedplaces.h
    src/locatedplaces.cpp
    src/documentstate.h
    src/documentstate.cpp
    src/filememorystore.h
    src/filememorystore.cpp
    src/sessionfile.h
    src/sessionfile.cpp
    src/navhistory.h
    src/navhistory.cpp
    src/residentlogstore.h
    src/residentlogstore.cpp
    src/indexedlogstore.h
    src/indexedlogstore.cpp
    src/textchunkcache.h
    src/textchunkcache.cpp
    src/sequentiallinereader.h
    src/bytelinereader.h
    src/stdinspooler.h
    src/stdinspooler.cpp
    src/commandspooler.h
    src/commandspooler.cpp
    src/logscan.h
    src/logscan.cpp
    src/timehistogram.h
    src/timehistogram.cpp
    src/timegaps.h
    src/timegaps.cpp
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
    src/keyedline.h
    src/keyedline.cpp
    src/logpattern.h
    src/logpattern.cpp
    src/patternheuristics.h
    src/patternheuristics.cpp
    src/schemafit.h
    src/schemafit.cpp
    src/fieldextractor.h
    src/fieldextractor.cpp
    src/linetemplate.h
    src/linetemplate.cpp
    src/schemaconverter.h
    src/schemaconverter.cpp
    src/schemainference.h
    src/schemainference.cpp
    src/schemaproposal.h
    src/schemaproposal.cpp
    # Системный текст нейро-подсказчика (F-17, шаг 5): ресурс ядра,
    # Q_INIT_RESOURCE(ai) — в schemaproposal.cpp.
    resources/ai/ai.qrc
    src/filterruleset.h
    src/filterruleset.cpp
    src/textmatchhighlighter.h
    src/textmatchhighlighter.cpp
    src/viewexport.h
    src/viewexport.cpp
    src/searchresultscontroller.h
    src/searchresultscontroller.cpp
    src/fieldlayout.h
    src/fieldlayout.cpp
    src/fieldsource.h
    src/fieldsource.cpp
    src/quicksearch.h
    src/quicksearch.cpp
    src/scrollbarmarks.h
    src/scrollbarmarks.cpp
    src/quicksearchhistory.h
    src/quicksearchhistory.cpp
    src/textquery.h
    src/textquery.cpp
    src/gotolocation.h
    src/gotolocation.cpp
    src/timedelta.h
    src/timedelta.cpp
    src/builtinschemas.h
    src/builtinschemas.cpp
    src/openrouting.h
    src/openrouting.cpp
    src/textencoding.h
    src/textencoding.cpp
    src/ansiescape.h
    src/ansiescape.cpp
    src/filecontentsearch.h
    src/filecontentsearch.cpp
    src/userregex.h
    src/userregex.cpp
)

set(DENDROLOG_WIDGETS_SOURCES
    src/LogListView.h
    src/LogListView.cpp
    src/newlinesindicator.h
    src/newlinesindicator.cpp
    src/filelegend.h
    src/filelegend.cpp
    src/encodingindicator.h
    src/encodingindicator.cpp
    src/commandoutputdialog.h
    src/commandoutputdialog.cpp
    src/commandstatusbar.h
    src/commandstatusbar.cpp
    src/scrollbarmarksoverlay.h
    src/scrollbarmarksoverlay.cpp
    src/logviewwidget.h
    src/logviewwidget.cpp
    src/fulllogcontroller.h
    src/fulllogcontroller.cpp
    src/gotodialog.h
    src/gotodialog.cpp
    src/quicksearchfield.h
    src/quicksearchfield.cpp
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
    src/shortcutmanager.h
    src/shortcutmanager.cpp
    src/colorschemecontroller.h
    src/colorschemecontroller.cpp

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

    # Панели и стартовый экран: в библиотеке ради tests/theme_mode
    # (перекраска при смене темы).
    src/welcomewidget.h
    src/welcomewidget.cpp
    src/filterpanelwidget.h
    src/filterpanelwidget.cpp
    src/markerpanelwidget.h
    src/markerpanelwidget.cpp
    src/bookmarkpanelwidget.h
    src/bookmarkpanelwidget.cpp
    src/sessioncontroller.h
    src/sessioncontroller.cpp
    src/fieldschemacontroller.h
    src/fieldschemacontroller.cpp
    src/sessionreportdialog.h
    src/sessionreportdialog.cpp
    src/searchresultsnavigator.h
    src/searchresultsnavigator.cpp
    src/entrydetailspanel.h
    src/entrydetailspanel.cpp
    src/statisticspanel.h
    src/statisticspanel.cpp

    # Каталог схем и редактор схем: в библиотеке ради tests/schema_store
    # (схема с описанием проходит через редактор и сохранение).
    src/schemastore.h
    src/schemastore.cpp
    src/conversionpatterndialog.h
    src/conversionpatterndialog.cpp
    src/conversionpatterndialog.ui
    src/patternblockcard.h
    src/patternblockcard.cpp
    src/separatornode.h
    src/separatornode.cpp
)

# Оболочка окна: в статической библиотеке, а не в exe, чтобы окно целиком
# создавал тест (tests/mainwindow_smoke). Отдельно от dendrolog_widgets — ей
# нужен Qt Network (проверка обновлений, один экземпляр), остальным тестам нет.
set(DENDROLOG_APP_SOURCES
    src/mainwindow.h
    src/mainwindow.cpp
    src/mainwindow.ui

    src/logtabbar.h
    src/logtabbar.cpp
    src/logtabwidget.h
    src/logtabwidget.cpp

    src/settingsdialog.h
    src/settingsdialog.cpp
    src/settingsdialog.ui

    src/searchpanelwidget.h
    src/searchpanelwidget.cpp
    src/timelinehistogramwidget.h
    src/timelinehistogramwidget.cpp

    src/singleinstance.h
    src/singleinstance.cpp
    src/updatechecker.h
    src/updatechecker.cpp

    # Нейро-подсказчик и окно подбора схемы (F-17, шаг 5).
    src/aischemaclient.h
    src/aischemaclient.cpp
    src/secretstore.h
    src/secretstore.cpp
    src/schemacandidates.h
    src/schemacandidates.cpp
    src/schemacandidateswindow.h
    src/schemacandidateswindow.cpp
    src/aiconsentdialog.h
    src/aiconsentdialog.cpp
)

# В exe — только точка входа и ресурсы.
set(PROJECT_SOURCES
    src/main.cpp
    resources/resources.qrc
)

# Сторонний код (F-2): miniz — inflate и чтение zip. Своя статическая цель,
# приватно у ядра: заголовок miniz виден только ядру (и тестам, что линкуют
# цель сами), флаги предупреждений — только у неё.
add_subdirectory(third_party/miniz)

add_library(dendrolog_core STATIC ${DENDROLOG_CORE_SOURCES})
target_include_directories(dendrolog_core PUBLIC src)
target_link_libraries(dendrolog_core
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Concurrent
    PRIVATE
        miniz
)

# TSan не видит мьютексы неинструментированного Qt. Обёртки над вызовами в Qt
# (линкер, --wrap) сообщают ему о передаче данных через очередь событий, пул
# потоков и QFuture — подробности в tests/sanitizers/tsan_qt_sync.cpp. Список
# символов здесь и обёртки там должны совпадать.
if(DENDROLOG_SANITIZER STREQUAL "thread")
    # Имена символов ниже — манглинг Qt 6.8 (QThreadPool::waitForDone с
    # QDeadlineTimer появился в 6.8). Со старшей или младшей версией список
    # нужно сверить, иначе обёртки молча перестанут срабатывать.
    if(Qt6_VERSION VERSION_LESS 6.8)
        message(FATAL_ERROR
            "DENDROLOG_SANITIZER=thread: нужен Qt 6.8+, найден ${Qt6_VERSION} "
            "(см. tests/sanitizers/tsan_qt_sync.cpp)")
    endif()
    target_sources(dendrolog_core PRIVATE tests/sanitizers/tsan_qt_sync.cpp)
    set(_tsan_wrapped_symbols
        _ZN11QMetaObject8activateEP7QObjectPKS_iPPv            # QMetaObject::activate
        _ZN16QCoreApplication9postEventEP7QObjectP6QEventi     # QCoreApplication::postEvent
        _ZN11QThreadPool5startEP9QRunnablei                    # QThreadPool::start
        _ZN11QThreadPool11waitForDoneE14QDeadlineTimer         # QThreadPool::waitForDone
        _ZN12QtConcurrent16ThreadEngineBase11startThreadEv     # ThreadEngineBase::startThread
        _ZN12QtConcurrent16ThreadEngineBase3runEv              # ThreadEngineBase::run
        _ZNK20QFutureInterfaceBase10isCanceledEv               # QFutureInterfaceBase::isCanceled
        _ZN20QFutureInterfaceBase14reportFinishedEv            # QFutureInterfaceBase::reportFinished
        _ZN20QFutureInterfaceBase18reportResultsReadyEii       # QFutureInterfaceBase::reportResultsReady
        _ZN20QFutureInterfaceBase15waitForFinishedEv           # QFutureInterfaceBase::waitForFinished
        _ZN20QFutureInterfaceBase13waitForResultEi             # QFutureInterfaceBase::waitForResult
        _ZNK20QFutureInterfaceBase6derefTEv                    # QFutureInterfaceBase::derefT
        _ZN20QFutureInterfaceBaseD1Ev                          # QFutureInterfaceBase::~QFutureInterfaceBase
        _ZN20QFutureInterfaceBaseD2Ev                          # (базовый деструктор, из ~QFutureInterface<T>)
        _ZN18QFutureWatcherBase15waitForFinishedEv             # QFutureWatcherBase::waitForFinished
        _ZN7QThread4waitE14QDeadlineTimer                      # QThread::wait
    )
    foreach(_symbol IN LISTS _tsan_wrapped_symbols)
        target_link_options(dendrolog_core PUBLIC "LINKER:--wrap=${_symbol}")
    endforeach()
endif()

add_library(dendrolog_widgets STATIC ${DENDROLOG_WIDGETS_SOURCES})
target_link_libraries(dendrolog_widgets
    PUBLIC
        dendrolog_core
        Qt6::Widgets
    PRIVATE
        Qt6::Svg
)

add_library(dendrolog_app STATIC ${DENDROLOG_APP_SOURCES})
target_link_libraries(dendrolog_app
    PUBLIC
        dendrolog_widgets
        Qt6::Widgets
        Qt6::Network
    PRIVATE
        Qt6::Svg
)
if(WIN32)
    # SecretStore: ключ нейро-подсказчика под DPAPI (CryptProtectData).
    target_link_libraries(dendrolog_app PRIVATE crypt32)
endif()

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
    dendrolog_app
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
