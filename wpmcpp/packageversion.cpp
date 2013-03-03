#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wininet.h>
#include <stdlib.h>
#include <time.h>

#include <QUrl>
#include <QDebug>
#include <QIODevice>
#include <QProcess>
#include <QDomElement>
#include <QDomDocument>
#include <QMutex>
#include <zlib.h>

#include "quazip.h"
#include "quazipfile.h"

#include "repository.h"
#include "packageversion.h"
#include "job.h"
#include "downloader.h"
#include "wpmutils.h"
#include "repository.h"
#include "version.h"
#include "windowsregistry.h"
#include "xmlutils.h"
#include "installedpackages.h"
#include "installedpackageversion.h"
#include "dbrepository.h"

QSemaphore PackageVersion::httpConnections(3);
QSemaphore PackageVersion::installationScripts(1);
QSet<QString> PackageVersion::lockedPackageVersions;
QMutex PackageVersion::lockedPackageVersionsMutex(QMutex::Recursive);

QString PackageVersion::getStringId(const QString& package,
    const Version& version)
{
    return package + "/" + version.getVersionString();
}

int PackageVersion::indexOf(const QList<PackageVersion*> pvs, PackageVersion* f)
{
    int r = -1;
    for (int i = 0; i < pvs.count(); i++) {
        PackageVersion* pv = pvs.at(i);
        if (pv->package == f->package && pv->version == f->version) {
            r = i;
            break;
        }
    }
    return r;
}

PackageVersion* PackageVersion::findLockedPackageVersion()
{
    PackageVersion* r = 0;

    lockedPackageVersionsMutex.lock();
    QSetIterator<QString> i(lockedPackageVersions);
    QString key;
    if (i.hasNext()) {
        key = i.next();
    }
    lockedPackageVersionsMutex.unlock();

    if (!key.isEmpty()) {
        QStringList parts = key.split('/');
        if (parts.count() == 2) {
            QString package = parts.at(0);
            Version version;
            if (version.setVersion(parts.at(1))) {
                DBRepository* rep = DBRepository::getDefault();
                r = rep->findPackageVersion_(package, version);
            }
        }
    }
    return r;
}

PackageVersion::PackageVersion(const QString& package)
{
    this->package = package;
    this->type = 0;
}

PackageVersion::PackageVersion()
{
    this->package = "unknown";
    this->type = 0;
}

void PackageVersion::emitStatusChanged()
{
    //Repository* r = Repository::getDefault();
    // TODO: r->fireStatusChanged(this);
}

void PackageVersion::lock()
{
    QString key = getStringId();

    bool changed = false;
    lockedPackageVersionsMutex.lock();
    if (!lockedPackageVersions.contains(key)) {
        lockedPackageVersions.insert(key);
        changed = true;
    }
    lockedPackageVersionsMutex.unlock();

    if (changed)
        emitStatusChanged();
}

void PackageVersion::unlock()
{
    QString key = getStringId();

    bool changed = false;
    lockedPackageVersionsMutex.lock();
    if (lockedPackageVersions.contains(key)) {
        lockedPackageVersions.remove(key);
        changed = true;
    }
    lockedPackageVersionsMutex.unlock();

    if (changed)
        emitStatusChanged();
}

bool PackageVersion::isLocked() const
{
    bool r;
    lockedPackageVersionsMutex.lock();
    r = lockedPackageVersions.contains(getStringId());
    lockedPackageVersionsMutex.unlock();
    return r;
}

QString PackageVersion::getStringId() const
{
    return getStringId(this->package, this->version);
}

void PackageVersion::fillFrom(PackageVersion *pv)
{
    this->package = pv->package;
    this->version = pv->version;
    this->importantFiles = pv->importantFiles;
    this->importantFilesTitles = pv->importantFilesTitles;
    this->type = pv->type;
    this->sha1 = pv->sha1;
    this->download = pv->download;
    this->msiGUID = pv->msiGUID;

    qDeleteAll(this->files);
    this->files.clear();
    for (int i = 0; i < pv->files.count(); i++) {
        this->files.append(pv->files.at(i)->clone());
    }

    qDeleteAll(this->detectFiles);
    this->detectFiles.clear();
    for (int i = 0; i < pv->detectFiles.count(); i++) {
        this->detectFiles.append(pv->detectFiles.at(i)->clone());
    }

    qDeleteAll(this->dependencies);
    this->dependencies.clear();
    for (int i = 0; i < pv->dependencies.count(); i++) {
        this->dependencies.append(pv->dependencies.at(i)->clone());
    }
}

QString PackageVersion::getPath() const
{
    return InstalledPackages::getDefault()->
            getPath(this->package, this->version);
}

void PackageVersion::setPath(const QString& path)
{
    //qDebug() << "PackageVersion::setPath " << path;
    InstalledPackages* ip = InstalledPackages::getDefault();
    ip->setPackageVersionPath(this->package, this->version, path);
}

bool PackageVersion::isDirectoryLocked()
{
    if (installed()) {
        QDir d(getPath());
        QDateTime now = QDateTime::currentDateTime();
        QString newName = QString("%1-%2").arg(d.absolutePath()).arg(now.toTime_t());

        if (!d.rename(d.absolutePath(), newName)) {
            return true;
        }

        if (!d.rename(newName, d.absolutePath())) {
            return true;
        }
    }

    return false;
}


QString PackageVersion::toString()
{
    return this->getPackageTitle() + " " + this->version.getVersionString();
}

QString PackageVersion::getShortPackageName()
{
    QStringList sl = this->package.split(".");
    return sl.last();
}

PackageVersion::~PackageVersion()
{
    qDeleteAll(this->detectFiles);
    qDeleteAll(this->files);
    qDeleteAll(this->dependencies);
}

bool PackageVersion::installed() const
{
    return InstalledPackages::getDefault()->isInstalled(this->package,
            this->version);
}

void PackageVersion::deleteShortcuts(const QString& dir, Job* job,
        bool menu, bool desktop, bool quickLaunch)
{
    if (menu) {
        job->setHint("Start menu");
        QDir d(WPMUtils::getShellDir(CSIDL_STARTMENU));
        WPMUtils::deleteShortcuts(dir, d);

        QDir d2(WPMUtils::getShellDir(CSIDL_COMMON_STARTMENU));
        WPMUtils::deleteShortcuts(dir, d2);
    }
    job->setProgress(0.33);

    if (desktop) {
        job->setHint("Desktop");
        QDir d3(WPMUtils::getShellDir(CSIDL_DESKTOP));
        WPMUtils::deleteShortcuts(dir, d3);

        QDir d4(WPMUtils::getShellDir(CSIDL_COMMON_DESKTOPDIRECTORY));
        WPMUtils::deleteShortcuts(dir, d4);
    }
    job->setProgress(0.66);

    if (quickLaunch) {
        job->setHint("Quick launch bar");
        const char* A = "\\Microsoft\\Internet Explorer\\Quick Launch";
        QDir d3(WPMUtils::getShellDir(CSIDL_APPDATA) + A);
        WPMUtils::deleteShortcuts(dir, d3);

        QDir d4(WPMUtils::getShellDir(CSIDL_COMMON_APPDATA) + A);
        WPMUtils::deleteShortcuts(dir, d4);
    }
    job->setProgress(1);

    job->complete();
}

void PackageVersion::uninstall(Job* job)
{
    if (!installed()) {
        job->setProgress(1);
        job->complete();
        return;
    }

    QDir d(getPath());

    if (job->getErrorMessage().isEmpty()) {
        job->setHint("Deleting shortcuts");
        Job* sub = job->newSubJob(0.20);
        deleteShortcuts(d.absolutePath(), sub, true, false, false);
        delete sub;
    }

    QString uninstallationScript;
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        uninstallationScript = ".Npackd\\Uninstall.bat";
        if (!QFile::exists(d.absolutePath() +
                "\\" + uninstallationScript)) {
            uninstallationScript = ".WPM\\Uninstall.bat";
            if (!QFile::exists(d.absolutePath() +
                    "\\" + uninstallationScript)) {
                uninstallationScript = "";
            }
        }
    }

    bool uninstallationScriptAcquired = false;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (!uninstallationScript.isEmpty()) {
            job->setHint("Waiting while other (un)installation "
                    "scripts are running");

            time_t start = time(NULL);
            while (!job->isCancelled()) {
                uninstallationScriptAcquired = installationScripts.
                        tryAcquire(1, 10000);
                if (uninstallationScriptAcquired) {
                    job->setProgress(0.25);
                    break;
                }

                time_t seconds = time(NULL) - start;
                job->setHint(QString(
                        "Waiting while other (un)installation scripts are "
                        "running (%1 minutes)").
                        arg(seconds / 60));
            }
        } else {
            job->setProgress(0.25);
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (!uninstallationScript.isEmpty()) {
            job->setHint("Running the uninstallation script "
                    "(this may take some time)");
            if (!d.exists(".Npackd"))
                d.mkdir(".Npackd");
            Job* sub = job->newSubJob(0.20);

            // prepare the environment variables
            QStringList env;
            env.append("NPACKD_PACKAGE_NAME");
            env.append(this->package);
            env.append("NPACKD_PACKAGE_VERSION");
            env.append(this->version.getVersionString());
            env.append("NPACKD_CL");
            env.append(AbstractRepository::getDefault_()->
                    computeNpackdCLEnvVar_());

            addDependencyVars(&env);

            QString output = this->executeFile(sub, d.absolutePath(),
                    uninstallationScript, ".Npackd\\Uninstall.log", env);
            if (!sub->getErrorMessage().isEmpty()) {
                QString err = sub->getErrorMessage();
                err.append("\n");
                err.append(output);
                job->setErrorMessage(err);
            }
            delete sub;
        }
        job->setProgress(0.45);
    }

    if (uninstallationScriptAcquired)
        installationScripts.release();

    // Uninstall.bat may have deleted some files
    d.refresh();

    if (job->getErrorMessage().isEmpty()) {
        if (d.exists()) {
            job->setHint("Deleting files");
            Job* rjob = job->newSubJob(0.54);
            removeDirectory(rjob, d.absolutePath());
            if (!rjob->getErrorMessage().isEmpty())
                job->setErrorMessage(rjob->getErrorMessage());
            else {
                setPath("");
            }
            delete rjob;
        }

        if (this->package == "com.googlecode.windows-package-manager.NpackdCL" ||
                this->package == "com.googlecode.windows-package-manager.NpackdCL64") {
            job->setHint("Updating NPACKD_CL");
            AbstractRepository::getDefault_()->updateNpackdCLEnvVar();
        }
        job->setProgress(1);
    }

    job->complete();
}

void PackageVersion::removeDirectory(Job* job, const QString& dir)
{
    QDir d(dir);

    WPMUtils::moveToRecycleBin(d.absolutePath());
    job->setProgress(0.3);

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        d.refresh();
        if (d.exists()) {
            Sleep(5000); // 5 Seconds
            WPMUtils::moveToRecycleBin(d.absolutePath());
        }
        job->setProgress(0.6);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        d.refresh();
        if (d.exists()) {
            Sleep(5000); // 5 Seconds
            QString oldName = d.dirName();
            if (!d.cdUp())
                job->setErrorMessage("Cannot change directory to " +
                        d.absolutePath() + "\\..");
            else {
                QString trash = ".NpackdTrash";
                if (!d.exists(trash)) {
                    if (!d.mkdir(trash))
                        job->setErrorMessage(QString(
                                "Cannot create directory %0\%1").
                                arg(d.absolutePath()).arg(trash));
                }
                if (d.exists(trash)) {
                    QString nn = trash + "\\" + oldName + "_%1";
                    int i = 0;
                    while (true) {
                        QString newName = nn.arg(i);
                        if (!d.exists(newName)) {
                            if (!d.rename(oldName, newName)) {
                                job->setErrorMessage(QString(
                                        "Cannot rename %1 to %2 in %3").
                                        arg(oldName).arg(newName).
                                        arg(d.absolutePath()));
                            }
                            break;
                        } else {
                            i++;
                        }
                    }
                }
            }
        }
        job->setProgress(1);
    }

    job->complete();
}

QString PackageVersion::planInstallation(QList<PackageVersion*>& installed,
        QList<InstallOperation*>& ops, QList<PackageVersion*>& avoid)
{
    QString res;

    avoid.append(this->clone());

    for (int i = 0; i < this->dependencies.count(); i++) {
        Dependency* d = this->dependencies.at(i);
        bool depok = false;
        for (int j = 0; j < installed.size(); j++) {
            PackageVersion* pv = installed.at(j);
            if ((pv->package != this->package || pv->version != this->version) &&
                    pv->package == d->package &&
                    d->test(pv->version)) {
                depok = true;
                break;
            }
        }
        if (!depok) {
            QScopedPointer<PackageVersion> pv(d->findBestMatchToInstall(avoid));
            if (!pv) {
                res = QString("Unsatisfied dependency: %1").
                           arg(d->toString());
                break;
            } else {
                res = pv->planInstallation(installed, ops, avoid);
                if (!res.isEmpty())
                    break;
            }
        }
    }

    if (res.isEmpty()) {
        InstallOperation* io = new InstallOperation();
        io->install = true;
        io->package = this->package;
        io->version = this->version;
        ops.append(io);
        installed.append(this->clone());
    }

    return res;
}

QString PackageVersion::planUninstallation(QList<PackageVersion*>& installed,
        QList<InstallOperation*>& ops)
{
    // qDebug() << "PackageVersion::planUninstallation()" << this->toString();
    QString res;

    if (!PackageVersion::contains(installed, this))
        return res;

    // this loop ensures that all the items in "installed" are processed
    // even if changes in the list were done in nested calls to
    // "planUninstallation"
    while (true) {
        int oldCount = installed.count();
        for (int i = 0; i < installed.count(); i++) {
            PackageVersion* pv = installed.at(i);
            if (pv->package != this->package || pv->version != this->version) {
                for (int j = 0; j < pv->dependencies.count(); j++) {
                    Dependency* d = pv->dependencies.at(j);
                    if (d->package == this->package && d->test(this->version)) {
                        int n = 0;
                        for (int k = 0; k < installed.count(); k++) {
                            PackageVersion* pv2 = installed.at(k);
                            if (d->package == pv2->package &&
                                    d->test(pv2->version)) {
                                n++;
                            }
                            if (n > 1)
                                break;
                        }
                        if (n <= 1) {
                            res = pv->planUninstallation(installed, ops);
                            if (!res.isEmpty())
                                break;
                        }
                    }
                }
                if (!res.isEmpty())
                    break;
            }
        }

        if (oldCount == installed.count() || !res.isEmpty())
            break;
    }

    if (res.isEmpty()) {
        InstallOperation* op = new InstallOperation();
        op->install = false;
        op->package = this->package;
        op->version = this->version;
        ops.append(op);
        installed.removeAt(PackageVersion::indexOf(installed, this));
    }

    return res;
}

void PackageVersion::downloadTo(Job* job, QString filename)
{
    if (!this->download.isValid()) {
        job->setErrorMessage("No download URL");
        job->complete();
        return;
    }

    QString r;

    job->setHint("Downloading");
    QTemporaryFile* f = 0;
    Job* djob = job->newSubJob(0.9);
    f = Downloader::download(djob, this->download);
    if (!djob->getErrorMessage().isEmpty())
        job->setErrorMessage(QString("Download failed: %1").arg(
                djob->getErrorMessage()));
    delete djob;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Computing SHA1");
        r = WPMUtils::sha1(f->fileName());
        job->setProgress(0.95);

        if (!this->sha1.isEmpty() && this->sha1.toLower() != r.toLower()) {
            job->setErrorMessage(QString(
                    "Wrong SHA1: %1 was expected, but %2 found").
                    arg(this->sha1).arg(r));
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        QString errMsg;
        QString t = filename.replace('/', '\\');
        if (!CopyFileW((WCHAR*) f->fileName().utf16(),
                       (WCHAR*) t.utf16(), false)) {
            WPMUtils::formatMessage(GetLastError(), &errMsg);
            job->setErrorMessage(errMsg);
        } else {
            job->setProgress(1);
        }
    }

    delete f;

    job->complete();
}

QString PackageVersion::getFileExtension()
{
    if (this->download.isValid()) {
        QString fn = this->download.path();
        QStringList parts = fn.split('/');
        QString file = parts.at(parts.count() - 1);
        int index = file.lastIndexOf('.');
        if (index > 0)
            return file.right(file.length() - index);
        else
            return ".bin";
    } else {
        return ".bin";
    }
}

QString PackageVersion::downloadAndComputeSHA1(Job* job)
{
    if (!this->download.isValid()) {
        job->setErrorMessage("No download URL");
        job->complete();
        return "";
    }

    QString r;

    job->setHint("Downloading");
    QTemporaryFile* f = 0;
    Job* djob = job->newSubJob(0.95);
    f = Downloader::download(djob, this->download);
    if (!djob->getErrorMessage().isEmpty())
        job->setErrorMessage(QString("Download failed: %1").arg(
                djob->getErrorMessage()));
    delete djob;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Computing SHA1");
        r = WPMUtils::sha1(f->fileName());
        job->setProgress(1);
    }

    if (f)
        delete f;

    job->complete();

    return r;
}

QString PackageVersion::getPackageTitle() const
{
    AbstractRepository* rep = AbstractRepository::getDefault_();

    QString pn;
    Package* package = rep->findPackage_(this->package);
    if (package)
        pn = package->title;
    else
        pn = this->package;
    delete package;

    return pn;
}

bool PackageVersion::createShortcuts(const QString& dir, QString *errMsg)
{
    *errMsg = "";

    QString packageTitle = this->getPackageTitle();

    QDir d(dir);
    Package* p = AbstractRepository::getDefault_()->findPackage_(this->package);
    for (int i = 0; i < this->importantFiles.count(); i++) {
        QString ifile = this->importantFiles.at(i);
        QString ift = this->importantFilesTitles.at(i);

        QString path(ifile);
        path.prepend("\\");
        path.prepend(d.absolutePath());
        path.replace('/' , '\\');

        if (!d.exists(path)) {
            *errMsg = QString("Shortcut target %1 does not exist").arg(path);
            break;
        }

        QString workingDir = path;
        int pos = workingDir.lastIndexOf('\\');
        workingDir.chop(workingDir.length() - pos);

        QString simple = ift;
        QString withVersion = simple + " " + this->version.getVersionString();
        if (packageTitle != ift) {
            QString suffix = " (" + packageTitle + ")";
            simple.append(suffix);
            withVersion.append(suffix);
        }

        simple = WPMUtils::makeValidFilename(simple, ' ') + ".lnk";
        withVersion = WPMUtils::makeValidFilename(withVersion, ' ') + "%1.lnk";
        QString commonStartMenu = WPMUtils::getShellDir(CSIDL_COMMON_STARTMENU);
        simple = commonStartMenu + "\\" + simple;
        withVersion = commonStartMenu + "\\" + withVersion;

        QString from;
        if (QFileInfo(simple).exists())
            from = WPMUtils::findNonExistingFile(withVersion);
        else
            from = simple;

        // qDebug() << "createShortcuts " << ifile << " " << p << " " <<
        //         from;

        QString desc;
        if (p)
            desc = p->description;
        if (desc.isEmpty())
            desc = this->package;
        HRESULT r = WPMUtils::createLink(
                (WCHAR*) path.replace('/', '\\').utf16(),
                (WCHAR*) from.utf16(),
                (WCHAR*) desc.utf16(),
                (WCHAR*) workingDir.utf16());

        if (!SUCCEEDED(r)) {
            *errMsg = QString("Shortcut creation from %1 to %2 failed").
                    arg(from).arg(path);
            break;
        }
    }
    delete p;

    return errMsg->isEmpty();
}

QString PackageVersion::getPreferredInstallationDirectory()
{
    QString name = WPMUtils::getInstallationDirectory() + "\\" +
            WPMUtils::makeValidFilename(this->getPackageTitle(), '_');
    if (!QFileInfo(name).exists())
        return name;
    else
        return WPMUtils::findNonExistingFile(name + "-" +
                this->version.getVersionString() + "%1");
}

void PackageVersion::install(Job* job, const QString& where)
{
    job->setHint("Preparing");

    if (installed()) {
        job->setProgress(1);
        job->complete();
        return;
    }

    if (!this->download.isValid()) {
        job->setErrorMessage("No download URL");
        job->complete();
        return;
    }

    // qDebug() << "install.2";
    QDir d(where);
    QString npackdDir = where + "\\.Npackd";

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Creating directory");
        QString s = d.absolutePath();
        if (!d.mkpath(s)) {
            job->setErrorMessage(QString("Cannot create directory: %0").
                    arg(s));
        } else {
            job->setProgress(0.01);
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Creating .Npackd sub-directory");
        QString s = npackdDir;
        if (!d.mkpath(s)) {
            job->setErrorMessage(QString("Cannot create directory: %0").
                    arg(s));
        } else {
            job->setProgress(0.02);
        }
    }

    bool httpConnectionAcquired = false;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Waiting for a free HTTP connection");

        time_t start = time(NULL);
        while (!job->isCancelled()) {
            httpConnectionAcquired = httpConnections.tryAcquire(1, 10000);
            if (httpConnectionAcquired) {
                job->setProgress(0.05);
                break;
            }

            time_t seconds = time(NULL) - start;
            job->setHint(QString(
                    "Waiting for a free HTTP connection (%1 minutes)").
                    arg(seconds / 60));
        }
    }

    // qDebug() << "install.3";
    QFile* f = new QFile(npackdDir + "\\__NpackdPackageDownload");

    bool downloadOK = false;
    QString dsha1;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Downloading & computing hash sum");
        if (!f->open(QIODevice::ReadWrite)) {
            job->setErrorMessage(QString("Cannot open the file: %0").
                    arg(f->fileName()));
        } else {
            Job* djob = job->newSubJob(0.58);
            Downloader::download(djob, this->download, f,
                    this->sha1.isEmpty() ? 0 : &dsha1);
            downloadOK = !djob->isCancelled() && djob->getErrorMessage().isEmpty();
            f->close();
            delete djob;
        }
    }

    if (httpConnectionAcquired)
        httpConnections.release();

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (!downloadOK) {
            if (!f->open(QIODevice::ReadWrite)) {
                job->setErrorMessage(QString("Cannot open the file: %0").
                        arg(f->fileName()));
            } else {
                job->setHint("Downloading & computing hash sum (2nd try)");
                double rest = 0.63 - job->getProgress();
                Job* djob = job->newSubJob(rest);
                Downloader::download(djob, this->download, f,
                        this->sha1.isEmpty() ? 0 : &dsha1);
                if (!djob->getErrorMessage().isEmpty())
                    job->setErrorMessage(djob->getErrorMessage());
                f->close();
                delete djob;
            }
        } else {
            job->setProgress(0.63);
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (!this->sha1.isEmpty()) {
            if (dsha1.toLower() != this->sha1.toLower()) {
                job->setErrorMessage(QString(
                        "Hash sum (SHA1) %1 found, but %2 "
                        "was expected. The file has changed.").arg(dsha1).
                        arg(this->sha1));
            } else {
                job->setProgress(0.64);
            }
        } else {
            job->setProgress(0.64);
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (this->type == 0) {
            job->setHint("Extracting files");
            Job* djob = job->newSubJob(0.11);
            unzip(djob, f->fileName(), d.absolutePath() + "\\");
            if (!djob->getErrorMessage().isEmpty())
                job->setErrorMessage(QString(
                        "Error unzipping file into directory %0: %1").
                        arg(d.absolutePath()).
                        arg(djob->getErrorMessage()));
            else if (!job->isCancelled())
                job->setProgress(0.75);
            delete djob;
        } else {
            job->setHint("Renaming the downloaded file");
            QString t = d.absolutePath();
            t.append("\\");
            QString fn = this->download.path();
            QStringList parts = fn.split('/');
            t.append(parts.at(parts.count() - 1));
            t.replace('/', '\\');

            if (!QFile::rename(f->fileName(), t)) {
                job->setErrorMessage(QString("Cannot rename %0 to %1").
                        arg(f->fileName()).arg(t));
            } else {
                job->setProgress(0.75);
            }
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        QString errMsg = this->saveFiles(d);
        if (!errMsg.isEmpty()) {
            job->setErrorMessage(errMsg);
        } else {
            job->setProgress(0.85);
        }
    }

    QString installationScript;
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        installationScript = ".Npackd\\Install.bat";
        if (!QFile::exists(d.absolutePath() +
                "\\" + installationScript)) {
            installationScript = ".WPM\\Install.bat";
            if (!QFile::exists(d.absolutePath() +
                    "\\" + installationScript)) {
                installationScript = "";
            }
        }
    }

    bool installationScriptAcquired = false;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (!installationScript.isEmpty()) {
            job->setHint("Waiting while other (un)installation "
                    "scripts are running");

            time_t start = time(NULL);
            while (!job->isCancelled()) {
                installationScriptAcquired = installationScripts.
                        tryAcquire(1, 10000);
                if (installationScriptAcquired) {
                    job->setProgress(0.86);
                    break;
                }

                time_t seconds = time(NULL) - start;
                job->setHint(QString(
                        "Waiting while other (un)installation scripts are "
                        "running (%1 minutes)").
                        arg(seconds / 60));
            }
        } else {
            job->setProgress(0.86);
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (!installationScript.isEmpty()) {
            job->setHint("Running the installation script "
                    "(this may take some time)");
            Job* exec = job->newSubJob(0.09);
            if (!d.exists(".Npackd"))
                d.mkdir(".Npackd");

            QStringList env;
            env.append("NPACKD_PACKAGE_NAME");
            env.append(this->package);
            env.append("NPACKD_PACKAGE_VERSION");
            env.append(this->version.getVersionString());
            env.append("NPACKD_CL");
            env.append(AbstractRepository::getDefault_()->
                    computeNpackdCLEnvVar_());

            addDependencyVars(&env);

            QString output = this->executeFile(exec, d.absolutePath(),
                    installationScript, ".Npackd\\Install.log", env);
            if (!exec->getErrorMessage().isEmpty()) {
                QString err = exec->getErrorMessage();
                err.append("\n");
                err.append(output);
                job->setErrorMessage(err);
            } else {
                QString path = d.absolutePath();
                path.replace('/', '\\');
                setPath(path);
            }
            delete exec;
        } else {
            QString path = d.absolutePath();
            path.replace('/', '\\');
            setPath(path);
        }

        if (this->package == "com.googlecode.windows-package-manager.NpackdCL" ||
                this->package == "com.googlecode.windows-package-manager.NpackdCL64") {
            job->setHint("Updating NPACKD_CL");
            AbstractRepository::getDefault_()->updateNpackdCLEnvVar();
        }

        job->setProgress(0.95);
    }

    if (installationScriptAcquired)
        installationScripts.release();

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        QString err;
        this->createShortcuts(d.absolutePath(), &err);
        if (err.isEmpty())
            job->setProgress(0.97);
        else
            job->setErrorMessage(err);
    }

    if (!job->getErrorMessage().isEmpty() || job->isCancelled()) {
        job->setHint(QString(
                "Deleting start menu, desktop and quick launch shortcuts"));
        Job* sub = new Job();
        deleteShortcuts(d.absolutePath(), sub, true, true, true);
        delete sub;

        job->setHint(QString("Deleting files"));
        Job* rjob = new Job();
        removeDirectory(rjob, d.absolutePath());
        delete rjob;
    }

    if (f && f->exists())
        f->remove();

    delete f;

    job->complete();
}

void PackageVersion::addDependencyVars(QStringList* vars)
{
    for (int i = 0; i < this->dependencies.count(); i++) {
        Dependency* d = this->dependencies.at(i);
        if (!d->var.isEmpty()) {
            vars->append(d->var);
            InstalledPackageVersion* ipv = d->findHighestInstalledMatch();
            if (ipv) {
                vars->append(ipv->getDirectory());
            } else {
                // this could happen if a package was un-installed manually
                // without Npackd or the repository has changed after this
                // package was installed
                vars->append("");
            }
        }
    }
}

void PackageVersion::unzip(Job* job, QString zipfile, QString outputdir)
{
    job->setHint("Opening ZIP file");
    QuaZip zip(zipfile);
    if (!zip.open(QuaZip::mdUnzip)) {
        job->setErrorMessage(QString("Cannot open the ZIP file %1: %2").
                       arg(zipfile).arg(zip.getZipError()));
    } else {
        job->setProgress(0.01);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Extracting");
        QuaZipFile file(&zip);
        int n = zip.getEntriesCount();
        int blockSize = 1024 * 1024;
        char* block = new char[blockSize];
        int i = 0;
        for (bool more = zip.goToFirstFile(); more; more = zip.goToNextFile()) {
            QString name = zip.getCurrentFileName();
            if (!file.open(QIODevice::ReadOnly)) {
                job->setErrorMessage(QString(
                        "Error unzipping the file %1: Error %2 in %3").
                        arg(zipfile).arg(file.getZipError()).
                        arg(name));
                break;
            }
            name.prepend(outputdir);
            QFile meminfo(name);
            QFileInfo infofile(meminfo);
            QDir dira(infofile.absolutePath());
            if (dira.mkpath(infofile.absolutePath())) {
                if (meminfo.open(QIODevice::ReadWrite)) {
                    while (true) {
                        qint64 read = file.read(block, blockSize);
                        if (read <= 0)
                            break;
                        meminfo.write(block, read);
                    }
                    meminfo.close();
                }
            } else {
                job->setErrorMessage(QString("Cannot create directory %1").arg(
                        infofile.absolutePath()));
                file.close();
            }
            file.close(); // do not forget to close!
            i++;
            job->setProgress(0.01 + 0.99 * i / n);
            if (i % 100 == 0)
                job->setHint(QString("%L1 files").arg(i));

            if (job->isCancelled() || !job->getErrorMessage().isEmpty())
                break;
        }
        zip.close();

        delete[] block;
    }

    job->complete();
}

QString PackageVersion::saveFiles(const QDir& d)
{
    QString res;
    for (int i = 0; i < this->files.count(); i++) {
        PackageVersionFile* f = this->files.at(i);
        QString fullPath = d.absolutePath() + "\\" + f->path;
        QString fullDir = WPMUtils::parentDirectory(fullPath);
        if (d.mkpath(fullDir)) {
            QFile file(fullPath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QTextStream stream(&file);
                stream.setCodec("UTF-8");
                stream << f->content;
                file.close();
            } else {
                res = QString("Could not create file %1").arg(
                        fullPath);
                break;
            }
        } else {
            res = QString("Could not create directory %1").arg(
                    fullDir);
            break;
        }
    }
    return res;
}

QString PackageVersion::getStatus() const
{
    QString status;
    bool installed = this->installed();
    AbstractRepository* r = AbstractRepository::getDefault_();
    PackageVersion* newest = r->findNewestInstallablePackageVersion_(
            this->package);
    if (installed) {
        status = "installed";
    }
    if (installed && newest != 0 && version.compare(newest->version) < 0) {
        if (!newest->installed())
            status += ", updateable";
        else
            status += ", obsolete";
    }

    if (isLocked()) {
        if (!status.isEmpty())
            status = ", " + status;
        status = "locked" + status;
    }

    delete newest;

    return status;
}

QStringList PackageVersion::findLockedFiles()
{
    QStringList r;
    if (installed()) {
        QStringList files = WPMUtils::getProcessFiles();
        QString dir(getPath());
        for (int i = 0; i < files.count(); i++) {
            if (WPMUtils::isUnder(files.at(i), dir)) {
                r.append(files.at(i));
            }
        }
    }
    return r;
}

QString PackageVersion::executeFile(Job* job, const QString& where,
        const QString& path,
        const QString& outputFile, const QStringList& env)
{
    QString ret;

    QDir d(where);
    QProcess p(0);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.setWorkingDirectory(d.absolutePath());

    QString exe = WPMUtils::findCmdExe();
    QString file = d.absolutePath() + "\\" + path;
    file.replace('/', '\\');
    QStringList args;
    /*args.append("/U");
    args.append("/E:ON");
    args.append("/V:OFF");
    args.append("/C");
    args.append(file);
    */
    p.setNativeArguments("/U /E:ON /V:OFF /C \"\"" + file + "\"\"");
    // qDebug() << p.nativeArguments();
    QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
    for (int i = 0; i < env.count(); i += 2) {
        pe.insert(env.at(i), env.at(i + 1));
    }
    p.setProcessEnvironment(pe);
    p.start(exe, args);

    time_t start = time(NULL);
    while (true) {
        if (job->isCancelled()) {
            if (p.state() == QProcess::Running) {
                p.terminate();
                if (p.waitForFinished(10000))
                    break;
                p.kill();
            }
        }
        if (p.waitForFinished(5000) || p.state() == QProcess::NotRunning) {
            job->setProgress(1);
            if (p.exitCode() != 0) {
                job->setErrorMessage(
                        QString("Process %1 exited with the code %2").arg(
                        exe).arg(p.exitCode()));
            }
            QFile f(d.absolutePath() + "\\" + outputFile);
            if (f.open(QIODevice::WriteOnly)) {
                QByteArray output = p.readAll();
                ret.setUtf16((const ushort*) output.constData(),
                        output.size() / 2);
                f.write(output);
                f.close();
            }
            break;
        }
        time_t seconds = time(NULL) - start;
        double percents = ((double) seconds) / 300; // 5 Minutes
        if (percents > 0.9)
            percents = 0.9;
        job->setProgress(percents);
        job->setHint(QString("%1 minutes").arg(seconds / 60));
    }
    job->complete();

    return ret;
}

bool PackageVersion::isInWindowsDir() const
{
    QString dir = WPMUtils::getWindowsDir();
    return this->installed() && (WPMUtils::pathEquals(getPath(), dir) ||
            WPMUtils::isUnder(getPath(), dir));
}

PackageVersion* PackageVersion::clone() const
{
    QDomDocument doc;
    QDomElement version = doc.createElement("version");
    doc.appendChild(version);
    toXML(&version);
    QString err;
    return parse(&version, &err);
}

PackageVersionFile* PackageVersion::createPackageVersionFile(QDomElement* e,
        QString* err)
{
    *err = "";

    QString path = e->attribute("path");
    QString content = e->firstChild().nodeValue();
    PackageVersionFile* a = new PackageVersionFile(path, content);

    return a;
}

DetectFile* PackageVersion::createDetectFile(QDomElement* e, QString* err)
{
    *err = "";

    DetectFile* a = new DetectFile();
    a->path = XMLUtils::getTagContent(*e, "path").trimmed();
    a->path.replace('/', '\\');
    if (a->path.isEmpty()) {
        err->append("Empty tag <path> under <detect-file>");
    }

    if (err->isEmpty()) {
        a->sha1 = XMLUtils::getTagContent(*e, "sha1").trimmed().toLower();
        *err = WPMUtils::validateSHA1(a->sha1);
        if (!err->isEmpty()) {
            err->prepend("Wrong SHA1 in <detect-file>: ");
        }
    }

    if (err->isEmpty())
        return a;
    else {
        delete a;
        return 0;
    }
}

Dependency* PackageVersion::createDependency(QDomElement* e)
{
    // qDebug() << "Repository::createDependency";

    QString package = e->attribute("package").trimmed();

    Dependency* d = new Dependency();
    d->package = package;

    d->var = XMLUtils::getTagContent(*e, "variable");

    if (d->setVersions(e->attribute("versions")))
        return d;
    else {
        delete d;
        return 0;
    }
}

PackageVersion* PackageVersion::parse(QDomElement* e, QString* err)
{
    *err = "";

    // qDebug() << "Repository::createPackageVersion.1" << e->attribute("package");

    QString packageName = e->attribute("package").trimmed();
    *err = WPMUtils::validateFullPackageName(packageName);
    if (!err->isEmpty()) {
        err->prepend("Error in the attribute 'package' in <version>: ");
    }

    PackageVersion* a = new PackageVersion(packageName);

    if (err->isEmpty()) {
        QString url = XMLUtils::getTagContent(*e, "url").trimmed();
        if (!url.isEmpty()) {
            a->download.setUrl(url, QUrl::StrictMode);
            QUrl d = a->download;
            if (!d.isValid() || d.isRelative() ||
                    (d.scheme() != "http" && d.scheme() != "https")) {
                err->append(QString("Not a valid download URL for %1: %2").
                        arg(a->package).arg(url));
            }
        }
    }

    if (err->isEmpty()) {
        QString name = e->attribute("name", "1.0").trimmed();
        if (a->version.setVersion(name)) {
            a->version.normalize();
        } else {
            err->append(QString("Not a valid version for %1: %2").
                    arg(a->package).arg(name));
        }
    }

    if (err->isEmpty()) {
        a->sha1 = XMLUtils::getTagContent(*e, "sha1").trimmed().toLower();
        if (!a->sha1.isEmpty()) {
            *err = WPMUtils::validateSHA1(a->sha1);
            if (!err->isEmpty()) {
                err->prepend(QString("Invalid SHA1 for %1: ").
                        arg(a->toString()));
            }
        }
    }

    if (err->isEmpty()) {
        QString type = e->attribute("type", "zip").trimmed();
        if (type == "one-file")
            a->type = 1;
        else if (type == "" || type == "zip")
            a->type = 0;
        else {
            err->append(QString("Wrong value for the attribute 'type' for %1: %3").
                    arg(a->toString()).arg(type));
        }
    }

    if (err->isEmpty()) {
        QDomNodeList ifiles = e->elementsByTagName("important-file");
        for (int i = 0; i < ifiles.count(); i++) {
            QDomElement e = ifiles.at(i).toElement();
            QString p = e.attribute("path").trimmed();
            if (p.isEmpty())
                p = e.attribute("name").trimmed();

            if (p.isEmpty()) {
                err->append(QString("Empty 'path' attribute value for "
                        "<important-file> for %1").
                        arg(a->toString()));
                break;
            }

            if (a->importantFiles.contains(p)) {
                err->append(QString("More than one <important-file> with "
                        "the same 'path' attribute %1 for %2").
                        arg(p).arg(a->toString()));
                break;
            }

            a->importantFiles.append(p);

            QString title = e.attribute("title").trimmed();
            if (title.isEmpty()) {
                err->append(QString("Empty 'title' attribute value for "
                        "<important-file> for %1").
                        arg(a->toString()));
                break;
            }

            a->importantFilesTitles.append(title);
        }
    }

    if (err->isEmpty()) {
        QDomNodeList files = e->elementsByTagName("file");
        for (int i = 0; i < files.count(); i++) {
            QDomElement e = files.at(i).toElement();
            PackageVersionFile* pvf = createPackageVersionFile(&e, err);
            if (pvf) {
                a->files.append(pvf);
            } else {
                break;
            }
        }
    }

    if (err->isEmpty()) {
        for (int i = 0; i < a->files.count() - 1; i++) {
            PackageVersionFile* fi = a->files.at(i);
            for (int j = i + 1; j < a->files.count(); j++) {
                PackageVersionFile* fj = a->files.at(j);
                if (fi->path == fj->path) {
                    err->append(QString("Duplicate <file> entry for %1 in %2").
                            arg(fi->path).arg(a->toString()));
                    goto out;
                }
            }
        }
    out:;
    }

    if (err->isEmpty()) {
        QDomNodeList detectFiles = e->elementsByTagName("detect-file");
        for (int i = 0; i < detectFiles.count(); i++) {
            QDomElement e = detectFiles.at(i).toElement();
            DetectFile* df = createDetectFile(&e, err);
            if (df) {
                a->detectFiles.append(df);
            } else {
                err->prepend(QString("Invalid <detect-file> for %1: ").
                        arg(a->toString()));
                break;
            }
        }
    }

    if (err->isEmpty()) {
        for (int i = 0; i < a->detectFiles.count() - 1; i++) {
            DetectFile* fi = a->detectFiles.at(i);
            for (int j = i + 1; j < a->detectFiles.count(); j++) {
                DetectFile* fj = a->detectFiles.at(j);
                if (fi->path == fj->path) {
                    err->append(QString(
                            "Duplicate <detect-file> entry for %1 in %2").
                            arg(fi->path).arg(a->toString()));
                    goto out2;
                }
            }
        }
    out2:;
    }

    if (err->isEmpty()) {
        QDomNodeList deps = e->elementsByTagName("dependency");
        for (int i = 0; i < deps.count(); i++) {
            QDomElement e = deps.at(i).toElement();
            Dependency* d = createDependency(&e);
            if (d)
                a->dependencies.append(d);
        }
    }

    if (err->isEmpty()) {
        for (int i = 0; i < a->dependencies.count() - 1; i++) {
            Dependency* fi = a->dependencies.at(i);
            for (int j = i + 1; j < a->dependencies.count(); j++) {
                Dependency* fj = a->dependencies.at(j);
                if (fi->autoFulfilledIf(*fj) ||
                        fj->autoFulfilledIf(*fi)) {
                    err->append(QString("Duplicate <dependency> for %1 in %2").
                            arg(fi->package).arg(a->toString()));
                    goto out3;
                }
            }
        }
    out3:;
    }

    if (err->isEmpty()) {
        a->msiGUID = XMLUtils::getTagContent(*e, "detect-msi").trimmed().
                toLower();
        if (!a->msiGUID.isEmpty()) {
            *err = WPMUtils::validateGUID(a->msiGUID);
            if (!err->isEmpty())
                *err = QString("Wrong MSI GUID for %1: %2").
                        arg(a->toString()).arg(a->msiGUID);
        }
    }

    if (err->isEmpty())
        return a;
    else {
        delete a;
        return 0;
    }
}

bool PackageVersion::contains(const QList<PackageVersion *> &list,
        PackageVersion *pv)
{
    return indexOf(list, pv) >= 0;
}

void PackageVersion::toXML(QDomElement* version) const {
    QDomDocument doc = version->ownerDocument();
    version->setAttribute("name", this->version.getVersionString());
    version->setAttribute("package", this->package);
    if (this->type == 1)
        version->setAttribute("type", "one-file");
    for (int i = 0; i < this->importantFiles.count(); i++) {
        QDomElement importantFile = doc.createElement("important-file");
        version->appendChild(importantFile);
        importantFile.setAttribute("path", this->importantFiles.at(i));
        importantFile.setAttribute("title", this->importantFilesTitles.at(i));
    }
    for (int i = 0; i < this->files.count(); i++) {
        QDomElement file = doc.createElement("file");
        version->appendChild(file);
        file.setAttribute("path", this->files.at(i)->path);
        file.appendChild(doc.createTextNode(files.at(i)->content));
    }
    if (this->download.isValid()) {
        XMLUtils::addTextTag(*version, "url", this->download.toEncoded());
    }
    if (!this->sha1.isEmpty()) {
        XMLUtils::addTextTag(*version, "sha1", this->sha1);
    }
    for (int i = 0; i < this->dependencies.count(); i++) {
        Dependency* d = this->dependencies.at(i);
        QDomElement dependency = doc.createElement("dependency");
        version->appendChild(dependency);
        dependency.setAttribute("package", d->package);
        dependency.setAttribute("versions", d->versionsToString());
        if (!d->var.isEmpty())
            XMLUtils::addTextTag(dependency, "variable", d->var);
    }
    if (!this->msiGUID.isEmpty()) {
        XMLUtils::addTextTag(*version, "detect-msi", this->msiGUID);
    }
    for (int i = 0; i < detectFiles.count(); i++) {
        DetectFile* df = this->detectFiles.at(i);
        QDomElement detectFile = doc.createElement("detect-file");
        version->appendChild(detectFile);
        XMLUtils::addTextTag(detectFile, "path", df->path);
        XMLUtils::addTextTag(detectFile, "sha1", df->sha1);
    }
}

QString PackageVersion::serialize() const
{
    QDomDocument doc;
    QDomElement version = doc.createElement("version");
    doc.appendChild(version);
    toXML(&version);

    return doc.toString(4);
}

PackageVersionFile* PackageVersion::findFile(const QString& path) const
{
    PackageVersionFile* r = 0;
    QString lowerPath = path.toLower();
    for (int i = 0; i < this->files.count(); i++) {
        PackageVersionFile* pvf = this->files.at(i);
        if (pvf->path.toLower() == lowerPath) {
            r = pvf;
            break;
        }
    }
    return r;
}
