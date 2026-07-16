#pragma once

#include <QByteArray>
#include <QList>
#include <QSettings>
#include <QString>

#include <functional>
#include <optional>

namespace ktm::xc2 {

struct Xc2InstallLayout {
    QString root;
    QString javaExe;
    QString backendJar;
    QString configDir;
    QString logConfig;
    QString runtimeConfig;
    QString dpduRootXml;
    QString providerDll;
};

struct Xc2PrerequisiteIssue {
    QString code;
    QString path;
    QString message;
    bool blocking = true;
};

struct Xc2PrerequisiteReport {
    Xc2InstallLayout layout;
    QList<Xc2PrerequisiteIssue> issues;

    bool ok() const;
};

struct Xc2ValidationPolicy {
    QByteArray backendSha256Hex;
    QByteArray providerSha256Hex;
};

struct Xc2RegistryRequest {
    QSettings::Format format = QSettings::Registry32Format;
    QString key = QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\D-PDU API");
    QString valueName = QStringLiteral("Root File");
};

using Xc2RegistryReader = std::function<std::optional<QString>(
    const Xc2RegistryRequest &request)>;

class Xc2InstallationProbe final {
public:
    static Xc2PrerequisiteReport inspectProduction(const QString &installRoot);
    static Xc2PrerequisiteReport inspectForTest(
        const QString &installRoot,
        const Xc2ValidationPolicy &policy,
        Xc2RegistryReader registryReader);
};

} // namespace ktm::xc2
