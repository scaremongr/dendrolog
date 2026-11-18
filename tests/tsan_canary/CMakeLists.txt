# Режимы одного exe (см. main.cpp). clean и cow обязаны пройти без отчётов
# TSan; race и cow-race обязаны дать отчёт о гонке именно в своих функциях
# (racyGuiWrite/racyWorkerWrite, racyListRead/racyListWrite) — строку SUMMARY с
# этими именами CTest ищет в выводе (код возврата при этом не важен).
# clean/race проверяют tests/sanitizers/tsan_qt_sync.cpp, cow/cow-race —
# tests/sanitizers/tsan_qt_refcount.h.
add_executable(tsan_canary main.cpp)
target_link_libraries(tsan_canary PRIVATE dendrolog_core)

add_test(NAME tsan_canary_clean COMMAND tsan_canary clean)
add_test(NAME tsan_canary_race COMMAND tsan_canary race)
add_test(NAME tsan_canary_cow COMMAND tsan_canary cow)
add_test(NAME tsan_canary_cow_race COMMAND tsan_canary cow-race)
set_tests_properties(tsan_canary_clean tsan_canary_race
                     tsan_canary_cow tsan_canary_cow_race PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "${DENDROLOG_SANITIZER_ENV}")
set_tests_properties(tsan_canary_race PROPERTIES
    PASS_REGULAR_EXPRESSION "ThreadSanitizer: data race [^\n]* in racy(Gui|Worker)Write")
set_tests_properties(tsan_canary_cow_race PROPERTIES
    PASS_REGULAR_EXPRESSION "ThreadSanitizer: data race [^\n]* in racyList(Read|Write)")
