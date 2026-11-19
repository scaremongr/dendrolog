# miniz — inflate/deflate, CRC-32 и чтение zip (F-2). Исходники — без правок,
# из официального релиза (версия, URL и SHA-256 — в README.md рядом).
#
# Отдельная статическая цель, приватно слинкованная с dendrolog_core: заголовок
# miniz виден только ядру и тем тестам, что линкуют цель сами (tests/unpack
# сжимает входы через miniz). Чужой код не правится — предупреждения гасятся
# флагами цели.
add_library(miniz STATIC miniz.c miniz.h)
target_include_directories(miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Qt здесь нет: без AUTOMOC цель не получает пустого mocs_compilation.cpp
# (C++ без путей Qt — а сборка под TSan подключает в каждый C++-файл
# заголовок Qt, tests/sanitizers/tsan_qt_refcount.h).
set_target_properties(miniz PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# Макросы-синонимы имён zlib (inflate, crc32, …) не нужны и опасны: Qt тянет
# свой или системный zlib, а макрос crc32 переписал бы чужой код.
target_compile_definitions(miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
if(MSVC)
    target_compile_options(miniz PRIVATE /W0)
    # fopen/strcpy и т. п. в чужом коде — без предупреждений CRT.
    target_compile_definitions(miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(miniz PRIVATE -w)
endif()
