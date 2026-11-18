#include "colorschemecontroller.h"

#include <QApplication>
#include <QPointer>
#include <QWidget>
#include <QStyle>
#include <QStyleHints>

Qt::ColorScheme effectiveColorScheme(ThemeMode mode, Qt::ColorScheme systemScheme)
{
    switch (mode) {
        case ThemeMode::Light: return Qt::ColorScheme::Light;
        case ThemeMode::Dark:  return Qt::ColorScheme::Dark;
        case ThemeMode::System: break;
    }
    return systemScheme == Qt::ColorScheme::Dark ? Qt::ColorScheme::Dark
                                                 : Qt::ColorScheme::Light;
}

static bool looksDark(const QPalette& p)
{
    return p.color(QPalette::Window).lightness() < 128
        || p.color(QPalette::Base).lightness() < 128;
}

// ---------------------------------------------------------------------------
// Windows (especially Windows 10) does not propagate the system dark theme to
// the classic native widget style, so the application runs on Fusion — which
// fully honours the QPalette — and is fed a light or dark palette here.
// ---------------------------------------------------------------------------
QPalette ColorSchemeController::darkPalette()
{
    const QColor window(0x2d, 0x2d, 0x2d);
    const QColor base(0x1e, 0x1e, 0x1e);
    const QColor alternateBase(0x2a, 0x2a, 0x2a);
    const QColor text(0xdd, 0xdd, 0xdd);
    const QColor button(0x35, 0x35, 0x35);
    const QColor highlight(0x2a, 0x7a, 0xda);
    const QColor disabled(0x7f, 0x7f, 0x7f);

    // Каждая роль задана здесь: палитра не зависит от того, какая была у
    // приложения до неё. Роли рельефа, посещённой ссылки и акцента раньше не
    // задавались и доставались от прежней палитры; значения ниже — те, что
    // получались при обычном старте (тёмная тема поверх светлой палитры
    // Fusion), чтобы вид тёмной темы не изменился. Закреплены в theme_mode.
    const QColor light(255, 255, 255);
    const QColor midlight(202, 202, 202);
    const QColor mid(184, 184, 184);
    const QColor dark(159, 159, 159);
    const QColor shadow(118, 118, 118);

    // Конструктор начинает с пустой палитры, а не с палитры приложения.
    QPalette p(text, button, light, dark, mid, text, QColor(Qt::red), base, window);
    p.setColor(QPalette::Midlight, midlight);
    p.setColor(QPalette::Shadow, shadow);
    p.setColor(QPalette::AlternateBase, alternateBase);
    p.setColor(QPalette::ToolTipBase, window);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::LinkVisited, QColor(255, 0, 255));
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, disabled);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, QColor(48, 140, 198));
#endif

    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Dark, QColor(190, 190, 190));
    p.setColor(QPalette::Disabled, QPalette::Shadow, QColor(177, 177, 177));
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x45, 0x45, 0x45));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Disabled, QPalette::Accent, QColor(145, 145, 145));
#endif
    return p;
}

// Светлая палитра Fusion, записанная явно: стандартная палитра стиля под
// тёмной ОС — тёмная (см. шапку заголовка).
QPalette ColorSchemeController::lightPalette()
{
    const QColor window(239, 239, 239);
    const QColor light = window.lighter(150);
    const QColor mid = window.darker(130);
    const QColor dark = window.darker(150);
    const QColor shadow = dark.darker(135);
    const QColor text(Qt::black);
    const QColor highlight(48, 140, 198);
    const QColor disabledText(190, 190, 190);

    QPalette p(text, window, light, dark, mid, text, Qt::white, Qt::white, window);
    p.setColor(QPalette::Midlight, mid.lighter(110));
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Shadow, shadow);
    p.setColor(QPalette::AlternateBase, QColor(247, 247, 247));
    p.setColor(QPalette::ToolTipBase, QColor(255, 255, 220));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Link, QColor(0, 0, 255));
    p.setColor(QPalette::LinkVisited, QColor(128, 0, 128));
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(0, 0, 0, 128));

    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::Base, window);
    p.setColor(QPalette::Disabled, QPalette::Dark, QColor(209, 209, 209).darker(110));
    p.setColor(QPalette::Disabled, QPalette::Shadow, shadow.lighter(150));
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(145, 145, 145));
    return p;
}

QPalette ColorSchemeController::lightPaletteFrom(const QPalette& standardPalette)
{
    return looksDark(standardPalette) ? lightPalette() : standardPalette;
}

QPalette ColorSchemeController::paletteFor(Qt::ColorScheme effective)
{
    if (effective == Qt::ColorScheme::Dark)
        return darkPalette();
    const QStyle* style = QApplication::style();
    return style ? lightPaletteFrom(style->standardPalette()) : lightPalette();
}

// ---------------------------------------------------------------------------
// Qt не доводит смену палитры до виджетов со своим style sheet и до их
// детей: QWidgetPrivate::propagatePaletteChange пропускает WA_StyleSheet, и
// такой виджет остаётся с палитрой, с которой был создан, не получив даже
// PaletteChange. У нас это каждая CardFrame со всем содержимым, подписи
// приглушённого цвета, образцы цвета. Переполировка стилем снимает с виджета
// палитру, сохранённую таблицей стилей (он берёт новую у родителя и получает
// PaletteChange), и применяет таблицу заново — заодно пересчитываются
// ссылки palette(...) внутри неё. Порядок — от родителей к детям: палитра
// ребёнка берётся у родителя.
// ---------------------------------------------------------------------------
void ColorSchemeController::repolishStyledWidgets()
{
    QList<QPointer<QWidget>> widgets;
    const auto topLevels = QApplication::topLevelWidgets();
    for (QWidget* top : topLevels)
        widgets.append(top);
    for (int i = 0; i < widgets.size(); ++i) {
        const QObjectList children = widgets.at(i)->children();
        for (QObject* child : children) {
            if (child->isWidgetType())
                widgets.append(static_cast<QWidget*>(child));
        }
    }
    // Обработчик PaletteChange вправе пересобрать своих детей — отсюда QPointer.
    for (const QPointer<QWidget>& widget : std::as_const(widgets)) {
        if (!widget || !widget->testAttribute(Qt::WA_StyleSheet))
            continue;
        QStyle* style = widget->style();
        style->unpolish(widget);
        if (widget)
            style->polish(widget);
    }
}

// ---------------------------------------------------------------------------
ColorSchemeController::ColorSchemeController(QObject* parent)
    : QObject(parent)
{
    connect(&AppSettings::instance(), &AppSettings::settingsChanged,
            this, &ColorSchemeController::refresh);
    // Follow the OS theme live if the user toggles dark/light mode while the
    // application is running.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, &ColorSchemeController::refresh);
    refresh();
}

void ColorSchemeController::setSystemSchemeSource(SystemSchemeSource source)
{
    m_systemScheme = std::move(source);
    refresh();
}

void ColorSchemeController::requestPlatformScheme(ThemeMode mode)
{
    // Пока пользователь не выбирал тему вручную, платформу не трогаем вовсе.
    if (mode == m_requestedMode)
        return;
    m_requestedMode = mode;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    Qt::ColorScheme request = Qt::ColorScheme::Unknown;   // снять запрос
    if (mode == ThemeMode::Light)
        request = Qt::ColorScheme::Light;
    else if (mode == ThemeMode::Dark)
        request = Qt::ColorScheme::Dark;
    QGuiApplication::styleHints()->setColorScheme(request);
#endif
}

void ColorSchemeController::refresh()
{
    // Запрос платформе и смена палитры сами порождают сигналы, ведущие сюда.
    if (m_refreshing) {
        m_refreshAgain = true;
        return;
    }
    m_refreshing = true;
    do {
        m_refreshAgain = false;

        const ThemeMode mode = AppSettings::instance().themeMode();
        requestPlatformScheme(mode);

        const Qt::ColorScheme system = m_systemScheme
            ? m_systemScheme()
            : QGuiApplication::styleHints()->colorScheme();
        const Qt::ColorScheme effective = effectiveColorScheme(mode, system);

        // Стандартная палитра стиля могла смениться вместе с темой ОС, даже
        // если действующая схема та же, — сравниваем саму палитру.
        const QPalette palette = paletteFor(effective);
        if (palette != QApplication::palette()) {
            QApplication::setPalette(palette);
            repolishStyledWidgets();
        }

        if (effective != m_effective) {
            m_effective = effective;
            emit schemeChanged(effective);
        }
    } while (m_refreshAgain);
    m_refreshing = false;
}
