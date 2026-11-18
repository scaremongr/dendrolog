// ============================================================================
// theme_mode — ручной выбор темы (Settings → Appearance):
//   • правило выбора: какая схема действует при таком-то значении настройки
//     и такой-то теме ОС — таблицей;
//   • настройка хранится строкой и переживает save()/load(); мусор в ini
//     читается как «как в системе»;
//   • контроллер: «как в системе» следует за темой ОС, ручной режим её
//     игнорирует, смена настройки применяется сразу — палитрой приложения;
//   • светлая палитра светлая и тогда, когда стиль под тёмной ОС отдал бы
//     тёмную (запасной путь без QStyleHints::setColorScheme);
//   • открытый LogListView перекрашивается: в кэше отрисовки строк не
//     остаётся текста цветом прежней темы;
//   • плашка «↓ N new lines» рисуется цветами текущей палитры и
//     перекрашивается вместе с темой.
//   • тёмная палитра задаёт все роли сама и не зависит от прежней палитры
//     приложения; её вид при обычном старте закреплён значениями;
//   • открытые панели (Text Filters, Search, Row Highlighters, Entry Details,
//     Statistics, сканер каталога, стартовый экран) после смены темы выглядят так же, как
//     созданные сразу в новой теме, — сравнением картинок.
// ============================================================================

#include "LogListView.h"
#include "appsettings.h"
#include "apptheme.h"
#include "cardframe.h"
#include "colorschemecontroller.h"
#include "newlinesindicator.h"
#include "directoryscannerpanel.h"
#include "entrydetailspanel.h"
#include "filterpanelwidget.h"
#include "markerpanelwidget.h"
#include "bookmarkpanelwidget.h"
#include "logviewwidget.h"
#include "statisticspanel.h"
#include "welcomewidget.h"
#include "testdocuments.h"

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <memory>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

static bool isDark(const QPalette& p)
{
    return p.color(QPalette::Base).lightness() < 128
        && p.color(QPalette::Window).lightness() < 128
        && p.color(QPalette::Text).lightness() > 128;
}

static bool isLight(const QPalette& p)
{
    return p.color(QPalette::Base).lightness() > 128
        && p.color(QPalette::Window).lightness() > 128
        && p.color(QPalette::Text).lightness() < 128;
}

static void checkRule()
{
    using S = Qt::ColorScheme;
    struct Case { ThemeMode mode; S system; S expected; const char* name; };
    const Case cases[] = {
        {ThemeMode::System, S::Light,   S::Light, "system/light"},
        {ThemeMode::System, S::Dark,    S::Dark,  "system/dark"},
        {ThemeMode::System, S::Unknown, S::Light, "system/unknown is light, as before"},
        {ThemeMode::Light,  S::Light,   S::Light, "light/light"},
        {ThemeMode::Light,  S::Dark,    S::Light, "light ignores a dark OS"},
        {ThemeMode::Light,  S::Unknown, S::Light, "light/unknown"},
        {ThemeMode::Dark,   S::Light,   S::Dark,  "dark ignores a light OS"},
        {ThemeMode::Dark,   S::Dark,    S::Dark,  "dark/dark"},
        {ThemeMode::Dark,   S::Unknown, S::Dark,  "dark/unknown"},
    };
    for (const Case& c : cases)
        CHECK(effectiveColorScheme(c.mode, c.system) == c.expected, QStringLiteral("rule: ") + c.name);
}

static void checkPersistence()
{
    CHECK(AppSettings::themeModeFromString(QStringLiteral("dark")) == ThemeMode::Dark, "parse dark");
    CHECK(AppSettings::themeModeFromString(QStringLiteral("Light")) == ThemeMode::Light, "parse is case-insensitive");
    CHECK(AppSettings::themeModeFromString(QStringLiteral("system")) == ThemeMode::System, "parse system");
    CHECK(AppSettings::themeModeFromString(QStringLiteral("solarized")) == ThemeMode::System, "garbage is system");
    CHECK(AppSettings::themeModeFromString(QString()) == ThemeMode::System, "missing key is system");
    for (ThemeMode m : {ThemeMode::System, ThemeMode::Light, ThemeMode::Dark})
        CHECK(AppSettings::themeModeFromString(AppSettings::themeModeToString(m)) == m, "string round trip");

    AppSettings& s = AppSettings::instance();
    s.load();
    CHECK(s.themeMode() == ThemeMode::System, "default is system");

    int changed = 0;
    const auto counter = QObject::connect(&s, &AppSettings::settingsChanged, [&changed] { ++changed; });
    s.setThemeMode(ThemeMode::Dark);
    CHECK(changed == 1, "setter notifies");
    s.setThemeMode(ThemeMode::Dark);
    CHECK(changed == 1, "same value does not notify");
    QObject::disconnect(counter);
    s.save();
    {
        QSettings ini(AppSettings::iniFilePath(), QSettings::IniFormat);
        CHECK(ini.value(QStringLiteral("App/theme")).toString() == QLatin1String("dark"), "stored as a word");
    }
    s.setThemeMode(ThemeMode::Light);
    s.load();
    CHECK(s.themeMode() == ThemeMode::Dark, "load restores the saved choice");

    // Пользовательские цвета — один набор на обе темы: выбор темы их не трогает.
    const QColor custom(12, 34, 56);
    const QColor before = AppTheme::instance().logWarn;
    AppTheme::instance().logWarn = custom;
    s.setThemeMode(ThemeMode::Light);
    s.save();
    s.setThemeMode(ThemeMode::Dark);
    s.save();
    AppTheme::instance().logWarn = before;
    s.load();
    CHECK(AppTheme::instance().logWarn == custom, "custom colours survive a theme switch");
    AppTheme::instance().logWarn = before;

    s.setThemeMode(ThemeMode::System);
    s.save();
}

static void checkController()
{
    AppSettings& s = AppSettings::instance();
    s.setThemeMode(ThemeMode::System);

    Qt::ColorScheme os = Qt::ColorScheme::Light;
    ColorSchemeController controller;
    controller.setSystemSchemeSource([&os] { return os; });
    int changed = 0;
    QObject::connect(&controller, &ColorSchemeController::schemeChanged, [&changed] { ++changed; });

    CHECK(controller.effectiveScheme() == Qt::ColorScheme::Light, "starts light");
    CHECK(isLight(QApplication::palette()), "light palette applied");

    // «Как в системе»: смена темы ОС подхватывается.
    os = Qt::ColorScheme::Dark;
    controller.refresh();
    CHECK(controller.effectiveScheme() == Qt::ColorScheme::Dark, "system mode follows the OS to dark");
    CHECK(isDark(QApplication::palette()), "dark palette applied");
    CHECK(changed == 1, "one change reported");

    // Ручной выбор применяется сразу, по сигналу настроек.
    s.setThemeMode(ThemeMode::Light);
    CHECK(controller.effectiveScheme() == Qt::ColorScheme::Light, "manual light under a dark OS");
    CHECK(isLight(QApplication::palette()), "light palette under a dark OS");
    CHECK(changed == 2, "manual choice reported");

    // Ручной режим: тема ОС игнорируется.
    os = Qt::ColorScheme::Light;
    controller.refresh();
    os = Qt::ColorScheme::Dark;
    controller.refresh();
    CHECK(controller.effectiveScheme() == Qt::ColorScheme::Light, "manual light ignores OS changes");
    CHECK(isLight(QApplication::palette()), "palette unchanged by OS changes");
    CHECK(changed == 2, "no change reported in manual mode");

    os = Qt::ColorScheme::Light;
    controller.refresh();
    s.setThemeMode(ThemeMode::Dark);
    CHECK(isDark(QApplication::palette()), "manual dark under a light OS");
    controller.refresh();
    CHECK(isDark(QApplication::palette()), "manual dark ignores a light OS");

    // Возврат к «как в системе» берёт текущую тему ОС.
    s.setThemeMode(ThemeMode::System);
    CHECK(controller.effectiveScheme() == Qt::ColorScheme::Light, "back to system: light OS");
    CHECK(isLight(QApplication::palette()), "back to system: light palette");
    os = Qt::ColorScheme::Dark;
    controller.refresh();
    CHECK(isDark(QApplication::palette()), "system mode follows the OS again");

    os = Qt::ColorScheme::Light;
    controller.refresh();
}

static void checkPalettes()
{
    CHECK(isDark(ColorSchemeController::darkPalette()), "dark palette is dark");
    CHECK(isLight(ColorSchemeController::lightPalette()), "own light palette is light");
    CHECK(isLight(ColorSchemeController::paletteFor(Qt::ColorScheme::Light)), "paletteFor(light)");
    CHECK(isDark(ColorSchemeController::paletteFor(Qt::ColorScheme::Dark)), "paletteFor(dark)");
    CHECK(isLight(ColorSchemeController::paletteFor(Qt::ColorScheme::Unknown)), "paletteFor(unknown)");
    // Стиль под тёмной ОС отдаёт тёмную стандартную палитру (Fusion с Qt 6.5):
    // для светлой темы она не годится, берётся собственная.
    CHECK(isLight(ColorSchemeController::lightPaletteFrom(ColorSchemeController::darkPalette())),
          "a dark standard palette is replaced");
    QPalette lightStandard = ColorSchemeController::lightPalette();
    lightStandard.setColor(QPalette::Highlight, QColor(1, 2, 3));
    CHECK(ColorSchemeController::lightPaletteFrom(lightStandard) == lightStandard,
          "a light standard palette is kept as is");
}

// Число «чёрных» пикселей — текст, нарисованный цветом светлой темы.
static int blackPixels(const QImage& image)
{
    int n = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qRed(row[x]) < 16 && qGreen(row[x]) < 16 && qBlue(row[x]) < 16)
                ++n;
    }
    return n;
}

static void checkOpenView(const QDir& dir)
{
    AppSettings& s = AppSettings::instance();
    s.setThemeMode(ThemeMode::Light);
    ColorSchemeController controller;

    QByteArray bytes;
    for (int i = 0; i < 40; ++i)
        bytes += "plain words without any markup at all, row " + QByteArray(i % 2 ? "odd" : "even") + '\n';
    const QString path = testdocs::writeFile(dir, QStringLiteral("plain.log"), bytes);
    testdocs::Document doc = testdocs::buildResident({path}, testdocs::buildSchema());
    CHECK(doc.errors.isEmpty(), "fixture loads");

    LogListView view;
    view.resize(900, 400);
    view.setModel(doc.model.get());
    view.show();
    testdocs::pump(300);

    const QImage light = view.viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    CHECK(blackPixels(light) > 20, "light theme draws dark text");

    s.setThemeMode(ThemeMode::Dark);
    testdocs::pump(50);
    const QImage dark = view.viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    CHECK(dark.pixelColor(dark.width() - 5, dark.height() - 5).lightness() < 128
          || dark.pixelColor(dark.width() - 5, 5).lightness() < 128, "open view gets the dark background");
    CHECK(blackPixels(dark) == 0,
          QStringLiteral("no row is left painted with the light theme's text colour: %1 px").arg(blackPixels(dark)));

    s.setThemeMode(ThemeMode::Light);
    testdocs::pump(50);
    const QImage again = view.viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    CHECK(blackPixels(again) > 20, "and back to light");

    // Плашка новых строк: фон — Highlight палитры, текст — HighlightedText.
    auto* indicator = view.findChild<NewLinesIndicator*>(QStringLiteral("newLinesIndicator"));
    CHECK(indicator, "the view has a new-lines indicator");
    if (indicator) {
        indicator->setCount(12345);
        indicator->resize(indicator->sizeHint());
        indicator->show();
        CHECK(indicator->text().contains(QStringLiteral("12,345 new lines")), "indicator text");
        CHECK(NewLinesIndicator::label(1) == QStringLiteral("\u2193 1 new line")
                  && NewLinesIndicator::label(99999).contains(QStringLiteral("99,999 "))
                  && NewLinesIndicator::label(120500).contains(QStringLiteral(" 120K "))
                  && NewLinesIndicator::label(3456789).contains(QStringLiteral(" 3.4M "))
                  && NewLinesIndicator::label(25000000).contains(QStringLiteral(" 25M ")),
              "large counts are shortened");
        const auto fillAndText = [&](const char* theme) {
            const QImage image = indicator->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const QPalette& pal = indicator->palette();
            // Точка у левого скругления: внутри плашки, левее текста.
            const QColor fill = image.pixelColor(image.height() / 2 + 2, image.height() / 2);
            const QColor expected = pal.color(QPalette::Highlight);
            const auto near = [](const QColor& a, const QColor& b) {
                return qAbs(a.red() - b.red()) < 12 && qAbs(a.green() - b.green()) < 12
                    && qAbs(a.blue() - b.blue()) < 12;
            };
            CHECK(near(fill, expected),
                  QStringLiteral("%1: indicator fill is the palette highlight (%2 vs %3)")
                      .arg(theme, fill.name(), expected.name()));
            const QColor ink = pal.color(QPalette::HighlightedText);
            int inked = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (near(image.pixelColor(x, y), ink))
                        ++inked;
            CHECK(inked > 10, QStringLiteral("%1: indicator text uses the highlighted-text colour").arg(theme));
            return fill;
        };
        const QColor lightFill = fillAndText("light");
        s.setThemeMode(ThemeMode::Dark);
        testdocs::pump(50);
        const QColor darkFill = fillAndText("dark");
        CHECK(isDark(indicator->palette()), "the indicator follows the theme change");
        Q_UNUSED(lightFill);
        Q_UNUSED(darkFill);
        s.setThemeMode(ThemeMode::Light);
        testdocs::pump(50);
        fillAndText("light again");
    }

    s.setThemeMode(ThemeMode::System);
}

// ---------------------------------------------------------------------------
// Тёмная палитра задаёт все роли сама: какая палитра была у приложения до
// неё — неважно. Раньше незаданные роли (Light, Mid, Dark, Shadow…)
// наследовались от текущей палитры приложения.
// ---------------------------------------------------------------------------
static QString describeDifference(const QPalette& a, const QPalette& b)
{
    for (int g = 0; g < QPalette::NColorGroups; ++g) {
        for (int r = 0; r < QPalette::NColorRoles; ++r) {
            const QBrush x = a.brush(QPalette::ColorGroup(g), QPalette::ColorRole(r));
            const QBrush y = b.brush(QPalette::ColorGroup(g), QPalette::ColorRole(r));
            if (x != y)
                return QStringLiteral("group %1 role %2: %3 vs %4")
                    .arg(g).arg(r).arg(x.color().name(QColor::HexArgb), y.color().name(QColor::HexArgb));
        }
    }
    return QString();
}

static void checkDarkPaletteIsSelfContained()
{
    const QPalette before = QApplication::palette();

    QApplication::setPalette(ColorSchemeController::lightPalette());
    const QPalette afterLight = ColorSchemeController::darkPalette();

    // «Чистый старт» под тёмной ОС: стиль отдаёт тёмную палитру.
    QApplication::setPalette(afterLight);
    const QPalette afterDark = ColorSchemeController::darkPalette();

    // И заведомо чужая палитра — каждая роль своего цвета.
    QPalette garish;
    for (int g = 0; g < QPalette::NColorGroups; ++g)
        for (int r = 0; r < QPalette::NColorRoles; ++r)
            garish.setColor(QPalette::ColorGroup(g), QPalette::ColorRole(r),
                            QColor(200, 10 * g, 10 * r));
    QApplication::setPalette(garish);
    const QPalette afterGarish = ColorSchemeController::darkPalette();

    CHECK(describeDifference(afterLight, afterDark).isEmpty(),
          QStringLiteral("dark after light == dark after dark: ") + describeDifference(afterLight, afterDark));
    CHECK(describeDifference(afterLight, afterGarish).isEmpty(),
          QStringLiteral("dark does not inherit from the previous palette: ")
              + describeDifference(afterLight, afterGarish));
    CHECK(afterLight == afterDark && afterLight == afterGarish, "palettes compare equal");

    // Вид тёмной темы при обычном старте (под светлой темой) закреплён:
    // роли, которые раньше доставались от светлой палитры Fusion.
    struct Pin { QPalette::ColorGroup group; QPalette::ColorRole role; QColor color; const char* name; };
    const Pin pins[] = {
        {QPalette::Active,   QPalette::Window,     QColor(0x2d, 0x2d, 0x2d), "Window"},
        {QPalette::Active,   QPalette::Base,       QColor(0x1e, 0x1e, 0x1e), "Base"},
        {QPalette::Active,   QPalette::Text,       QColor(0xdd, 0xdd, 0xdd), "Text"},
        {QPalette::Active,   QPalette::Highlight,  QColor(0x2a, 0x7a, 0xda), "Highlight"},
        {QPalette::Active,   QPalette::Light,      QColor(255, 255, 255),    "Light"},
        {QPalette::Active,   QPalette::Midlight,   QColor(202, 202, 202),    "Midlight"},
        {QPalette::Active,   QPalette::Mid,        QColor(184, 184, 184),    "Mid"},
        {QPalette::Active,   QPalette::Dark,       QColor(159, 159, 159),    "Dark"},
        {QPalette::Active,   QPalette::Shadow,     QColor(118, 118, 118),    "Shadow"},
        {QPalette::Active,   QPalette::LinkVisited, QColor(255, 0, 255),     "LinkVisited"},
        {QPalette::Active,   QPalette::BrightText, QColor(255, 0, 0),        "BrightText"},
        {QPalette::Active,   QPalette::AlternateBase, QColor(0x2a, 0x2a, 0x2a), "AlternateBase"},
        {QPalette::Active,   QPalette::Button,     QColor(0x35, 0x35, 0x35), "Button"},
        {QPalette::Active,   QPalette::ToolTipBase, QColor(0x2d, 0x2d, 0x2d), "ToolTipBase"},
        {QPalette::Active,   QPalette::PlaceholderText, QColor(0x7f, 0x7f, 0x7f), "PlaceholderText"},
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        {QPalette::Active,   QPalette::Accent,     QColor(48, 140, 198),     "Accent"},
        {QPalette::Disabled, QPalette::Accent,     QColor(145, 145, 145),    "Disabled Accent"},
#endif
        {QPalette::Disabled, QPalette::Highlight,  QColor(0x45, 0x45, 0x45), "Disabled Highlight"},
        {QPalette::Inactive, QPalette::Mid,        QColor(184, 184, 184),    "Inactive Mid"},
        {QPalette::Disabled, QPalette::Dark,       QColor(190, 190, 190),    "Disabled Dark"},
        {QPalette::Disabled, QPalette::Shadow,     QColor(177, 177, 177),    "Disabled Shadow"},
        {QPalette::Disabled, QPalette::Base,       QColor(0x1e, 0x1e, 0x1e), "Disabled Base"},
        {QPalette::Disabled, QPalette::Text,       QColor(0x7f, 0x7f, 0x7f), "Disabled Text"},
    };
    for (const Pin& pin : pins) {
        const QColor got = afterLight.color(pin.group, pin.role);
        CHECK(got.rgba() == pin.color.rgba(), QStringLiteral("dark palette %1: %2, expected %3")
                                    .arg(QLatin1String(pin.name), got.name(), pin.color.name()));
    }

    QApplication::setPalette(before);
}

// ---------------------------------------------------------------------------
// Открытые панели перекрашиваются при смене темы. Проверка общая, а не по
// одному свойству на виджет: панель, созданная в одной теме и пережившая
// смену, обязана выглядеть ровно как панель, созданная сразу в новой теме.
// Всё, что посчитано из палитры один раз (стиль, HTML, иконка), даёт
// отличающиеся пиксели.
// ---------------------------------------------------------------------------
static QImage grabOf(QWidget& w)
{
    // Рамка фокуса зависит от того, какое окно активно, а не от темы.
    if (QWidget* focused = QApplication::focusWidget())
        focused->clearFocus();
    return w.grab().toImage().convertToFormat(QImage::Format_RGB32);
}

// Число отличающихся пикселей и описание первого из них.
static int differingPixels(const QImage& a, const QImage& b, QString* where)
{
    if (a.size() != b.size()) {
        *where = QStringLiteral("sizes differ: %1x%2 vs %3x%4")
                     .arg(a.width()).arg(a.height()).arg(b.width()).arg(b.height());
        return -1;
    }
    int n = 0;
    QRect box;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) == b.pixel(x, y))
                continue;
            if (n == 0)
                *where = QStringLiteral("first at (%1,%2): %3, a fresh widget has %4")
                             .arg(x).arg(y).arg(a.pixelColor(x, y).name(), b.pixelColor(x, y).name());
            box |= QRect(x, y, 1, 1);
            ++n;
        }
    }
    if (n)
        *where += QStringLiteral("; box %1,%2 %3x%4").arg(box.x()).arg(box.y()).arg(box.width()).arg(box.height());
    return n;
}

using WidgetFactory = std::function<std::unique_ptr<QWidget>()>;

static std::unique_ptr<QWidget> shown(const WidgetFactory& make)
{
    std::unique_ptr<QWidget> w = make();
    w->setAttribute(Qt::WA_ShowWithoutActivating);
    w->resize(520, 360);
    w->move(300, 300); // не под указателем: наведение — тоже не тема
    w->show();
    testdocs::pump(80);
    return w;
}

static void checkRepaintsOnThemeChange(const char* name, const WidgetFactory& make)
{
    AppSettings& s = AppSettings::instance();
    const struct { ThemeMode from; ThemeMode to; const char* name; } directions[] = {
        {ThemeMode::Light, ThemeMode::Dark, "light -> dark"},
        {ThemeMode::Dark, ThemeMode::Light, "dark -> light"},
    };
    for (const auto& d : directions) {
        s.setThemeMode(d.from);
        testdocs::pump(20);
        std::unique_ptr<QWidget> survivor = shown(make);
        const QImage before = grabOf(*survivor);

        s.setThemeMode(d.to);
        testdocs::pump(80);
        const QImage switched = grabOf(*survivor);
        std::unique_ptr<QWidget> fresh = shown(make);
        const QImage reference = grabOf(*fresh);

        QString where;
        CHECK(differingPixels(before, reference, &where) != 0,
              QStringLiteral("%1, %2: the themes look different at all").arg(QLatin1String(name), QLatin1String(d.name)));
        const int n = differingPixels(switched, reference, &where);
        CHECK(n == 0, QStringLiteral("%1, %2: %3 px keep the previous theme (%4)")
                          .arg(QLatin1String(name), QLatin1String(d.name)).arg(n).arg(where));
    }
    s.setThemeMode(ThemeMode::System);
}

static void checkOpenPanels(const QDir& dir)
{
    ColorSchemeController controller;

    // Цвета правил и маркеров — данные пользователя, от темы не зависят;
    // задаём их явно, чтобы сравнивать только «хром» панели.
    const auto filterPanel = [](FilterPanelWidget::Mode mode) {
        return [mode]() -> std::unique_ptr<QWidget> {
            auto panel = std::make_unique<FilterPanelWidget>(mode);
            FilterRuleSet set;
            FilterRule shown;
            shown.text = QStringLiteral("timeout");
            shown.highlightColor = QColor(0x33, 0x99, 0x66);
            FilterRule hollow = shown;
            hollow.text = QStringLiteral("retry");
            hollow.highlightEnabled = false;
            FilterRule broken = shown;
            broken.text = QStringLiteral("(unclosed");
            broken.isRegex = true;
            set.rules = {shown, hollow, broken};
            panel->setRuleSet(set);
            return panel;
        };
    };
    checkRepaintsOnThemeChange("Text Filters", filterPanel(FilterPanelWidget::Mode::Filter));
    checkRepaintsOnThemeChange("Search query", filterPanel(FilterPanelWidget::Mode::Search));
    // Пустая панель Text Filters не сравнивается: в ней заготовка правила, а
    // её цвет подсветки — данные правила, выбранные по теме в момент создания.

    checkRepaintsOnThemeChange("Row Highlighters", []() -> std::unique_ptr<QWidget> {
        auto panel = std::make_unique<MarkerPanelWidget>();
        HighlightPattern marker;
        marker.text = QStringLiteral("ERROR");
        marker.color = QColor(0x99, 0x33, 0x66);
        panel->setMarkers({marker});
        return panel;
    });
    checkRepaintsOnThemeChange("Row Highlighters, empty", []() -> std::unique_ptr<QWidget> {
        return std::make_unique<MarkerPanelWidget>();
    });

    // Закладки: живая с заметкой и скрытая фильтром (приглушённый акцент).
    // Вкладка одна на все экземпляры панели — данные не зависят от темы.
    {
        const QString path = testdocs::writeFile(dir, QStringLiteral("bookmarks.log"),
            "2026-03-05 10:00:00.000 [w] INFO - first\n"
            "2026-03-05 10:00:01.000 [w] ERROR - second\n"
            "2026-03-05 10:00:02.000 [w] INFO - third\n");
        LogViewWidget tab;
        tab.addLogFile(path);
        testdocs::waitFor([&tab] { return !tab.isLoading() && tab.model()->rowCount() == 3; });
        const int live = tab.bookmarks().add(1);
        tab.bookmarks().setNote(live, QStringLiteral("connection lost"));
        tab.bookmarks().add(2);
        tab.model()->setLogLevelFilter({ LogLevel::Error });
        testdocs::settle(*tab.model());
        LogViewWidget* source = &tab;
        checkRepaintsOnThemeChange("Bookmarks", [source]() -> std::unique_ptr<QWidget> {
            auto panel = std::make_unique<BookmarkPanelWidget>();
            panel->setLogView(source);
            return panel;
        });
    }
    checkRepaintsOnThemeChange("Bookmarks, empty", []() -> std::unique_ptr<QWidget> {
        return std::make_unique<BookmarkPanelWidget>();
    });

    checkRepaintsOnThemeChange("Entry Details", []() -> std::unique_ptr<QWidget> {
        return std::make_unique<EntryDetailsPanel>();
    });
    checkRepaintsOnThemeChange("Entry Details, a section off", []() -> std::unique_ptr<QWidget> {
        auto panel = std::make_unique<EntryDetailsPanel>();
        const auto toggles = panel->findChildren<QToolButton*>();
        if (!toggles.isEmpty())
            toggles.first()->setChecked(false);
        return panel;
    });

    checkRepaintsOnThemeChange("Statistics placeholder", []() -> std::unique_ptr<QWidget> {
        return std::make_unique<StatisticsPanel>();
    });

    checkRepaintsOnThemeChange("Directory scanner", []() -> std::unique_ptr<QWidget> {
        return std::make_unique<DirectoryScannerPanel>();
    });

    checkRepaintsOnThemeChange("Welcome screen", []() -> std::unique_ptr<QWidget> {
        auto welcome = std::make_unique<WelcomeWidget>();
        welcome->setRecentFiles({QStringLiteral("/var/log/app/server.log"),
                                 QStringLiteral("/var/log/app/worker.log")});
        return welcome;
    });

    checkRepaintsOnThemeChange("Empty card list", []() -> std::unique_ptr<QWidget> {
        auto list = std::make_unique<CardListArea>();
        list->setEmptyText(QStringLiteral("Nothing here yet"));
        list->cardsChanged();
        return list;
    });
}

// Фон поля быстрого поиска «не найдено» считается из палитры на момент
// вызова — MainWindow пересчитывает его на PaletteChange.
static void checkNotFoundColour()
{
    const QColor error = AppTheme::instance().logError;
    const QColor onLight = AppTheme::searchNotFoundBackground(ColorSchemeController::lightPalette());
    const QColor onDark = AppTheme::searchNotFoundBackground(ColorSchemeController::darkPalette());
    CHECK(onLight.lightness() > 128, "not-found tint on the light theme is light");
    CHECK(onDark.lightness() < 128, "not-found tint on the dark theme is dark");
    CHECK(onLight == CardFrame::mixedColor(error, QColor(Qt::white), 0.72), "light value unchanged");
    CHECK(onDark == CardFrame::mixedColor(error, QColor(0x1e, 0x1e, 0x1e), 0.72), "dark value unchanged");
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("DendroLogTest"));
    QApplication::setApplicationName(QStringLiteral("theme_mode"));
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setCursorFlashTime(0); // мигающий курсор — шум для сравнения картинок
    QFile::remove(AppSettings::iniFilePath());

    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;

    checkRule();
    checkPersistence();
    checkPalettes();
    checkDarkPaletteIsSelfContained();
    checkNotFoundColour();
    checkController();
    checkOpenView(QDir(temporary.path()));
    checkOpenPanels(QDir(temporary.path()));

    QFile::remove(AppSettings::iniFilePath());
    if (failures) {
        std::fprintf(stderr, "theme_mode: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("theme_mode: OK\n");
    return 0;
}
