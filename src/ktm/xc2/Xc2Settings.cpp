#include "ktm/xc2/Xc2Settings.h"

#include "appconstants.h"

#include <QDir>
#include <QSettings>

namespace ktm::xc2 {
namespace {

constexpr auto kInstallRootKey = "ktm/xc2InstallRoot";

QString cleanStoredPath(const QString &path)
{
    if (path.trimmed().isEmpty())
        return {};
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

} // namespace

QString Xc2Settings::installRoot()
{
    return cleanStoredPath(
        rx14::appSettings().value(QString::fromLatin1(kInstallRootKey)).toString());
}

void Xc2Settings::setInstallRoot(const QString &root)
{
    QSettings settings = rx14::appSettings();
    const QString cleaned = cleanStoredPath(root);
    if (cleaned.isEmpty()) {
        settings.remove(QString::fromLatin1(kInstallRootKey));
        return;
    }
    settings.setValue(QString::fromLatin1(kInstallRootKey), cleaned);
}

void Xc2Settings::clearInstallRoot()
{
    rx14::appSettings().remove(QString::fromLatin1(kInstallRootKey));
}

} // namespace ktm::xc2
