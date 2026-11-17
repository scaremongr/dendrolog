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
//     остаётся текста цветом прежней темы.
// ============================================================================

#include "LogListView.h"
#include "appsettings.h"
#include "apptheme.h"
#include "colorschemecontroller.h"
#include "testdocuments.h"

#include <QApplication>
#include <QImage>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <cstdio>

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

    s.setThemeMode(ThemeMode::System);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("DendroLogTest"));
    QApplication::setApplicationName(QStringLiteral("theme_mode"));
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFile::remove(AppSettings::iniFilePath());

    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;

    checkRule();
    checkPersistence();
    checkPalettes();
    checkController();
    checkOpenView(QDir(temporary.path()));

    QFile::remove(AppSettings::iniFilePath());
    if (failures) {
        std::fprintf(stderr, "theme_mode: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("theme_mode: OK\n");
    return 0;
}
