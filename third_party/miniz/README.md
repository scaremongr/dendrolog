# miniz

Сторонняя библиотека: inflate/deflate, CRC-32 и чтение zip (с zip64) одним
файлом на C. Используется ядром для сжатых источников (F-2, `docs/design/F-2.md`):
`GzipDevice` берёт из неё только raw inflate и CRC-32 — заголовок, хвост и
склейку потоков gzip разбирает свой код.

| | |
| --- | --- |
| Версия | 3.0.2 (`MZ_VERSION` 11.0.2) |
| Источник | https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip |
| SHA-256 архива | `ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5` |
| Лицензия | MIT (`LICENSE` рядом — из архива без изменений) |
| Взяты файлы | `miniz.c`, `miniz.h`, `LICENSE` — байт в байт, без правок |

Сборка — `CMakeLists.txt` рядом: статическая цель `miniz`, приватно у
`dendrolog_core`. Предупреждения чужого кода гасятся флагами цели (`-w`,
`/W0`), а не правкой исходников; `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` убирает
макросы-синонимы имён zlib (`crc32`, `inflate`, …). В `.gitattributes` файлы
помечены `-text`, чтобы `core.autocrlf` не менял их байты.

Обновление: скачать архив релиза, сверить SHA-256 с опубликованным, заменить
три файла, записать здесь версию и хеш, прогнать `tests/unpack` (в том числе
под ASan и TSan) и замер `unpack --bench`.
