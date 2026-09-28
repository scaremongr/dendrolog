# Тесты без окна. Каждый каталог — один исполняемый файл из своего main.cpp,
# слинкованный с библиотеками приложения (dendrolog_core / dendrolog_widgets);
# исходники приложения тесты сами не компилируют.

# На Windows тестовому exe нужны Qt DLL — каталог Qt bin идёт в PATH теста.
if(WIN32)
    get_target_property(_qt_core_loc Qt6::Core LOCATION)
    get_filename_component(DENDROLOG_QT_BIN_DIR "${_qt_core_loc}" DIRECTORY)
endif()

# dendrolog_add_test(<имя теста> LIBRARIES <цели>...
#                    [TARGET <имя exe>] [TIMEOUT <секунды>])
# Исходник — main.cpp в каталоге, откуда вызвана функция; имя exe по
# умолчанию совпадает с именем теста.
function(dendrolog_add_test test_name)
    cmake_parse_arguments(ARG "" "TARGET;TIMEOUT" "LIBRARIES" ${ARGN})
    if(NOT ARG_TARGET)
        set(ARG_TARGET ${test_name})
    endif()

    add_executable(${ARG_TARGET} main.cpp)
    target_include_directories(${ARG_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/tests/common)
    target_link_libraries(${ARG_TARGET} PRIVATE ${ARG_LIBRARIES})

    add_test(NAME ${test_name} COMMAND ${ARG_TARGET})
    if(ARG_TIMEOUT)
        set_tests_properties(${test_name} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
    endif()
    if(WIN32)
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${DENDROLOG_QT_BIN_DIR}")
    endif()
endfunction()

add_subdirectory(pattern_smoke)
add_subdirectory(lineindex_smoke)
add_subdirectory(dirscanner_smoke)
add_subdirectory(logstore_equivalence)
add_subdirectory(logview_state)
add_subdirectory(viewexport)
add_subdirectory(search_results)
add_subdirectory(field_reextraction)
add_subdirectory(quick_search)
