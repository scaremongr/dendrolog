cmake_minimum_required(VERSION 3.16)
project(PatternSmoke CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(pattern_smoke main.cpp ../../src/logpattern.cpp ../../src/patternheuristics.cpp)
target_include_directories(pattern_smoke PRIVATE ../../src)
target_link_libraries(pattern_smoke PRIVATE Qt6::Core)

add_test(NAME pattern_smoke COMMAND pattern_smoke)
# На Windows тестовому exe нужны Qt DLL; добавляем каталог Qt bin в PATH теста.
if(WIN32)
    get_target_property(_qt_core_loc Qt6::Core LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
    set_tests_properties(pattern_smoke PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qt_bin_dir}")
endif()
