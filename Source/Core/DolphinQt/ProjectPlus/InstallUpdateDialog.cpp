/*
*  Project+ Dolphin Self-Updater
*  Credit to the Mario Party Netplay team for the base code of this updater
*  Copyright (C) 2025 Tabitha Hanegan
*/
#define QT_NO_CAST_FROM_ASCII

#include "Common/MinizipUtil.h"
#include "InstallUpdateDialog.h"
#include "DownloadWorker.h"

#include <QCoreApplication>
#include <QProcess>
#include <QDir>
#include <QTextStream>
#include <QVBoxLayout>
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QMessageBox>
#include <QThread>
#include <QStorageInfo>
#include <QJsonObject>
#include <QTimer>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QDebug>

#include "Common/HttpRequest.h"

#include <mz.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

// --------------------- SCOOP DEPENDENCY CHECK ----------------------
void InstallUpdateDialog::ensureDependenciesThen(std::function<void()> cont)
{
#ifdef _WIN32
    const bool haveAria   = !QStandardPaths::findExecutable(QStringLiteral("aria2c")).isEmpty();
    const bool haveRclone = !QStandardPaths::findExecutable(QStringLiteral("rclone")).isEmpty();

    if (haveAria && haveRclone) {
        // Nothing to do
        cont();
        return;
    }

    // Tell the user what we’re doing (non-blocking)
    stepLabel->setText(QStringLiteral("Installing download tools (scoop)..."));
    qDebug().noquote() << "[deps] Installing aria2 & rclone via scoop (if missing)";

    QProcess* ps = new QProcess(this);
    ps->setProcessChannelMode(QProcess::MergedChannels);
    const QString psPath = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));

    // User-scope install (no admin), add bucket, install missing tools.
    // Always continue even if something fails so we still try rclone/HTTP later.
    const QString script = QString::fromUtf8(
        "$ErrorActionPreference='Continue';"
        "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;"
        "if (-not (Get-Command scoop -ErrorAction SilentlyContinue)) {"
        "  iwr -useb get.scoop.sh | iex"
        "};"
        "scoop bucket add main 2>$null | Out-Null;"
        "if (-not (Get-Command aria2c -ErrorAction SilentlyContinue)) {"
        "  scoop install aria2 2>$null | Out-Null"
        "};"
        "if (-not (Get-Command rclone -ErrorAction SilentlyContinue)) {"
        "  scoop install rclone 2>$null | Out-Null"
        "};"
        "Write-Host 'DONE'"
    );

    connect(ps, &QProcess::readyReadStandardOutput, this, [ps]() {
        const QString out = QString::fromUtf8(ps->readAllStandardOutput()).trimmed();
        if (!out.isEmpty())
            qDebug().noquote() << "[scoop]" << out;
    });

    connect(ps, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [=](int /*code*/, QProcess::ExitStatus) {
        ps->deleteLater();
        // Re-check and continue regardless — we still have rclone/HTTP fallbacks
        const bool nowAria   = !QStandardPaths::findExecutable(QStringLiteral("aria2c")).isEmpty();
        const bool nowRclone = !QStandardPaths::findExecutable(QStringLiteral("rclone")).isEmpty();
        qDebug().noquote() << "[deps] aria2c=" << nowAria << "rclone=" << nowRclone;
        cont();
    });

    ps->start(psPath.isEmpty() ? QStringLiteral("powershell.exe") : psPath,
              { QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                QStringLiteral("-Command"), script });
#else
    // Non-Windows: nothing to install here — just continue.
    cont();
#endif
}



// --------------------- UTILITAIRE ETA ----------------------
static QString formatETA(double seconds)
{
    if (seconds < 1.0)
        return QStringLiteral("less than 1s");

    int m = static_cast<int>(seconds) / 60;
    int s = static_cast<int>(seconds) % 60;

    if (m > 0)
    {
        QString sStr = QString::asprintf("%02d", s);
        return QString::asprintf("%dm%ss", m, sStr.toUtf8().constData());
    }

    return QString::asprintf("%ds", s);
}

// --------------------- UTILITAIRES -----------------------
static bool hasEnoughFreeSpace(const QString& path, qint64 minFreeBytes)
{
    QStorageInfo storage(path);
    storage.refresh();
    return storage.bytesAvailable() >= minFreeBytes;
}

// --------------------- CONSTRUCTEUR ----------------------
InstallUpdateDialog::InstallUpdateDialog(QWidget *parent,
                                         QString installationDirectory,
                                         QString temporaryDirectory,
                                         QString filename,
                                         QString downloadUrl,
                                         QString sdUrl)
    : QDialog(parent),
      installationDirectory(std::move(installationDirectory)),
      temporaryDirectory(std::move(temporaryDirectory)),
      filename(std::move(filename)),
      downloadUrl(std::move(downloadUrl)),
      m_sdUrl(std::move(sdUrl))
{
    setWindowTitle(QStringLiteral("Project+ Dolphin - Updater"));

    QVBoxLayout* layout = new QVBoxLayout(this);
    label = new QLabel(QStringLiteral("Preparing installation..."), this);
    progressBar = new QProgressBar(this);
    stepLabel = new QLabel(QStringLiteral("Preparing..."), this);
    stepProgressBar = new QProgressBar(this);

    layout->addWidget(label);
    layout->addWidget(progressBar);
    layout->addWidget(stepLabel);
    layout->addWidget(stepProgressBar);
    setLayout(layout);
    setMinimumSize(400, 150);

    progressBar->setVisible(true);
    stepLabel->setVisible(true);
    stepProgressBar->setVisible(true);

    startTimer(100);
}

InstallUpdateDialog::~InstallUpdateDialog() = default;

// --------------------- CHECK ----------------------
void InstallUpdateDialog::checkIfAllDownloadsFinished(bool sdFinished, bool sdSuccess)
{
    static bool sdDone = false;
    static bool zipStarted = false;
    static bool zipDone = false;

    // Mise à jour des états
    if (sdFinished && sdSuccess)
        sdDone = true;

    qDebug().noquote() << "🧠 Status → sdDone=" << sdDone
                       << ", zipStarted=" << zipStarted
                       << ", zipDone=" << zipDone;

    // ------------- ETAPE 1 : SD pas encore finie -------------
    if (!sdDone)
    {
        qDebug().noquote() << "⏳ Waiting for SD download...";
        return;
    }

    // ------------- ETAPE 2 : Téléchargement ZIP -------------
    if (!zipStarted && !downloadUrl.isEmpty())
    {
        zipStarted = true;
        qDebug().noquote() << "📦 Starting main ZIP download after SD...";

        // Mise à jour UI
        label->setText(QStringLiteral("Step 2/2: Downloading main package..."));
        stepLabel->setText(QStringLiteral("Preparing download..."));
        stepProgressBar->setValue(0);
        progressBar->setValue(50);  // ✅ 50 % après la SD

        QThread* thread = new QThread;
        auto* worker = new DownloadWorker(downloadUrl,
                                          temporaryDirectory + QDir::separator() + filename);
        worker->moveToThread(thread);

        connect(thread, &QThread::started, worker, &DownloadWorker::startDownload);

        connect(worker, &DownloadWorker::progressUpdated, this, [this](qint64 done, qint64 total) {
            if (total <= 0)
                return;
            const int percent = static_cast<int>((done * 100) / total);
            stepProgressBar->setValue(percent);
            stepLabel->setText(QStringLiteral("Downloading main ZIP: %1%").arg(percent));

            // 🔹 Fait progresser la barre principale entre 50 et 100 %
            progressBar->setValue(50 + percent / 2);
        });

        connect(worker, &DownloadWorker::finished, this, [=]() {
            qDebug().noquote() << "✅ Main ZIP download finished";
            zipDone = true;

            thread->quit();
            worker->deleteLater();
            thread->deleteLater();

            // ✅ Passage à l’installation
            QMetaObject::invokeMethod(this, [=]() {
                this->checkIfAllDownloadsFinished(true, true);
            }, Qt::QueuedConnection);
        });

        connect(worker, &DownloadWorker::errorOccurred, this, [=](const QString& err) {
            qWarning().noquote() << "❌ ZIP download failed:" << err;
            zipDone = true;
            thread->quit();
            worker->deleteLater();
            thread->deleteLater();
            QMessageBox::critical(this, QStringLiteral("Error"), err);
        });

        thread->start();
        return;
    }

    // ------------- ETAPE 3 : Installation -------------
    if (sdDone && zipDone)
    {
        qDebug().noquote() << "🚀 All downloads complete → starting installation...";
        label->setText(QStringLiteral("Installing update..."));
        progressBar->setValue(100);
        stepLabel->setText(QStringLiteral("Extracting and finalizing..."));
        install();
    }
}


// --------------------- TELECHARGEMENT ----------------------
void InstallUpdateDialog::download()
{
    label->setText(QStringLiteral("Step 1/2: Downloading SD card..."));
    progressBar->setRange(0, 100);
    stepProgressBar->setRange(0, 100);
    stepLabel->setText(QStringLiteral("0% Downloaded..."));
    progressBar->setValue(0);
    stepProgressBar->setValue(0);

    if (!hasEnoughFreeSpace(QDir::tempPath(), 8LL * 1024 * 1024 * 1024))
    {
        QMessageBox::critical(this, QStringLiteral("Error"),
                              QStringLiteral("You need at least 8 GB free space to install this update."));
        reject();
        return;
    }

    // 🔸 NEW: wait for dependencies (aria2c/rclone)
    ensureDependenciesThen([=]() {
        const QString sdUrl = m_sdUrl;
        if (sdUrl.isEmpty())
        {
            qWarning().noquote() << "⚠️ No SD URL found, skipping SD download.";
            checkIfAllDownloadsFinished(true, true);
            return;
        }

        const QString sdPath = QDir::toNativeSeparators(
            QCoreApplication::applicationDirPath() + QStringLiteral("/User/Wii/sd.raw"));

        QFileInfo fi(sdPath);
        QDir().mkpath(fi.path());
        if (QFile::exists(sdPath))
        {
            QFile::remove(sdPath);
            qDebug().noquote() << "🧹 Removed old file:" << sdPath;
        }

        auto sdFinished = std::make_shared<bool>(false);
        auto sdSuccess = std::make_shared<bool>(false);

        auto uiProgress = [this](int p, const QString& t)
        {
            stepProgressBar->setValue(qBound(0, p, 100));
            stepLabel->setText(t);
        };
        auto uiDone = [this](bool ok, const QString& t)
        {
            stepProgressBar->setValue(ok ? 100 : 0);
            stepLabel->setText(t);
        };

        // 1️⃣ Try aria2c if available
        const QString ariaPath = QStandardPaths::findExecutable(QStringLiteral("aria2c"));
        if (!ariaPath.isEmpty())
        {
            qDebug().noquote() << "🧩 aria2c detected → using aria2c for SD download";

            QProcess* aria = new QProcess(this);
            aria->setProcessChannelMode(QProcess::MergedChannels);
            aria->setWorkingDirectory(QFileInfo(sdPath).path());

            QStringList args = {
                QStringLiteral("--allow-overwrite=true"),
                QStringLiteral("-x"), QStringLiteral("8"),
                QStringLiteral("-s"), QStringLiteral("8"),
                QStringLiteral("--console-log-level=notice"),
                QStringLiteral("--summary-interval=1"),
                QStringLiteral("--enable-color=false"),
                QStringLiteral("--show-console-readout=false"),
                QStringLiteral("-d"), QFileInfo(sdPath).path(),
                QStringLiteral("-o"), QFileInfo(sdPath).fileName(),
                sdUrl
            };

            connect(aria, &QProcess::readyReadStandardOutput, this, [this, aria, uiProgress]() {
                const QString out = QString::fromUtf8(aria->readAllStandardOutput());
                QRegularExpression re(QStringLiteral(R"(\((\d{1,3})%\).+DL:([\d\.]+[KMG]i?B))"));
                auto m = re.match(out);
                if (m.hasMatch())
                {
                    int percent = qBound(0, m.captured(1).toInt(), 100);
                    QString speed = m.captured(2).trimmed();
                    uiProgress(percent, QStringLiteral("aria2c: %1% (%2/s)").arg(percent).arg(speed));
                }
            });

            connect(aria, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                    this, [=](int code, QProcess::ExitStatus) {
                QFileInfo fi(sdPath);
                bool ok = (code == 0 && fi.exists() && fi.size() > (10 * 1024 * 1024));

                if (ok)
                {
                    qDebug().noquote() << "✅ SD download succeeded with aria2c";
                    *sdFinished = true;
                    *sdSuccess = true;
                    uiDone(true, QStringLiteral("🎉 SD download complete (aria2c)!"));
                    QMetaObject::invokeMethod(this, [=]() {
                        this->checkIfAllDownloadsFinished(*sdFinished, *sdSuccess);
                    }, Qt::QueuedConnection);
                }
                else
                {
                    qWarning().noquote() << "❌ aria2c failed → switching to rclone...";
                    startRcloneFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
                }

                aria->deleteLater();
            });

            qDebug().noquote() << "🚀 Launching aria2c:" << ariaPath << args.join(QLatin1Char(' '));
            aria->start(ariaPath, args);
            return;
        }

        // 2️⃣ Try rclone (it will fallback to HTTPS inside)
        const QString rclonePath = QStandardPaths::findExecutable(QStringLiteral("rclone"));
        if (!rclonePath.isEmpty())
        {
            qDebug().noquote() << "🧩 rclone detected → using rclone fallback";
            startRcloneFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
        }
        else
        {
            // 3️⃣ If neither exists → fallback to HTTP directly
            qWarning().noquote() << "⚠️ Neither aria2c nor rclone found → switching to HTTPS fallback...";
            startHttpFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
        }
    }); // ✅ ferme le lambda
} // ✅ ferme la fonction





// --------------------- RCLONE FALLBACK (corrigé et optimisé) ----------------------
void InstallUpdateDialog::startRcloneFallback(const QString& sdUrl,
                                              const QString& sdPath,
                                              std::shared_ptr<bool> sdFinished,
                                              std::shared_ptr<bool> sdSuccess,
                                              std::function<void(int, const QString&)> uiProgress,
                                              std::function<void(bool, const QString&)> uiDone)
{
    QProcess* rclone = new QProcess(this);
    rclone->setProcessChannelMode(QProcess::MergedChannels);

    // 🔹 Extraire base URL et nom du fichier
    QUrl url(sdUrl);
    QString baseUrl = url.adjusted(QUrl::RemoveFilename).toString();
    QString fileName = QFileInfo(url.path()).fileName();

    // ⚙️ Commande rclone optimisée
    QStringList rargs = {
        QStringLiteral("copyto"),
        QStringLiteral(":http:%1").arg(fileName),
        QDir::toNativeSeparators(sdPath),
        QStringLiteral("--http-url"), baseUrl,
        QStringLiteral("--multi-thread-streams=8"),
        QStringLiteral("--multi-thread-cutoff=1M"),
        QStringLiteral("--buffer-size=64M"),
        QStringLiteral("--transfers=4"),
        QStringLiteral("--no-check-certificate"),
        QStringLiteral("--progress"),
        QStringLiteral("--retries=2"),
        QStringLiteral("--low-level-retries=3")
    };

    qDebug().noquote() << "🚀 Launching rclone:" << rargs.join(QLatin1Char(' '));

    auto lastUpdate = std::make_shared<QElapsedTimer>();
    lastUpdate->start();

    connect(rclone, &QProcess::readyReadStandardOutput, this,
            [this, rclone, uiProgress, lastUpdate]() {
        const QString out = QString::fromUtf8(rclone->readAllStandardOutput());
        const QStringList lines = out.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);

        static int lastPercent = 0;

        for (const QString& line : lines)
        {
            QString trimmed = line.trimmed();

            // Exemple : "Transferred:   3.750 GiB / 5.500 GiB, 68%, 98.2 MiB/s, ETA 37s"
            QRegularExpression re(QStringLiteral(
                R"(Transferred:.*?,\s*(\d{1,3})%,\s*([\d\.]+\s*[KMG]i?B\/s))"));
            auto match = re.match(trimmed);

            if (match.hasMatch())
            {
                int percent = qBound(0, match.captured(1).toInt(), 100);
                QString speed = match.captured(2).trimmed();

                if (lastUpdate->elapsed() > 200 || percent != lastPercent)
                {
                    uiProgress(percent, QStringLiteral("rclone: %1% (%2)").arg(percent).arg(speed));
                    lastPercent = percent;
                    lastUpdate->restart();
                }
            }
        }
    });

    connect(rclone, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [=](int code, QProcess::ExitStatus) {
        QFileInfo fi(sdPath);
        const bool ok = (code == 0 && fi.exists() && fi.size() > (10 * 1024 * 1024));

        if (ok)
        {
            qDebug().noquote() << "✅ SD download succeeded with rclone";
            *sdFinished = true;
            *sdSuccess = true;
            uiDone(true, QStringLiteral("🎉 SD download complete (rclone)!"));
        }
        else
        {
            qWarning().noquote() << "❌ rclone failed → trying HTTPS fallback...";
            *sdFinished = false;
            *sdSuccess = false;

            // 🔁 Fallback HTTPS
            startHttpFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
        }

        // ✅ Signale la fin (même si fallback)
        QMetaObject::invokeMethod(this, [=]() {
            this->checkIfAllDownloadsFinished(*sdFinished, *sdSuccess);
        }, Qt::QueuedConnection);

        rclone->deleteLater();
    });

    rclone->start(QStringLiteral("rclone"), rargs);
}




// --------------------- HTTP FALLBACK ----------------------
void InstallUpdateDialog::startHttpFallback(const QString& sdUrl,
                                            const QString& sdPath,
                                            std::shared_ptr<bool> sdFinished,
                                            std::shared_ptr<bool> sdSuccess,
                                            std::function<void(int, const QString&)> uiProgress,
                                            std::function<void(bool, const QString&)> uiDone)
{
#ifdef _WIN32
    QProcess* ps = new QProcess(this);
    ps->setProcessChannelMode(QProcess::MergedChannels);
    const QString psPath = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));

    // ✅ Script PowerShell avec vitesse fluide et unité adaptée (B/s, KB/s, MB/s)
    const QString script = QString::fromUtf8(
         "$url='%1';$out='%2';"
    "$req=[System.Net.HttpWebRequest]::Create($url);"
    "$req.Proxy=[System.Net.GlobalProxySelection]::GetEmptyWebProxy();"
    "$req.UserAgent='ProjectPlus-Updater';"
    "$res=$req.GetResponse();"
    "$total=$res.ContentLength;"
    "$stream=$res.GetResponseStream();"
    "$fs=[System.IO.FileStream]::new($out,[System.IO.FileMode]::Create);"
    "$buffer=New-Object byte[] (4MB);"  
    "$received=0;$prev=0;"
    "$sw=[System.Diagnostics.Stopwatch]::StartNew();"
    "while(($read=$stream.Read($buffer,0,$buffer.Length)) -gt 0){"
    "  $fs.Write($buffer,0,$read);"
    "  $received+=$read;"
    "  if($sw.ElapsedMilliseconds -gt 500){"
    "    $percent=[math]::Round(($received/$total)*100,1);"
    "    $speedBytes=($received-$prev)/$sw.Elapsed.TotalSeconds;"
    "    if($speedBytes -gt 1048576){$speedStr=[Math]::Round($speedBytes/1MB,2).ToString()+' MB/s'}"
    "    elseif($speedBytes -gt 1024){$speedStr=[Math]::Round($speedBytes/1KB,1).ToString()+' KB/s'}"
    "    else{$speedStr=$speedBytes.ToString()+' B/s'}"
    "    Write-Host (\"$percent|$speedStr\");"
    "    $prev=$received;$sw.Restart()"
    "  }"
    "}"
    "$fs.Close();$stream.Close();$res.Close();"
    "Write-Host 'DONE';"
).arg(sdUrl, sdPath);

    connect(ps, &QProcess::readyReadStandardOutput, this, [this, ps, uiProgress]() {
        const QString out = QString::fromUtf8(ps->readAllStandardOutput()).trimmed();
        if (out.isEmpty())
            return;

        const QStringList lines = out.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
        for (const QString& line : lines)
        {
            const QStringList parts = line.trimmed().split(QLatin1Char('|'));
            if (parts.size() == 2 &&
                parts[0].contains(QRegularExpression(QStringLiteral(R"(^\d+(\.\d+)?)"))))
            {
                const int percent = qBound(0, static_cast<int>(parts[0].toDouble()), 100);
                const QString speed = parts[1].trimmed();
                uiProgress(percent, QStringLiteral("HTTP: %1% (%2)").arg(percent).arg(speed));
            }
            else if (line.contains(QStringLiteral("DONE"), Qt::CaseInsensitive))
            {
                uiProgress(100, QStringLiteral("HTTP: 100% (done)"));
            }
            else
            {
                qDebug().noquote() << "[HTTP]" << line;
            }
        }
    });

    connect(ps, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [=](int exitCode, QProcess::ExitStatus) {
        QFileInfo fi(sdPath);
        const bool ok = (exitCode == 0 && fi.exists() && fi.size() > (10 * 1024 * 1024));
        *sdFinished = true;
        *sdSuccess = ok;

        if (ok)
        {
            uiDone(true, QStringLiteral("✅ SD download complete (HTTPS)!"));
            qDebug().noquote() << "✅ HTTPS download finished successfully.";
        }
        else
        {
            uiDone(false, QStringLiteral("❌ HTTPS download failed."));
            qWarning().noquote() << "❌ HTTPS PowerShell fallback failed.";
            QMessageBox::critical(nullptr,
                                  QStringLiteral("Download failed"),
                                  QStringLiteral("HTTPS download failed.\n\n"
                                                 "Please check your Internet connection or try again later."));
        }

        QMetaObject::invokeMethod(this, [=]() {
            this->checkIfAllDownloadsFinished(*sdFinished, *sdSuccess);
        }, Qt::QueuedConnection);

        ps->deleteLater();
    });

    qDebug().noquote() << "🌐 Launching PowerShell HTTPS fallback...";
    ps->start(psPath.isEmpty() ? QStringLiteral("powershell.exe") : psPath,
              { QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                QStringLiteral("-Command"), script });
#else
    // macOS/Linux fallback (curl)
    QProcess* curl = new QProcess(this);
    curl->setProcessChannelMode(QProcess::MergedChannels);

    connect(curl, &QProcess::readyReadStandardOutput, this, [this, curl, uiProgress]() {
        const QString out = QString::fromUtf8(curl->readAllStandardOutput());
        QRegularExpression re(QStringLiteral(R"((\d{1,3})%)"));
        auto m = re.match(out);
        if (m.hasMatch()) {
            int p = m.captured(1).toInt();
            uiProgress(p, QStringLiteral("curl: %1%").arg(p));
        }
    });

    connect(curl, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [=](int e, QProcess::ExitStatus) {
        QFileInfo fi(sdPath);
        const bool ok = (e == 0 && fi.exists() && fi.size() > (10 * 1024 * 1024));
        *sdFinished = true;
        *sdSuccess = ok;
        uiDone(ok, ok ? QStringLiteral("✅ SD download complete (curl)!")
                      : QStringLiteral("❌ SD download failed (curl)"));
        if (!ok) {
            QMessageBox::critical(nullptr, QStringLiteral("Download failed"),
                                  QStringLiteral("Download via curl also failed."));
        }
        QMetaObject::invokeMethod(this, [=]() {
            this->checkIfAllDownloadsFinished(*sdFinished, *sdSuccess);
        }, Qt::QueuedConnection);
        curl->deleteLater();
    });

    curl->start(QStringLiteral("curl"),
                { QStringLiteral("-L"), QStringLiteral("--progress-bar"),
                  QStringLiteral("-o"), sdPath, sdUrl });
#endif
}


// --------------------- TIMER ----------------------
void InstallUpdateDialog::timerEvent(QTimerEvent *e)
{
    killTimer(e->timerId());
    if (!downloadUrl.isEmpty())
        download();
    else
        install();
}

// --------------------- INSTALL (simplifié pour l'instant) ----------------------
void InstallUpdateDialog::install()
{
    qDebug().noquote() << QStringLiteral("🧩 Starting installation...");

    const QString zipFile = temporaryDirectory + QDir::separator() + filename;
    const QString destDir = installationDirectory;

    if (!QFile::exists(zipFile))
    {
        QMessageBox::critical(this, QStringLiteral("Error"),
                              QStringLiteral("ZIP file missing!"));
        reject();
        return;
    }

    stepProgressBar->setValue(50);
    progressBar->setValue(50);
    label->setText(QStringLiteral("Step 2/2: Installing update..."));
    progressBar->setValue(50);
stepLabel->setText(QStringLiteral("Extracting files..."));
progressBar->setValue(50);
stepProgressBar->setValue(50);


    // --- Extraction ZIP via minizip ---
    bool success = unzipFile(zipFile.toStdString(), destDir.toStdString(),
        [this](int current, int total)
        {
            int percent = (total > 0) ? (current * 100 / total) : 0;
            stepProgressBar->setValue(percent);
            stepLabel->setText(QStringLiteral("Extracting: %1%").arg(percent));
        });

    if (!success)
    {
        QMessageBox::critical(this, QStringLiteral("Error"),
                              QStringLiteral("Failed to extract ZIP file."));
        reject();
        return;
    }

    QFile::remove(zipFile);
    qDebug().noquote() << QStringLiteral("✅ ZIP extracted and deleted.");

    stepLabel->setText(QStringLiteral("Installation complete!"));
    stepProgressBar->setValue(100);
    progressBar->setValue(100);

       qDebug().noquote() << QStringLiteral("✅ Installation complete → restarting Dolphin...");

#ifdef _WIN32
    const QString exe = QDir::toNativeSeparators(
        installationDirectory + QDir::separator() + QStringLiteral("Dolphin.exe"));
#else
    const QString exe = QDir::toNativeSeparators(
        installationDirectory + QDir::separator() + QStringLiteral("Dolphin"));
#endif

    if (!QFile::exists(exe))
    {
        QMessageBox::information(this, QStringLiteral("Done"),
                                 QStringLiteral("Installation finished. Launch manually."));
        accept();
        return;
    }

#ifdef _WIN32
    const QString appPid = QString::number(QCoreApplication::applicationPid());
    const QString tempDir = QDir::toNativeSeparators(QDir::tempPath());
    const QString scriptPath = QDir(tempDir).filePath(QStringLiteral("restart_dolphin.bat")); // ✅ plus sûr

    QStringList scriptLines = {
        QStringLiteral("@echo off"),
        QStringLiteral("echo == Closing Dolphin PID %1").arg(appPid),
        QStringLiteral("taskkill /F /PID %1 >nul 2>&1").arg(appPid),
        QStringLiteral("timeout /t 1 >nul"),
        QStringLiteral("echo == Relaunching Dolphin..."),
        QStringLiteral("start \"\" \"%1\"").arg(exe),
        QStringLiteral("echo == Cleanup"),
        QStringLiteral("del \"%1\" >nul 2>&1").arg(scriptPath),
        QStringLiteral("exit")
    };

    QFile scriptFile(scriptPath);
    if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QTextStream out(&scriptFile);
        for (const QString& line : scriptLines)
            out << line << QLatin1Char('\n'); // ✅ évite conversion implicite
        scriptFile.close();
    }
    else
    {
        QMessageBox::warning(this, QStringLiteral("Error"),
                             QStringLiteral("Failed to create restart script."));
        return;
    }

    SHELLEXECUTEINFO sei = {0};
    sei.cbSize = sizeof(SHELLEXECUTEINFO);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    sei.hwnd = nullptr;
    sei.lpVerb = L"runas";
    sei.lpFile = reinterpret_cast<LPCWSTR>(scriptPath.utf16());
    sei.lpParameters = nullptr;
    sei.lpDirectory = nullptr;
    sei.nShow = SW_HIDE;

    if (!ShellExecuteEx(&sei))
    {
        QMessageBox::critical(this, QStringLiteral("Error"),
                              QStringLiteral("Failed to launch restart script as administrator."));
    }

    QCoreApplication::quit();
#else
    qDebug().noquote() << "♻️ Restarting Dolphin (Linux/macOS)...";
    if (!QProcess::startDetached(exe, QCoreApplication::arguments()))
    {
        QMessageBox::warning(this, QStringLiteral("Restart failed"),
                             QStringLiteral("Failed to relaunch Dolphin automatically.\nPlease restart manually."));
    }
    QCoreApplication::quit();
#endif
}





// --------------------- EXTRACTION ZIP ----------------------
bool InstallUpdateDialog::unzipFile(const std::string& zipFilePath,
                                    const std::string& destDir,
                                    std::function<void(int, int)> progressCallback)
{
    qDebug().noquote() << QStringLiteral("📦 Extracting ZIP:") << QString::fromStdString(zipFilePath);

    void* reader = mz_zip_reader_create();
    if (!reader)
        return false;

    if (mz_zip_reader_open_file(reader, zipFilePath.c_str()) != MZ_OK)
    {
        mz_zip_reader_delete(&reader);
        return false;
    }

    int total = 0;
    {
        void* tmp = mz_zip_reader_create();
        if (mz_zip_reader_open_file(tmp, zipFilePath.c_str()) == MZ_OK)
        {
            mz_zip_reader_goto_first_entry(tmp);
            while (mz_zip_reader_goto_next_entry(tmp) == MZ_OK)
                total++;
            mz_zip_reader_close(tmp);
            mz_zip_reader_delete(&tmp);
        }
    }

    int current = 0;
    int err = mz_zip_reader_goto_first_entry(reader);
    while (err == MZ_OK)
    {
        err = mz_zip_reader_entry_open(reader);
        if (err != MZ_OK)
            break;

        mz_zip_file* info = nullptr;
        mz_zip_reader_entry_get_info(reader, &info);

        if (info && info->filename)
        {
            std::string entry = info->filename;
            std::string outPath = destDir + "/" + entry;

            if (entry.back() == '/')
            {
                QDir().mkpath(QString::fromStdString(outPath));
            }
            else
            {
                QDir().mkpath(QFileInfo(QString::fromStdString(outPath)).path());
                mz_zip_reader_entry_save_file(reader, outPath.c_str());
            }

            current++;
            if (progressCallback)
                progressCallback(current, total);
        }

        mz_zip_reader_entry_close(reader);
        err = mz_zip_reader_goto_next_entry(reader);
    }

    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    return true;
}

void InstallUpdateDialog::closeEvent(QCloseEvent* event)
{
    // Tuer proprement tous les sous-processus actifs (aria2c, rclone, PowerShell, curl)
    const auto processes = findChildren<QProcess*>();
    for (QProcess* p : processes)
    {
        if (p && p->state() != QProcess::NotRunning)
        {
            qDebug().noquote() << "🛑 Killing running process:" << p->program();
            disconnect(p, nullptr, this, nullptr);
            p->kill();
            p->waitForFinished(1000);
        }
    }

    QDialog::closeEvent(event);
}
