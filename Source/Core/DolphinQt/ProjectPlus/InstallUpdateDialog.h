#pragma once

#include <QDialog>
#include <QString>
#include <QLabel>
#include <QProgressBar>
#include <memory>
#include <functional>

class InstallUpdateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit InstallUpdateDialog(QWidget *parent,
                                 QString installationDirectory,
                                 QString temporaryDirectory,
                                 QString filename,
                                 QString downloadUrl,
                                 QString sdUrl);
    ~InstallUpdateDialog();

protected:
    void timerEvent(QTimerEvent *e) override;
    void closeEvent(QCloseEvent* event) override;

private:
    // === Fonctions principales ===
    void ensureDependencies();
    void ensureDependenciesThen(std::function<void()> cont);
    void download();
    void install();
    void checkIfAllDownloadsFinished(bool sdFinished, bool sdSuccess);

    // === Fonctions internes de fallback ===
    void startRcloneFallback(const QString& sdUrl,
                             const QString& sdPath,
                             std::shared_ptr<bool> sdFinished,
                             std::shared_ptr<bool> sdSuccess,
                             std::function<void(int, const QString&)> uiProgress,
                             std::function<void(bool, const QString&)> uiDone);

    void startHttpFallback(const QString& sdUrl,
                           const QString& sdPath,
                           std::shared_ptr<bool> sdFinished,
                           std::shared_ptr<bool> sdSuccess,
                           std::function<void(int, const QString&)> uiProgress,
                           std::function<void(bool, const QString&)> uiDone);

    // === Outils ===
    bool unzipFile(const std::string& zipFilePath,
                   const std::string& destDir,
                   std::function<void(int, int)> progressCallback);

private:
    // === Variables ===
    QString installationDirectory;
    QString temporaryDirectory;
    QString filename;
    QString downloadUrl;
    QString m_sdUrl;

    QLabel* label;
    QLabel* stepLabel;
    QProgressBar* progressBar;
    QProgressBar* stepProgressBar;
};
