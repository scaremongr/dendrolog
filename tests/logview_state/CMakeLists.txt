cmake_minimum_required(VERSION 3.16)
project(LogViewState CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)
add_executable(logview_state main.cpp
    ../../src/LogListView.cpp
    ../../src/logmodel.cpp
    ../../src/residentlogstore.cpp
    ../../src/indexedlogstore.cpp
    ../../src/textchunkcache.cpp
    ../../src/logscan.cpp
    ../../src/filterruleset.cpp
    ../../src/textmatchhighlighter.cpp
    ../../src/syntaxhighlighter.cpp
    ../../src/texttoken.cpp
    ../../src/apptheme.cpp
    ../../src/appsettings.cpp
    ../../src/lineindex.cpp
    ../../src/logindexer.cpp
    ../../src/lineclassifier.cpp
    ../../src/logparser.cpp
    ../../src/logfile.cpp
    ../../src/logpattern.cpp
    ../../src/patternheuristics.cpp)
target_include_directories(logview_state PRIVATE ../../src ../common)
target_link_libraries(logview_state PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent)

add_test(NAME logview_state COMMAND logview_state)
set_tests_properties(logview_state PROPERTIES TIMEOUT 900)
# На Windows тестовому exe нужны Qt DLL; добавляем каталог Qt bin в PATH теста.
if(WIN32)
    get_target_property(_qt_core_loc Qt6::Core LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
    set_tests_properties(logview_state PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qt_bin_dir}")
endif()
