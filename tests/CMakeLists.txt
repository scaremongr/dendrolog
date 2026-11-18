# Тесты без окна. Каждый каталог — один исполняемый файл из своего main.cpp,
# слинкованный с библиотеками приложения (dendrolog_core / dendrolog_widgets);
# исходники приложения тесты сами не компилируют.

# На Windows тестовому exe нужны Qt DLL — каталог Qt bin идёт в PATH теста.
if(WIN32)
    get_target_property(_qt_core_loc Qt6::Core LOCATION)
    get_filename_component(DENDROLOG_QT_BIN_DIR "${_qt_core_loc}" DIRECTORY)
endif()

# Прогон под санитайзером (DENDROLOG_SANITIZER в корневом CMakeLists.txt):
# опции рантайма и файлы подавлений — через окружение теста, таймауты
# умножаются. Qt не инструментирован, поэтому TSan замедляет тесты в разы
# сильнее, чем ASan.
set(DENDROLOG_SANITIZER_ENV "")
set(DENDROLOG_TEST_TIMEOUT_FACTOR 1)
set(_supp_dir "${CMAKE_CURRENT_SOURCE_DIR}/sanitizers")
if(DENDROLOG_SANITIZER STREQUAL "address")
    set(DENDROLOG_TEST_TIMEOUT_FACTOR 4)
    set(DENDROLOG_SANITIZER_ENV
        "ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1"
        "LSAN_OPTIONS=suppressions=${_supp_dir}/lsan.supp:print_suppressions=0"
        # Сборка оставляет UBSan «восстановимым», иначе рантайм не смотрит в
        # подавления; падать на первой находке велит halt_on_error.
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=${_supp_dir}/ubsan.supp")
elseif(DENDROLOG_SANITIZER STREQUAL "thread")
    set(DENDROLOG_TEST_TIMEOUT_FACTOR 10)
    set(DENDROLOG_SANITIZER_ENV
        "TSAN_OPTIONS=suppressions=${_supp_dir}/tsan.supp:print_suppressions=0:second_deadlock_stack=1")
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
        math(EXPR _timeout "${ARG_TIMEOUT} * ${DENDROLOG_TEST_TIMEOUT_FACTOR}")
        set_tests_properties(${test_name} PROPERTIES TIMEOUT ${_timeout})
    endif()
    if(DENDROLOG_SANITIZER_ENV)
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "${DENDROLOG_SANITIZER_ENV}")
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
add_subdirectory(quick_search_field)
add_subdirectory(logview_lifecycle)
add_subdirectory(goto_location)
add_subdirectory(level_jump)
add_subdirectory(theme_mode)
add_subdirectory(open_routing)

# Проверка самой проверки: слой tests/sanitizers/tsan_qt_sync.cpp не должен ни
# пропускать правильные передачи через Qt как гонки, ни прятать настоящие.
if(DENDROLOG_SANITIZER STREQUAL "thread")
    add_subdirectory(tsan_canary)
endif()
