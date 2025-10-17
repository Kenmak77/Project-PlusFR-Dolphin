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
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QUrl>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
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

    connect(ps, &QProcess::readyReadStandardOutput, this, [this, ps]() {
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

        static double zipDisplayed = 50.0;  // valeur affichée
static double zipTarget    = 50.0;  // cible à atteindre
static QTimer* zipAnimTimer = nullptr;

// Démarre l’anim une seule fois
if (!zipAnimTimer)
{
    zipAnimTimer = new QTimer(this);
    zipAnimTimer->setInterval(30); // ~33 FPS
    connect(zipAnimTimer, &QTimer::timeout, this, [this]() {
        // interpolation lissée vers la cible
        zipDisplayed += (zipTarget - zipDisplayed) * 0.15;  // 0.10 = plus doux, 0.25 = plus réactif
        progressBar->setValue(static_cast<int>(zipDisplayed));
    });
    zipAnimTimer->start();
}

// valeur de départ à 50% quand on commence l’étape 2
zipDisplayed = 50.0;
zipTarget    = 50.0;
progressBar->setValue(50);

      connect(worker, &DownloadWorker::progressUpdated, this, [this](qint64 done, qint64 total) {
    if (total <= 0) return;

    const int localPercent = static_cast<int>((done * 100) / total);

    // barre d’étape (celle du bas) = progression du ZIP
    stepProgressBar->setValue(localPercent);
    stepLabel->setText(QStringLiteral("Step 2/2: Downloading ZIP (%1%)").arg(localPercent));

    // barre globale (haut) : cible entre 50 et 100
    // 50 + (0..100)*0.5  =>  50..100
    zipTarget = 50.0 + (static_cast<double>(localPercent) * 0.5);
});

       connect(worker, &DownloadWorker::finished, this, [=]() {
    qDebug().noquote() << "✅ Main ZIP download finished";
    stepLabel->setText(QStringLiteral("ZIP download complete"));
    stepProgressBar->setValue(75);

    // 💫 Amène la cible à 100% (la barre globale va s’animer jusqu’à 100)
    zipTarget = 100.0;

    // Sécurité : après 400 ms, on s’assure qu’elle est bien à 100%
    QTimer::singleShot(400, this, [this]() {
      if (m_isClosing) return; // ✅ sécurité
        progressBar->setValue(100);
    });

    zipDone = true;

    thread->quit();
    worker->deleteLater();
    thread->deleteLater();

    // ✅ Passage à l’installation après un petit délai (le temps que la barre termine son anim)
    QTimer::singleShot(500, this, [this]() {
      if (m_isClosing) return; // ✅ sécurité
        this->checkIfAllDownloadsFinished(true, true);
    });
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


void InstallUpdateDialog::download()
{
  const qint64 minRequiredBytes = 8ll * 1024 * 1024 * 1024; // 8 Go
if (!hasEnoughFreeSpace(installationDirectory, minRequiredBytes))
{
    QMessageBox::critical(this,
        QStringLiteral("Not enough space"),
        QStringLiteral("You need at least 8 GB of free space to install this update.\n\n"
                       "Please free some disk space and try again."));
    qWarning().noquote() << "❌ Not enough free space. Aborting update.";
    reject();
    return;
}

    label->setText(QStringLiteral("Step 1/2: Checking SD card..."));
    progressBar->setRange(0, 100);
    stepProgressBar->setRange(0, 100);
    progressBar->setValue(0);
    stepProgressBar->setValue(0);
    stepLabel->setText(QStringLiteral("Checking local SD hash..."));

    const QString sdPath = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QStringLiteral("/User/Wii/sd.raw"));

    // ✅ Si la SD existe localement → calcul hash dans un thread
    if (QFile::exists(sdPath))
    {
        qDebug().noquote() << "🔍 SD.raw found locally at:" << sdPath;

        stepLabel->setText(QStringLiteral("Computing SD hash..."));
        stepProgressBar->setRange(0, 0); // indéterminée

        // ⚙️ Thread de calcul du hash
        m_hashThread = QThread::create([this, sdPath]() {
            QFile sdFile(sdPath);
            QString localHash;
            if (sdFile.open(QIODevice::ReadOnly))
            {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                constexpr qint64 chunkSize = 4 * 1024 * 1024; // 4 Mo
                QByteArray buffer;
                buffer.resize(chunkSize);

                qint64 totalSize = sdFile.size();
                qint64 sampleSize = 256ll * 1024 * 1024; // 256 Mo

                // --- Début du fichier ---
                sdFile.seek(0);
                qint64 bytesRead = 0;
                while (bytesRead < sampleSize && !sdFile.atEnd())
                {
                    qint64 n = sdFile.read(buffer.data(), chunkSize);
                    if (n <= 0) break;
                    hash.addData(buffer.constData(), n);
                    bytesRead += n;
                }

                // --- Fin du fichier ---
                if (totalSize > sampleSize)
                {
                    sdFile.seek(qMax(0ll, totalSize - sampleSize));
                    bytesRead = 0;
                    while (!sdFile.atEnd() && bytesRead < sampleSize)
                    {
                        qint64 n = sdFile.read(buffer.data(), chunkSize);
                        if (n <= 0) break;
                        hash.addData(buffer.constData(), n);
                        bytesRead += n;
                    }
                }

                localHash = QString::fromLatin1(hash.result().toHex());
                sdFile.close();
            }

            // ✅ Vérifie que la fenêtre est encore ouverte
            if (m_isClosing)
                return;

            // 🔹 Retour au thread principal
            QMetaObject::invokeMethod(QApplication::instance(), [this, localHash]() {
                if (m_isClosing)
                    return; // sécurité

                qDebug().noquote() << "💠 Local SD hash =" << localHash;
                stepProgressBar->setRange(0, 100);
                stepProgressBar->setValue(100);
                stepLabel->setText(QStringLiteral("Hash check complete!"));

                // 🔹 Compare avec le hash distant
                const QString hashUrl = QStringLiteral("https://update.pplusfr.org/update2.json");
                Common::HttpRequest req;
                auto response = req.Get(hashUrl.toStdString());

                if (!response.has_value())
                {
                    qWarning().noquote() << "⚠️ Failed to fetch" << hashUrl;
                    stepLabel->setText(QStringLiteral("⚠️ Failed to fetch update.json"));
                    return;
                }

                const QByteArray jsonBytes(reinterpret_cast<const char*>(response->data()),
                                           static_cast<int>(response->size()));
                const QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonBytes);
                if (!jsonDoc.isObject())
                {
                    qWarning().noquote() << "⚠️ Invalid JSON from" << hashUrl;
                    return;
                }

                const QJsonObject obj = jsonDoc.object();
                const QString remoteHash = obj.value(QStringLiteral("sd-hash-partial")).toString();

                qDebug().noquote() << "🌐 Remote SD hash (partial) =" << remoteHash;

                if (!remoteHash.isEmpty() &&
                    QString::compare(remoteHash, localHash, Qt::CaseInsensitive) == 0)
                {
                    qDebug().noquote() << "✅ SD already up to date, skipping SD download.";
                    progressBar->setValue(50);
                    stepProgressBar->setValue(100);
                    stepLabel->setText(QStringLiteral("SD is up to date — skipping download"));
                    QTimer::singleShot(300, this, [this]() {
                        if (m_isClosing) return;
                        this->checkIfAllDownloadsFinished(true, true);
                    });
                    return;
                }

                qDebug().noquote() << "⚠️ SD outdated — re-downloading...";
                stepLabel->setText(QStringLiteral("Downloading new SD..."));
                progressBar->setValue(25);
                stepProgressBar->setValue(0);

                this->startSDDownload();
            });
        });

        // 🔹 Nettoyage du thread si la fenêtre est fermée
        connect(this, &QObject::destroyed, this, [this]() {
            if (m_hashThread && m_hashThread->isRunning()) {
                qDebug().noquote() << "🧵 Stopping hash thread (window closed)";
                m_hashThread->requestInterruption();
                m_hashThread->quit();
                m_hashThread->wait(1000);
                m_hashThread = nullptr;
            }
        });

        m_hashThread->start();
        return;
    }

    // -------------------- Aucun fichier SD local → téléchargement direct --------------------
    qDebug().noquote() << "⚠️ No SD.raw found locally → will download.";
    label->setText(QStringLiteral("Step 1/2: Downloading SD card..."));
    stepLabel->setText(QStringLiteral("0% Downloaded..."));
    progressBar->setValue(0);
    stepProgressBar->setValue(0);

    this->startSDDownload(); // télécharge directement
}




void InstallUpdateDialog::startSDDownload()
{
    // Vérifie que aria2c/rclone sont prêts avant de lancer
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
        auto sdSuccess  = std::make_shared<bool>(false);

        auto uiProgress = [this](int p, const QString& t)
        {
            const int clamped = qBound(0, p, 100);
            stepProgressBar->setValue(clamped);
            stepLabel->setText(t);
            progressBar->setValue(clamped / 2); // 0..50%
        };

        auto uiDone = [this](bool ok, const QString& t)
        {
            stepProgressBar->setValue(ok ? 100 : 0);
            stepLabel->setText(t);
            if (ok) progressBar->setValue(50);
        };

        // 1) aria2c si dispo
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
                    const QString speed = m.captured(2).trimmed();
                    uiProgress(percent, QStringLiteral("aria2c: %1% (%2/s)").arg(percent).arg(speed));
                }
            });

            connect(aria, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                    this, [=](int code, QProcess::ExitStatus) {
                QFileInfo fi(sdPath);
                const bool ok = (code == 0 && fi.exists() && fi.size() > (10 * 1024 * 1024));

                if (ok)
                {
                    qDebug().noquote() << "✅ SD download succeeded with aria2c";
                    *sdFinished = true; *sdSuccess = true;
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

        static int ariaRetryCount = 0;
if (ariaRetryCount < 1) {
    ariaRetryCount++;
    qWarning().noquote() << "⚠️ aria2c failed — retrying once...";
    QTimer::singleShot(1000, this, [=]() {
        startSDDownload(); // retente
    });
    return;
}

        // 2) rclone sinon (et il fallback vers HTTPS dans ta fonction)
        const QString rclonePath = QStandardPaths::findExecutable(QStringLiteral("rclone"));
        if (!rclonePath.isEmpty())
        {
            qDebug().noquote() << "🧩 rclone detected → using rclone fallback";
            startRcloneFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
        }
        else
        {
            // 3) HTTP direct si rien
            qWarning().noquote() << "⚠️ Neither aria2c nor rclone found → switching to HTTPS fallback...";
            startHttpFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
        }
    });
}


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
    QStringLiteral("--multi-thread-streams=4"),
    QStringLiteral("--multi-thread-cutoff=16M"),
    QStringLiteral("--buffer-size=128M"),
    QStringLiteral("--transfers=2"),
    QStringLiteral("--low-level-retries=5"),
    QStringLiteral("--checkers=4"),
    QStringLiteral("--retries=3"),
    QStringLiteral("--progress")
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

    // ✅ Extraction dans un dossier temporaire local (dans le dossier Dolphin)
    const QString tmpDir = installationDirectory + QDir::separator() + QStringLiteral("update_tmp");
    const QString zipFile = temporaryDirectory + QDir::separator() + filename;

    if (!QFile::exists(zipFile))
    {
        QMessageBox::critical(this, QStringLiteral("Error"),
                              QStringLiteral("ZIP file missing!"));
        reject();
        return;
    }

    QDir().mkpath(tmpDir);

    label->setText(QStringLiteral("Step 2/2: Installing update..."));
    stepLabel->setText(QStringLiteral("Extracting files..."));
    stepProgressBar->setValue(0);
    progressBar->setValue(75);

    // --- Thread d’extraction ---
    QThread* thread = new QThread(nullptr);

    connect(thread, &QThread::started, this, [this, zipFile, tmpDir, thread]() {
        bool success = unzipFile(zipFile.toStdString(), tmpDir.toStdString(),
            [this](int current, int total)
            {
                const int percent = (total > 0) ? (current * 100 / total) : 0;

                QMetaObject::invokeMethod(QApplication::instance(), [=]() {
                    stepProgressBar->setValue(percent);
                    stepLabel->setText(QStringLiteral("Extracting: %1%").arg(percent));
                    progressBar->setValue(75 + (percent * 0.25));
                }, Qt::QueuedConnection);
            });

        QMetaObject::invokeMethod(QApplication::instance(), [=]() {
            thread->quit();
            thread->deleteLater();

            if (!success)
            {
                QMessageBox::critical(nullptr, QStringLiteral("Error"),
                                      QStringLiteral("Failed to extract ZIP file."));
                return;
            }

            QFile::remove(zipFile);
            qDebug().noquote() << QStringLiteral("✅ ZIP extracted to temporary folder:") << tmpDir;

            stepLabel->setText(QStringLiteral("Finalizing update..."));
            stepProgressBar->setValue(100);
            progressBar->setValue(100);

#ifdef _WIN32
            const QString exe = QDir::toNativeSeparators(
                installationDirectory + QDir::separator() + QStringLiteral("Dolphin.exe"));
#else
            const QString exe = QDir::toNativeSeparators(
                installationDirectory + QDir::separator() + QStringLiteral("Dolphin"));
#endif

            if (!QFile::exists(exe))
            {
                QMessageBox::information(nullptr, QStringLiteral("Done"),
                                         QStringLiteral("Installation finished. Launch manually."));
                return;
            }

            qDebug().noquote() << QStringLiteral("♻️ Preparing for restart...");

            // ✅ Ferme la fenêtre de l’installeur
            this->accept();

#ifdef _WIN32
// ⚙️ Script PowerShell pour remplacer tous les fichiers pendant que Dolphin est fermé
QString psScript = QStringLiteral(R"(
    $tmp = "%1";
    $dest = "%2";

    Start-Sleep -Seconds 1;

    # 🔪 Ferme tout process Dolphin encore actif
    Get-Process "Dolphin" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue;
    Start-Sleep -Seconds 1;

    Write-Host "🚚 Moving update files from $tmp to $dest...";
    robocopy $tmp $dest /E /MOVE /R:3 /W:1 | Out-Null;

    # 🧹 Supprime le dossier temporaire si encore là
    if (Test-Path $tmp) { Remove-Item -Path $tmp -Recurse -Force -ErrorAction SilentlyContinue }

    Write-Host "✅ Update applied. Restarting Dolphin...";
    Start-Process "$dest\\Dolphin.exe";
)") // fin du script inline PowerShell
.arg(QDir::toNativeSeparators(tmpDir),
     QDir::toNativeSeparators(installationDirectory));

qDebug().noquote() << QStringLiteral("🚀 Launching PowerShell update finalizer...");

QProcess::startDetached(
    QStringLiteral("powershell.exe"),
    QStringList()
        << QStringLiteral("-NoProfile")
        << QStringLiteral("-ExecutionPolicy") << QStringLiteral("Bypass")
        << QStringLiteral("-Command") << psScript
);
#else
// 🐧 Linux/macOS : même principe
QProcess::startDetached(
    QStringLiteral("/bin/bash"),
    {QStringLiteral("-c"),
     QStringLiteral("sleep 1 && mv -f '%1'/* '%2'/ && rm -rf '%1' && '%2'/Dolphin &")
         .arg(temporaryDirectory)
         .arg(installationDirectory)});
);
#endif

// ✅ Quitte Dolphin immédiatement pour permettre la copie
QCoreApplication::quit();

        }, Qt::QueuedConnection);
    });

    thread->start();
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

    // 🔹 Compte total des fichiers dans l’archive
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

    // ⚙️ Configuration de l’animation fluide de la barre globale (progressBar)
    static double extractDisplayed = 50.0;
    static double extractTarget = 50.0;
    static QTimer* extractTimer = nullptr;

    if (!extractTimer)
    {
        extractTimer = new QTimer(this);
        extractTimer->setInterval(30); // environ 33 FPS
        connect(extractTimer, &QTimer::timeout, this, [this]() {
            // interpolation douce vers la cible
            extractDisplayed += (extractTarget - extractDisplayed) * 0.15;
            progressBar->setValue(static_cast<int>(extractDisplayed));
        });
        extractTimer->start();
    }

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
            int percent = (total > 0) ? (current * 100 / total) : 0;

            // 🔹 Barre d’étape (en bas)
            stepProgressBar->setValue(percent);
            stepLabel->setText(QStringLiteral("Extracting: %1%").arg(percent));

            // 🔹 Barre globale (fluide) → 50 → 100 %
            extractTarget = 50.0 + (percent * 0.5);

            // Callback éventuel (si défini ailleurs)
            if (progressCallback)
                progressCallback(current, total);
        }

        mz_zip_reader_entry_close(reader);
        err = mz_zip_reader_goto_next_entry(reader);
    }

    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);

    // 🔚 Termine proprement la progression à 100 %
     if (extractTimer)
    {
        extractTimer->stop();
        extractTimer->deleteLater();
        extractTimer = nullptr;
    }

    progressBar->setValue(100);
    stepProgressBar->setValue(100);
    stepLabel->setText(QStringLiteral("Extraction complete"));

    return true;
}


void InstallUpdateDialog::closeEvent(QCloseEvent* event)
{
    if (m_isClosing)
        return;

    m_isClosing = true;
    qDebug().noquote() << "🛑 User requested to close update dialog.";

    setEnabled(false);

    // Annule tout ce qui pourrait déclencher un invokeMethod(this, …)
    disconnect(this, nullptr, nullptr, nullptr);

    // 🔹 Tuer tous les sous-processus (aria2c, rclone, PowerShell, etc.)
    const auto processes = findChildren<QProcess*>();
    for (QProcess* p : processes)
    {
        if (p && p->state() != QProcess::NotRunning)
        {
            qDebug().noquote() << "🧨 Killing process:" << p->program();
            disconnect(p, nullptr, this, nullptr);
            p->kill();
            if (!p->waitForFinished(1000))
                p->terminate();
        }
    }

    // 🔹 Stoppe proprement les threads
    const auto threads = findChildren<QThread*>();
    for (QThread* t : threads)
    {
        if (t && t->isRunning())
        {
            qDebug().noquote() << "🧵 Stopping thread:" << t;
            disconnect(t, nullptr, this, nullptr);
            t->requestInterruption();
            t->quit();
            t->wait(1000);
        }
    }

    // 🔹 Nettoyage des fichiers temporaires rclone (sd.raw.*)
{
    const QString userWiiPath = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QStringLiteral("/User/Wii"));
    QDir dir(userWiiPath);

    QStringList partials = dir.entryList(QStringList() << QStringLiteral("sd.raw.*"), QDir::Files);
    for (const QString& file : partials)
    {
        QString fullPath = dir.filePath(file);
        qDebug().noquote() << "🧹 Removing leftover partial file:" << fullPath;
        QFile::remove(fullPath);
    }
}

    // 🔹 Supprime les timers (pour éviter les callbacks post-fermeture)
    const auto timers = findChildren<QTimer*>();
    for (QTimer* timer : timers)
    {
        if (timer)
        {
            qDebug().noquote() << "⏱️ Deleting timer";
            timer->stop();
            disconnect(timer, nullptr, this, nullptr);
            timer->deleteLater();
        }
    }

    qDebug().noquote() << "✅ Update dialog closed safely.";
    QDialog::closeEvent(event);
}


