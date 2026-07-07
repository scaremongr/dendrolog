@echo off
REM Скрипт для настройки переменных окружения и запуска CMake
REM Путь к Qt, при необходимости измените на ваш путь установки Qt
set QT_PATH=C:\Qt\6.11.1\msvc2022_64

echo Настройка переменных окружения для Qt...
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