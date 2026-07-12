#ifndef STDINSPOOLER_H
#define STDINSPOOLER_H

#include <QString>
#include <QThread>
#include <atomic>

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
// ============================================================================
class StdinSpooler : public QThread {
    Q_OBJECT

public:
    explicit StdinSpooler(QObject* parent = nullptr);
    ~StdinSpooler() override;

    // Создаёт файл спула и запускает поток чтения. false — файл не создался.
    bool startSpooling();
    // Путь файла спула (валиден после успешного startSpooling()).
    QString spoolFilePath() const { return m_path; }
    void requestStop() { m_stop.store(true); }

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
    std::atomic_bool m_stop{false};
};

#endif // STDINSPOOLER_H
