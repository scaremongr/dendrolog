#pragma once

#include <QObject>
#include <QPalette>
#include <functional>

#include "appsettings.h"

// =============================================================================
// Тема окна: «как в системе», светлая или тёмная (Settings → Appearance).
//
// effectiveColorScheme() — всё правило выбора: ручной режим тему ОС не
// слушает, «как в системе» берёт её, а неизвестную считает светлой (как было
// до появления выбора). Закреплено таблицей в tests/theme_mode.
//
// ColorSchemeController применяет действующую схему ко всему приложению —
// палитрой QApplication. Стиль (Fusion под CompactStyle) рисует строго по
// палитре, поэтому открытые вкладки, панели и диалоги перекрашиваются сами:
// каждый виджет получает PaletteChange. Кто кэширует посчитанное из палитры
// (LogListView — pixmap'ы строк, таймлайн, HTML панелей), сбрасывает кэш в
// своём changeEvent. Цвета AppTheme от темы не зависят и здесь не меняются.
//
// Ловушки:
//  • QStyle::standardPalette() у Fusion с Qt 6.5 сам следует теме ОС: под
//    тёмной ОС он тёмный и для «Светлой» не годится — lightPaletteFrom()
//    подменяет его собственной светлой палитрой;
//  • QStyleHints::setColorScheme есть только с Qt 6.8 (проект собирается и с
//    6.5). Где он есть, ручной выбор дополнительно сообщается платформе —
//    ради заголовка окна и нативных диалогов; палитра от этого не зависит;
//  • после такого запроса QStyleHints::colorScheme() отдаёт уже не тему ОС,
//    а наш выбор. В ручном режиме это значение не используется, а при
//    возврате к «как в системе» запрос снимается раньше, чем тема читается.
// =============================================================================

// Действующая схема: всегда Light или Dark.
Qt::ColorScheme effectiveColorScheme(ThemeMode mode, Qt::ColorScheme systemScheme);

class ColorSchemeController : public QObject
{
    Q_OBJECT

public:
    // Применяет схему сразу и дальше следит за AppSettings::settingsChanged и
    // QStyleHints::colorSchemeChanged. Создавать после QApplication::setStyle.
    explicit ColorSchemeController(QObject* parent = nullptr);

    // Откуда брать тему ОС. По умолчанию — QStyleHints::colorScheme(); тесты
    // подставляют свою и зовут refresh().
    using SystemSchemeSource = std::function<Qt::ColorScheme()>;
    void setSystemSchemeSource(SystemSchemeSource source);

    Qt::ColorScheme effectiveScheme() const { return m_effective; }

    static QPalette darkPalette();
    static QPalette lightPalette();
    // Светлая палитра из стандартной палитры стиля: она сама, если светлая,
    // иначе lightPalette().
    static QPalette lightPaletteFrom(const QPalette& standardPalette);
    // Палитра приложения для действующей схемы.
    static QPalette paletteFor(Qt::ColorScheme effective);

    // Довести текущую палитру приложения до виджетов со style sheet: сам Qt
    // их при смене палитры пропускает (см. реализацию). refresh() зовёт это
    // после каждой смены палитры.
    static void repolishStyledWidgets();

public slots:
    // Пересчитать действующую схему и применить, если что-то изменилось.
    void refresh();

signals:
    void schemeChanged(Qt::ColorScheme scheme);

private:
    void requestPlatformScheme(ThemeMode mode);

    SystemSchemeSource m_systemScheme;
    Qt::ColorScheme    m_effective = Qt::ColorScheme::Unknown;
    ThemeMode          m_requestedMode = ThemeMode::System;
    bool               m_refreshing = false;
    bool               m_refreshAgain = false;
};
