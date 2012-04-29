#include "updatesearcher.h"

#include <QTemporaryFile>

#include "..\wpmcpp\downloader.h"
#include "..\wpmcpp\wpmutils.h"
#include "..\wpmcpp\repository.h"

UpdateSearcher::UpdateSearcher()
{
}

void UpdateSearcher::setDownload(Job* job, PackageVersion* pv)
{
    job->setHint("Downloading the package binary");

    Job* sub = job->newSubJob(0.9);
    QString sha1;

    QTemporaryFile* tf = Downloader::download(sub, QUrl(pv->download), &sha1);
    if (!sub->getErrorMessage().isEmpty())
        job->setErrorMessage(QString("Error downloading %1: %2").
                arg(pv->download.toString()).arg(sub->getErrorMessage()));
    else {
        pv->sha1 = sha1;
        job->setProgress(1);
    }
    delete tf;

    delete sub;

    job->complete();
}

QString UpdateSearcher::findTextInPage(Job* job, const QString& url,
        const QString& regex)
{
    QString ret;

    job->setHint(QString("Downloading %1").arg(url));

    QTemporaryFile* tf = 0;
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        Job* sub = job->newSubJob(0.5);
        tf = Downloader::download(sub, QUrl(url));
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Error downloading the page %1: %2").
                    arg(url).arg(sub->getErrorMessage()));
        delete sub;
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (!tf->open())
            job->setErrorMessage(
                    QString("Error opening the file downloaded from %1").
                    arg(url));
        else
            job->setProgress(0.51);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Searching in the text");

        QRegExp re(regex);
        QTextStream in(tf);

        while(!in.atEnd()) {
            QString line = in.readLine();
            int pos = re.indexIn(line);
            if (pos >= 0) {
                ret = re.cap(1);
                break;
            }
        }

        if (ret.isEmpty())
            job->setErrorMessage(QString("The text %1 was not found").
                    arg(regex));
    }

    delete tf;

    if (job->getErrorMessage().isEmpty())
        WPMUtils::outputTextConsole(QString("Found text %1 for %2\n").arg(ret).
                arg(regex));

    job->complete();

    return ret;
}

PackageVersion* UpdateSearcher::findUpdate(Job* job, const QString& package,
        const QString& versionPage,
        const QString& versionRE, QString* realVersion) {
    QString version;
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Searching for the version number");

        Job* sub = job->newSubJob(0.9);
        version = findTextInPage(sub, versionPage, versionRE);
        if (realVersion)
            *realVersion = version;
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Error searching for version number: %1").
                    arg(sub->getErrorMessage()));
        delete sub;
    }

    PackageVersion* ret = 0;
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Searching for the newest package");
        Version v;
        if (v.setVersion(version)) {
            v.normalize();
            Repository* rep = Repository::getDefault();
            PackageVersion* ni = rep->findNewestInstallablePackageVersion(package);
            if (!ni || ni->version.compare(v) < 0) {
                ret = new PackageVersion(package);
                ret->version = v;
            }
            job->setProgress(1);
        } else {
            job->setErrorMessage(QString("Invalid package version found: %1").
                    arg(version));
        }
    }

    job->complete();

    return ret;
}

PackageVersion* UpdateSearcher::findGraphicsMagickUpdates(Job* job) {
    job->setHint("Preparing");

    PackageVersion* ret = 0;

    const QString installScript =
            "for /f \"delims=\" %%x in ('dir /b *.exe') do set setup=%%x\n"
            "\"%setup%\" /SP- /VERYSILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /DIR=\"%CD%\"\n"
            "if %errorlevel% neq 0 exit /b %errorlevel%\n"
            "\n"
            "if \"%npackd_cl%\" equ \"\" set npackd_cl=..\\com.googlecode.windows-package-manager.NpackdCL-1\n"
            "set onecmd=\"%npackd_cl%\\npackdcl.exe\" \"path\" \"--package=com.googlecode.windows-package-manager.CLU\" \"--versions=[1, 2)\"\n"
            "for /f \"usebackq delims=\" %%x in (`%%onecmd%%`) do set clu=%%x\n"
            "\"%clu%\\clu\" add-path --path \"%CD%\"\n"
            "verify\n";
    const QString uninstallScript =
            "unins000.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART\n"
            "if %errorlevel% neq 0 exit /b %errorlevel%\n"
            "\n"
            "if \"%npackd_cl%\" equ \"\" set npackd_cl=..\\com.googlecode.windows-package-manager.NpackdCL-1\n"
            "set onecmd=\"%npackd_cl%\\npackdcl.exe\" \"path\" \"--package=com.googlecode.windows-package-manager.CLU\" \"--versions=[1, 2)\"\n"
            "for /f \"usebackq delims=\" %%x in (`%%onecmd%%`) do set clu=%%x\n"
            "\"%clu%\\clu\" remove-path --path \"%CD%\"\n"
            "verify\n";

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Searching for updates");

        Job* sub = job->newSubJob(0.2);
        ret = findUpdate(sub, "org.graphicsmagick.GraphicsMagickQ16",
                "http://sourceforge.net/projects/graphicsmagick/",
                "GraphicsMagick\\-([\\d\\.]+)\\.tar\\.gz");
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Error searching for the newest version: %1").
                    arg(sub->getErrorMessage()));
        delete sub;
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Examining the binary");

            Job* sub = job->newSubJob(0.75);
            ret->download = QUrl(QString(
                    "http://downloads.sourceforge.net/project/graphicsmagick/graphicsmagick-binaries/") +
                    ret->version.getVersionString() +
                    "/GraphicsMagick-" +
                    ret->version.getVersionString() +
                    "-Q16-windows-dll.exe");
            setDownload(sub, ret);
            if (!sub->getErrorMessage().isEmpty())
                job->setErrorMessage(QString("Error downloading the package binary: %1").
                        arg(sub->getErrorMessage()));
            delete sub;
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Adding dependencies and scripts");

            PackageVersionFile* pvf = new PackageVersionFile(".WPM\\Install.bat",
                    installScript);
            ret->files.append(pvf);
            pvf = new PackageVersionFile(".WPM\\Uninstall.bat",
                    uninstallScript);
            ret->files.append(pvf);
            Dependency* d = new Dependency();
            d->package = "com.googlecode.windows-package-manager.NpackdCL";
            d->minIncluded = true;
            d->min.setVersion(1, 15, 7);
            d->min.normalize();
            d->maxIncluded = false;
            d->max.setVersion(2, 0);
            d->max.normalize();
            ret->dependencies.append(d);

            d = new Dependency();
            d->package = "com.googlecode.windows-package-manager.NpackdCL";
            d->minIncluded = true;
            d->min.setVersion(1, 0);
            d->min.normalize();
            d->maxIncluded = true;
            d->max.setVersion(1, 0);
            d->max.normalize();
            ret->dependencies.append(d);

            d = new Dependency();
            d->package = "com.googlecode.windows-package-manager.CLU";
            d->minIncluded = true;
            d->min.setVersion(1, 0);
            d->min.normalize();
            d->maxIncluded = false;
            d->max.setVersion(2, 0);
            d->max.normalize();
            ret->dependencies.append(d);
        }

        job->setProgress(1);
    }

    job->complete();

    return ret;
}

PackageVersion* UpdateSearcher::findGTKPlusBundleUpdates(Job* job) {
    job->setHint("Preparing");

    PackageVersion* ret = 0;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Searching for updates");

        Job* sub = job->newSubJob(0.2);
        ret = findUpdate(sub, "org.gtk.GTKPlusBundle",
                "http://www.gtk.org/download/win32.php",
                "http://ftp\\.gnome\\.org/pub/gnome/binaries/win32/gtk\\+/[\\d\\.]+/gtk\\+\\-bundle_([\\d\\.]+).+win32\\.zip");
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Error searching for the newest version: %1").
                    arg(sub->getErrorMessage()));
        delete sub;
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Searching for the download URL");

            Job* sub = job->newSubJob(0.2);
            QString url = findTextInPage(sub,
                    "http://www.gtk.org/download/win32.php",
                    "(http://ftp\\.gnome\\.org/pub/gnome/binaries/win32/gtk\\+/([\\d\\.]+)/gtk\\+\\-bundle_.+win32\\.zip)");
            if (!sub->getErrorMessage().isEmpty())
                job->setErrorMessage(QString("Error searching for version number: %1").
                        arg(sub->getErrorMessage()));
            delete sub;

            if (job->getErrorMessage().isEmpty()) {
                ret->download = QUrl(url);
                if (!ret->download.isValid())
                    job->setErrorMessage(QString(
                            "Download URL is not valid: %1").arg(url));
            }
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Examining the binary");

            Job* sub = job->newSubJob(0.55);
            setDownload(sub, ret);
            if (!sub->getErrorMessage().isEmpty())
                job->setErrorMessage(QString("Error downloading the package binary: %1").
                        arg(sub->getErrorMessage()));
            delete sub;
        }
    }

    if (ret) {
        const QString installScript =
                "if \"%npackd_cl%\" equ \"\" set npackd_cl=..\\com.googlecode.windows-package-manager.NpackdCL-1\n"
                "set onecmd=\"%npackd_cl%\\npackdcl.exe\" \"path\" \"--package=com.googlecode.windows-package-manager.CLU\" \"--versions=[1, 2)\"\n"
                "for /f \"usebackq delims=\" %%x in (`%%onecmd%%`) do set clu=%%x\n"
                "\"%clu%\\clu\" add-path --path \"%CD%\\bin\"\n"
                "verify\n";
        const QString uninstallScript =
                "if \"%npackd_cl%\" equ \"\" set npackd_cl=..\\com.googlecode.windows-package-manager.NpackdCL-1\n"
                "set onecmd=\"%npackd_cl%\\npackdcl.exe\" \"path\" \"--package=com.googlecode.windows-package-manager.CLU\" \"--versions=[1, 2)\"\n"
                "for /f \"usebackq delims=\" %%x in (`%%onecmd%%`) do set clu=%%x\n"
                "\"%clu%\\clu\" remove-path --path \"%CD%\\bin\"\n"
                "verify\n";

        PackageVersionFile* pvf = new PackageVersionFile(".WPM\\Install.bat",
                installScript);
        ret->files.append(pvf);
        pvf = new PackageVersionFile(".WPM\\Uninstall.bat",
                uninstallScript);
        ret->files.append(pvf);
        pvf = new PackageVersionFile("etc\\gtk-2.0\\gtkrc",
                "gtk-theme-name = \"MS-Windows\"");
        ret->files.append(pvf);


        Dependency* d = new Dependency();
        d->package = "com.googlecode.windows-package-manager.NpackdCL";
        d->minIncluded = true;
        d->min.setVersion(1, 15, 7);
        d->min.normalize();
        d->maxIncluded = false;
        d->max.setVersion(2, 0);
        d->max.normalize();
        ret->dependencies.append(d);

        d = new Dependency();
        d->package = "com.googlecode.windows-package-manager.NpackdCL";
        d->minIncluded = true;
        d->min.setVersion(1, 0);
        d->min.normalize();
        d->maxIncluded = true;
        d->max.setVersion(1, 0);
        d->max.normalize();
        ret->dependencies.append(d);

        d = new Dependency();
        d->package = "com.googlecode.windows-package-manager.CLU";
        d->minIncluded = true;
        d->min.setVersion(1, 0);
        d->min.normalize();
        d->maxIncluded = false;
        d->max.setVersion(2, 0);
        d->max.normalize();
        ret->dependencies.append(d);
    }

    job->complete();

    return ret;
}

PackageVersion* UpdateSearcher::findH2Updates(Job* job) {
    job->setHint("Preparing");

    PackageVersion* ret = 0;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Searching for updates");

        Job* sub = job->newSubJob(0.2);
        ret = findUpdate(sub, "com.h2database.H2",
                "http://www.h2database.com/html/download.html",
                "Version ([\\d\\.]+) \\([\\d\\-]+\\), Last Stable");
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Error searching for the newest version: %1").
                    arg(sub->getErrorMessage()));
        delete sub;
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Searching for the download URL");

            Job* sub = job->newSubJob(0.2);
            QString url = findTextInPage(sub,
                    "http://www.h2database.com/html/download.html",
                    "Version [\\d\\.]+ \\(([\\d\\-]+)\\), Last Stable");
            if (!sub->getErrorMessage().isEmpty())
                job->setErrorMessage(QString("Error searching for version number: %1").
                        arg(sub->getErrorMessage()));
            delete sub;

            if (job->getErrorMessage().isEmpty()) {
                ret->download = QUrl("http://www.h2database.com/h2-setup-" +
                        url + ".exe");
                if (!ret->download.isValid())
                    job->setErrorMessage(QString(
                            "Download URL is not valid: %1").arg(url));
            }
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Examining the binary");

            Job* sub = job->newSubJob(0.55);
            ret->type = 1;
            setDownload(sub, ret);
            if (!sub->getErrorMessage().isEmpty())
                job->setErrorMessage(QString("Error downloading the package binary: %1").
                        arg(sub->getErrorMessage()));
            delete sub;
        }
    }

    if (ret) {
        const QString installScript =
                "for /f \"delims=\" %%x in ('dir /b *.exe') do set setup=%%x\n"
                "\"%setup%\" /S /D=%CD%\\Program\n";
        const QString uninstallScript =
                "Program\\Uninstall.exe /S _?=%CD%\\Program\n";

        PackageVersionFile* pvf = new PackageVersionFile(".WPM\\Install.bat",
                installScript);
        ret->files.append(pvf);
        pvf = new PackageVersionFile(".WPM\\Uninstall.bat",
                uninstallScript);
        ret->files.append(pvf);

        Dependency* d = new Dependency();
        d->package = "com.oracle.JRE";
        d->minIncluded = true;
        d->min.setVersion(1, 5);
        d->min.normalize();
        d->maxIncluded = false;
        d->max.setVersion(2, 0);
        d->max.normalize();
        ret->dependencies.append(d);
    }

    job->complete();

    return ret;
}

PackageVersion* UpdateSearcher::findHandBrakeUpdates(Job* job)
{
    job->setHint("Preparing");

    PackageVersion* ret = 0;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Searching for updates");

        Job* sub = job->newSubJob(0.2);
        ret = findUpdate(sub, "fr.handbrake.HandBrake",
                "http://handbrake.fr/downloads.php",
                "The current release version is <b>([\\d\\.]+)</b>");
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Error searching for the newest version: %1").
                    arg(sub->getErrorMessage()));
        delete sub;
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Searching for the download URL");

            ret->download = QUrl("http://handbrake.fr/rotation.php?file=HandBrake-" +
                    ret->version.getVersionString() + "-i686-Win_GUI.exe");
        }
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Examining the binary");

            Job* sub = job->newSubJob(0.55);
            ret->type = 1;
            setDownload(sub, ret);
            if (!sub->getErrorMessage().isEmpty())
                job->setErrorMessage(QString("Error downloading the package binary: %1").
                        arg(sub->getErrorMessage()));
            delete sub;
        }
    }

    if (ret) {
        const QString installScript =
                "ren rotation.php setup.exe\n"
                "setup.exe /S /D=%CD%\n";
        const QString uninstallScript =
                "uninst.exe /S\n";

        PackageVersionFile* pvf = new PackageVersionFile(".WPM\\Install.bat",
                installScript);
        ret->files.append(pvf);
        pvf = new PackageVersionFile(".WPM\\Uninstall.bat",
                uninstallScript);
        ret->files.append(pvf);

        Dependency* d = new Dependency();
        d->package = "com.microsoft.DotNetRedistributable";
        d->minIncluded = true;
        d->min.setVersion(2, 0);
        d->min.normalize();
        d->maxIncluded = false;
        d->max.setVersion(3, 0);
        d->max.normalize();
        ret->dependencies.append(d);
    }

    job->complete();

    return ret;
}

PackageVersion* UpdateSearcher::findImgBurnUpdates(Job* job)
{
    job->setHint("Preparing");

    PackageVersion* ret = 0;

    QString version;
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Searching for updates");

        Job* sub = job->newSubJob(0.2);
        ret = findUpdate(sub, "com.imgburn.ImgBurn",
                "http://www.imgburn.com/",
                "Current version: ([\\d\\.]+)", &version);
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Error searching for the newest version: %1").
                    arg(sub->getErrorMessage()));
        delete sub;
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Searching for the download URL");

            ret->download = QUrl("http://download.imgburn.com/SetupImgBurn_" +
                    version + ".exe");
        }
        job->setProgress(0.3);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Examining the binary");

            Job* sub = job->newSubJob(0.65);
            ret->type = 1;
            setDownload(sub, ret);
            if (!sub->getErrorMessage().isEmpty())
                job->setErrorMessage(QString("Error downloading the package binary: %1").
                        arg(sub->getErrorMessage()));
            delete sub;
        }
    }

    if (job->shouldProceed("Setting scripts")) {
        if (ret) {
            const QString installScript =
                    "for /f %%x in ('dir /b *.exe') do set setup=%%x\n"
                    "\"%setup%\" /S /D=%CD%\n";
            const QString uninstallScript =
                    "uninstall.exe /S\n";

            PackageVersionFile* pvf = new PackageVersionFile(".WPM\\Install.bat",
                    installScript);
            ret->files.append(pvf);
            pvf = new PackageVersionFile(".WPM\\Uninstall.bat",
                    uninstallScript);
            ret->files.append(pvf);
        }
        job->setProgress(1);
    }

    job->complete();

    return ret;
}

PackageVersion* UpdateSearcher::findIrfanViewUpdates(Job* job)
{
    job->setHint("Preparing");

    PackageVersion* ret = 0;

    QString version;
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Searching for updates");

        Job* sub = job->newSubJob(0.2);
        ret = findUpdate(sub, "de.irfanview.IrfanView",
                "www.irfanview.net/main_start_engl.htm",
                "Current version: ([\\d\\.]+)", &version);
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Error searching for the newest version: %1").
                    arg(sub->getErrorMessage()));
        delete sub;
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Searching for the download URL");

            ret->download = QUrl("http://irfanview.tuwien.ac.at/iview" +
                    version.remove('.') + ".zip");
        }
        job->setProgress(0.3);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        if (ret) {
            job->setHint("Examining the binary");

            Job* sub = job->newSubJob(0.65);
            ret->type = 1;
            setDownload(sub, ret);
            if (!sub->getErrorMessage().isEmpty())
                job->setErrorMessage(QString("Error downloading the package binary: %1").
                        arg(sub->getErrorMessage()));
            delete sub;
        }
    }

    if (job->shouldProceed("Setting scripts")) {
        if (ret) {
            const QString installScript =
                    "for /f %%x in ('dir /b *.exe') do set setup=%%x\n"
                    "\"%setup%\" /S /D=%CD%\n";
            const QString uninstallScript =
                    "uninstall.exe /S\n";

            PackageVersionFile* pvf = new PackageVersionFile(".WPM\\Install.bat",
                    installScript);
            ret->files.append(pvf);
            pvf = new PackageVersionFile(".WPM\\Uninstall.bat",
                    uninstallScript);
            ret->files.append(pvf);
        }
        job->setProgress(1);
    }

    job->complete();

    return ret;
}

void UpdateSearcher::findUpdates(Job* job)
{
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Downloading repositories");

        Repository* rep = Repository::getDefault();
        Job* sub = job->newSubJob(0.4);
        rep->reload(sub);
        if (!sub->getErrorMessage().isEmpty()) {
            job->setErrorMessage(sub->getErrorMessage());
        }
        delete sub;
    }

    QStringList packages;
    packages.append("GraphicsMagick");
    packages.append("GTKPlusBundle");
    packages.append("H2");
    packages.append("HandBrake");
    packages.append("ImgBurn");
    packages.append("IrfanView");

    for (int i = 0; i < packages.count(); i++) {
        const QString package = packages.at(i);
        if (job->isCancelled())
            break;

        job->setHint(QString("Searching for updates for %1").arg(package));

        Job* sub = job->newSubJob(0.6 / packages.count());
        PackageVersion* pv = 0;
        switch (i) {
            case 0:
                pv = findGraphicsMagickUpdates(sub);
                break;
            case 1:
                pv = findGTKPlusBundleUpdates(sub);
                break;
            case 2:
                pv = findH2Updates(sub);
                break;
            case 3:
                pv = findHandBrakeUpdates(sub);
                break;
            case 4:
                pv = findImgBurnUpdates(sub);
                break;
            case 5:
                pv = findIrfanViewUpdates(sub);
                break;
        }
        if (!sub->getErrorMessage().isEmpty()) {
            job->setErrorMessage(sub->getErrorMessage());
        } else {
            job->setProgress(0.4 + 0.6 * (i + 1) / packages.count());
            if (!pv)
                WPMUtils::outputTextConsole(QString(
                        "No updates found for %1\n").arg(package));
            else {
                WPMUtils::outputTextConsole(
                        QString("New %1 version: %2\n").arg(package).
                        arg(pv->version.getVersionString()));
                WPMUtils::outputTextConsole(pv->serialize());
                delete pv;
            }
        }
        delete sub;

        if (!job->getErrorMessage().isEmpty())
            break;
    }

    job->complete();
}