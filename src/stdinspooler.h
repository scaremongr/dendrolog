#ifndef STDINSPOOLER_H
#define STDINSPOOLER_H

#include <QString>
#include <QThread>
#include <atomic>
#include <memory>

class SpoolItem;

// ============================================================================
// StdinSpooler — приём потокового ввода: `program | DendroLog -`.
// Пайп нельзя ни перечитать, ни промотать, поэтому stdin спулится во
// временный файл, а вкладка открывает его как обычный РАСТУЩИЙ лог: работает
// весь существующий конвейер (детекция дозаписи, инкрементальная загрузка,
// follow-tail, при росте за порог — индексный бэкенд).
//
// Поток блокируется в чтении stdin; на выходе приложения при живом пайпе
// прервать fread нечем — requestStop() + короткий wait, затем terminate()
// (поток пишет только в свой буфер/файл — на завершении процесса безопасно).
//
// Файл спула (F-7) — SpoolItem в общем каталоге временных файлов (SpoolDir,
// `<root>/<pid>-<hex>/<n>-stream-stdin.log`): уборка сирот после падения
// убирает и его. Файл держат спулер и LogFile вкладки; удаляется, когда
// отпущены оба.
// ============================================================================
class StdinSpooler : public QThread {
    Q_OBJECT

public:
    explicit StdinSpooler(QObject* parent = nullptr);
    ~StdinSpooler() override;

    // Создаёт файл спула в каталоге экземпляра под spoolRoot
    // (SpoolDir::shared) и запускает поток чтения. false — файл не создался.
    bool startSpooling(const QString& spoolRoot, QString* errorString = nullptr);
    // Файл спула (валиден после успешного startSpooling()).
    QString spoolFilePath() const { return m_path; }
    std::shared_ptr<SpoolItem> spool() const { return m_spool; }
    void requestStop() { m_stop.store(true); }

    // Шов для теста окна (P-9): читать не stdin (дескриптор 0), а этот
    // дескриптор — конец пайпа теста. Программа его не вызывает; задаётся до
    // startSpooling(), значение берёт поток при запуске. -1 — снова stdin.
    static void setInputDescriptorForTesting(int fd);

signals:
    // В файл дописаны данные (не чаще ~3 раз в секунду). Приёмник дёргает
    // reloadChangedFiles() вкладки — сверх обычного таймера auto-reload.
    void bytesAppended(qint64 totalBytes);
    // Источник закрыл пайп (EOF) — больше данных не будет.
    void streamFinished(qint64 totalBytes);

protected:
    void run() override;

private:
    QString m_path;
    std::shared_ptr<SpoolItem> m_spool;
    std::atomic_bool m_stop{false};
};

#endif // STDINSPOOLER_H
