@echo off
REM Скрипт для настройки переменных окружения и запуска CMake.
REM Путь к Qt определяется автоматически (см. scripts\find-qt.ps1).
REM Для нестандартной установки задайте переменную окружения QT_PATH вручную:
REM     set QT_PATH=C:\Qt\6.11.1\msvc2022_64

if defined QT_PATH goto :qt_found

echo Поиск установленного Qt...
for /f "usebackq delims=" %%p in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\find-qt.ps1"`) do set "QT_PATH=%%p"

if not defined QT_PATH (
    echo Не удалось найти установку Qt6 ^(кит MSVC x64^).
    echo Установите Qt либо задайте переменную окружения QT_PATH вручную, например:
    echo     set QT_PATH=C:\Qt\6.11.1\msvc2022_64
    pause
    exit /b 1
)

:qt_found
set CMAKE_PREFIX_PATH=%QT_PATH%
echo Путь к Qt установлен: %CMAKE_PREFIX_PATH%

echo.
echo Запуск CMake для конфигурирования проекта...
echo.

REM Создаем директорию build если она не существует
if not exist build mkdir build

REM Запускаем CMake для конфигурирования проекта
cmake -S . -B build

echo.
if %ERRORLEVEL% EQU 0 (
    echo Конфигурирование проекта успешно завершено!
    echo Теперь вы можете выполнить сборку проекта, перейдя в директорию build.
) else (
    echo Произошла ошибка при конфигурировании проекта. Код ошибки: %ERRORLEVEL%
)

REM Пауза для просмотра результатов
pause
