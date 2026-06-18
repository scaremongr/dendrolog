#pragma once

#include <QColor>
#include "logentry.h"

// Forward declaration — keeps this header free of heavy QSettings dependencies.
class QSettings;

// =============================================================================
// AppTheme — единственный источник правды для всех визуальных констант приложения.
//
// Паттерн: Meyers singleton (thread-safe с C++11).
// Все цвета — публичные поля QColor, инициализируемые один раз.
//
// В hot path:  AppTheme::instance() = загрузка указателя (одна инструкция).
//              .fieldName            = чтение поля по смещению.
//              Нулевые накладные расходы по сравнению с глобальными переменными.
//
// Для смены темы: присвойте новые значения полям и перерисуйте (viewport()->update()).
//
// Пример использования:
//   const AppTheme& t = AppTheme::instance();
//   painter.setPen(t.gutterMarker);
//   painter.setBrush(t.badgeBg);
// =============================================================================

class AppTheme {
public:
    static AppTheme& instance() {
        static AppTheme s;
        return s;
    }

    // ---- Уровни лога --------------------------------------------------------
    // Используются для раскраски строк в LogListView и иконок уровней.

    QColor logFatal    {153,   0,   0}; // Тёмно-красный
    QColor logError    {255, 102, 102}; // Светло-красный
    QColor logWarn     {255, 204,   0}; // Жёлтый
    QColor logInfo     {  0, 120, 215}; // Синий (основной акцентный цвет)
    QColor logDebug    {128, 128, 128}; // Серый
    QColor logTrace    {169, 169, 169}; // Светло-серый
    QColor logSeparator{180, 180, 180}; // Разделитель «/» между уровнями W/E/F

    // ---- Бейджи в дереве файлов (насыщенность ниже — для маленьких плашек) --

    QColor treeBadgeFatal{110,   0,   0}; // Тёмно-багровый
    QColor treeBadgeError{175,  45,  45}; // Средний красный
    QColor treeBadgeWarn {175, 125,   0}; // Тёмный янтарь
    QColor treeBadgeSep  {130, 130, 130}; // Разделитель «/» между бейджами

    // ---- Желобок (gutter) ---------------------------------------------------

    QColor gutterMarker  {110, 110, 110, 160}; // Маркер «›» начала записи в LogListView
    QColor gutterNewEntry{ 36, 255, 83      }; // Маркер «›» для строк из последнего auto-reload

    // ---- Плашки (badges) в панели лога --------------------------------------

    QColor badgeBg{  0, 120, 215}; // Фон плашки (= logInfo)
    QColor badgeFg{Qt::white};     // Текст плашки

    // ---- Выделение текста ---------------------------------------------------

    QColor selectionFill {  0, 120, 215, 120}; // Заливка выделенного текстового фрагмента
    QColor selectionRowBg{  0, 120, 215,  60}; // Фон целой выделенной строки

    // ---- Синтаксическая подсветка -------------------------------------------

    QColor syntaxString{200, 180, 100};   // Строковые литералы ('...' и "...")
    QColor syntaxNumber{Qt::darkGreen};   // Числа, hex-значения (0x...)
    QColor syntaxUrl   { 30, 150, 220};   // Гиперссылки (http://, ftp:// и т.п.)
    QColor syntaxHex   {  0, 105,  65};   // Шестнадцатеричные литералы (0x...)
    QColor syntaxPath  { 90, 150, 145};   // Пути к файлам (/dir/file, C:\...)
    QColor syntaxGuid  {175, 130, 200};   // UUID / GUID (8-4-4-4-12)
    QColor syntaxTime  {200, 150, 100};   // Временные метки (2024-01-15T10:30:45)

    // ---- Подсветка парных скобок --------------------------------------------

    QColor bracketMatch{255, 200,   0, 190}; // Фон найденной парной скобки (янтарный)

    // ---- Подсветка результата поиска ----------------------------------------

    QColor searchMatch{255, 160,  70, 210};  // Фон вхождений искомого текста в найденной строке

    // ---- Вспомогательные методы ---------------------------------------------

    // Цвет для произвольного уровня лога.
    QColor forLevel(LogLevel level) const {
        switch (level) {
            case LogLevel::Fatal: return logFatal;
            case LogLevel::Error: return logError;
            case LogLevel::Warn:  return logWarn;
            case LogLevel::Info:  return logInfo;
            case LogLevel::Debug: return logDebug;
            case LogLevel::Trace: return logTrace;
            default:              return QColor(Qt::black);
        }
    }

    // Persistence — called from AppSettings::load() / save().
    // The [Theme] group in the INI file is owned by these methods.
    void load(QSettings& s);
    void save(QSettings& s) const;

private:
    AppTheme() = default;
    AppTheme(const AppTheme&)            = delete;
    AppTheme& operator=(const AppTheme&) = delete;
};
