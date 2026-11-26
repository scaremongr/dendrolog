# Режимы одного exe (см. main.cpp). clean, cow, future и thread обязаны пройти без
# отчётов TSan (код возврата TSan — 66). race, cow-race, future-race и
# thread-race обязаны дать отчёт о гонке именно на своих данных: режим сам
# считает такие отчёты по адресу (хук __tsan_on_report) и печатает «N report(s)
# on canary data», CTest требует N > 0 (код возврата при этом не важен) — от того, какой доступ
# TSan назвал первым, проверка не зависит. *_blinded — те же режимы с гоночными
# доступами под общим мьютексом (DENDRO_CANARY_BLIND=1): N обязано быть 0.
# clean/race/future/thread проверяют tests/sanitizers/tsan_qt_sync.cpp, cow/cow-race —
# tests/sanitizers/tsan_qt_refcount.h.
add_executable(tsan_canary main.cpp)
target_link_libraries(tsan_canary PRIVATE dendrolog_core)

set(_canary_tests "")
foreach(_mode clean race cow cow-race future future-race thread thread-race)
    string(REPLACE "-" "_" _name "tsan_canary_${_mode}")
    add_test(NAME ${_name} COMMAND tsan_canary ${_mode})
    set_tests_properties(${_name} PROPERTIES ENVIRONMENT "${DENDROLOG_SANITIZER_ENV}")
    list(APPEND _canary_tests ${_name})
endforeach()

foreach(_mode race cow-race future-race thread-race)
    string(REPLACE "-" "_" _name "tsan_canary_${_mode}")
    set_tests_properties(${_name} PROPERTIES
        PASS_REGULAR_EXPRESSION "tsan_canary ${_mode}: [1-9][0-9]* report\\(s\\) on canary data")
    add_test(NAME ${_name}_blinded COMMAND tsan_canary ${_mode})
    set_tests_properties(${_name}_blinded PROPERTIES
        ENVIRONMENT "${DENDROLOG_SANITIZER_ENV};DENDRO_CANARY_BLIND=1"
        PASS_REGULAR_EXPRESSION "tsan_canary ${_mode}: 0 report\\(s\\) on canary data \\(blinded\\)")
    list(APPEND _canary_tests ${_name}_blinded)
endforeach()

set_tests_properties(${_canary_tests} PROPERTIES TIMEOUT 300)
