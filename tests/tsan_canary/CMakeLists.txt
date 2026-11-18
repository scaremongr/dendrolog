# Два режима одного exe (см. main.cpp): clean обязан пройти без отчётов TSan,
# race обязан дать отчёт о гонке именно в racyGuiWrite/racyWorkerWrite — строку
# SUMMARY с этими именами CTest ищет в выводе (код возврата при этом не важен).
add_executable(tsan_canary main.cpp)
target_link_libraries(tsan_canary PRIVATE dendrolog_core)

add_test(NAME tsan_canary_clean COMMAND tsan_canary clean)
add_test(NAME tsan_canary_race COMMAND tsan_canary race)
set_tests_properties(tsan_canary_clean tsan_canary_race PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "${DENDROLOG_SANITIZER_ENV}")
set_tests_properties(tsan_canary_race PROPERTIES
    PASS_REGULAR_EXPRESSION "ThreadSanitizer: data race [^\n]* in racy(Gui|Worker)Write")
