cmake_minimum_required(VERSION 3.16)
project(DirScannerSmoke CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Svg)
add_executable(dirscanner_smoke main.cpp
    ../../src/directoryscanner.cpp
    ../../src/directoryscannerpanel.cpp
    ../../src/cardframe.cpp
    ../../src/appsettings.cpp
    ../../src/apptheme.cpp
    ../../src/lineclassifier.cpp
    ../../src/logparser.cpp
    ../../src/logfile.cpp
    ../../src/logpattern.cpp
    ../../src/patternheuristics.cpp
    ../../src/filechangedetector.cpp)
target_include_directories(dirscanner_smoke PRIVATE ../../src)
target_link_libraries(dirscanner_smoke PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Svg)

add_test(NAME dirscanner_smoke COMMAND dirscanner_smoke)
# На Windows тестовому exe нужны Qt DLL; добавляем каталог Qt bin в PATH теста.
if(WIN32)
    get_target_property(_qt_core_loc Qt6::Core LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
    set_tests_properties(dirscanner_smoke PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qt_bin_dir}")
endif()
