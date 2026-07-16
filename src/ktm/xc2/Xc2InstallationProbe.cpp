#include "ktm/xc2/Xc2InstallationProbe.h"

#include "ktm/xc2/Xc2ContractProfile.h"

#include <QtEndian>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>

namespace ktm::xc2 {
namespace {

constexpr quint16 kPeMachineI386 = 0x014c;

void addIssue(Xc2PrerequisiteReport &report,
              const QString &code,
              const QString &path,
              const QString &message)
{
    report.issues.append({code, path, message, true});
}

bool isValidSha256Hex(const QByteArray &hash)
{
    if (hash.size() != 64)
        return false;

    return std::all_of(hash.cbegin(), hash.cend(), [](char byte) {
        return (byte >= '0' && byte <= '9')
            || (byte >= 'a' && byte <= 'f')
            || (byte >= 'A' && byte <= 'F');
    });
}

bool hasApplicationRootSuffix(const QString &path)
{
    const QStringList components = QDir::fromNativeSeparators(path).split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    return components.size() >= 2
        && components.at(components.size() - 2).compare(
               QStringLiteral("resources"), Qt::CaseInsensitive) == 0
        && components.constLast().compare(
               QStringLiteral("app"), Qt::CaseInsensitive) == 0;
}

bool requireRegularFile(Xc2PrerequisiteReport &report,
                        const QString &path,
                        const QString &missingCode,
                        const QString &description)
{
    const QFileInfo info(path);
    if (info.exists() && info.isFile()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            return true;
        addIssue(report, QStringLiteral("file_unreadable"), path,
                 QStringLiteral("The required file cannot be opened for reading."));
        return false;
    }

    addIssue(report, missingCode, path,
             QStringLiteral("Missing %1.").arg(description));
    return false;
}

std::optional<QByteArray> sha256File(Xc2PrerequisiteReport &report,
                                    const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        addIssue(report, QStringLiteral("file_unreadable"), path,
                 QStringLiteral("The file cannot be opened for reading."));
        return std::nullopt;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk;
    while (!file.atEnd()) {
        chunk = file.read(64 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            addIssue(report, QStringLiteral("file_unreadable"), path,
                     QStringLiteral("The file cannot be read completely."));
            return std::nullopt;
        }
        hash.addData(chunk);
    }
    return hash.result().toHex().toUpper();
}

bool validateI386Pe(Xc2PrerequisiteReport &report,
                    const QString &path,
                    const QString &wrongArchitectureCode)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        addIssue(report, QStringLiteral("file_unreadable"), path,
                 QStringLiteral("The PE file cannot be opened for reading."));
        return false;
    }

    const auto invalidPe = [&report, &path]() {
        addIssue(report, QStringLiteral("invalid_pe"), path,
                 QStringLiteral("The file is not a bounded PE image."));
        return false;
    };

    if (file.size() < 0x40)
        return invalidPe();

    const QByteArray dosMagic = file.read(2);
    if (dosMagic.size() != 2 || dosMagic != QByteArrayLiteral("MZ"))
        return invalidPe();

    if (!file.seek(0x3c))
        return invalidPe();
    const QByteArray offsetBytes = file.read(4);
    if (offsetBytes.size() != 4)
        return invalidPe();

    const quint32 peOffset = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(offsetBytes.constData()));
    const quint64 fileSize = static_cast<quint64>(file.size());
    if (peOffset < 0x40 || static_cast<quint64>(peOffset) > fileSize - 6)
        return invalidPe();

    if (!file.seek(peOffset))
        return invalidPe();
    const QByteArray peHeader = file.read(6);
    if (peHeader.size() != 6
        || peHeader.left(4) != QByteArray("PE\0\0", 4)) {
        return invalidPe();
    }

    const quint16 machine = qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(peHeader.constData() + 4));
    if (machine != kPeMachineI386) {
        addIssue(report, wrongArchitectureCode, path,
                 QStringLiteral("The PE machine is not x86 (0x014c)."));
        return false;
    }
    return true;
}

struct ProviderXmlLookup {
    bool validXml = false;
    bool providerFound = false;
    QString uri;
};

ProviderXmlLookup lookupProviderUri(QIODevice *device,
                                    const QString &supportedShortName)
{
    QXmlStreamReader xml(device);
    ProviderXmlLookup result;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()
            || xml.name() != QLatin1String("MVCI_PDU_API")) {
            continue;
        }

        QString shortName;
        QString libraryUri;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isEndElement()
                && xml.name() == QLatin1String("MVCI_PDU_API")) {
                break;
            }
            if (!xml.isStartElement())
                continue;

            if (xml.name() == QLatin1String("SHORT_NAME")) {
                shortName = xml.readElementText(
                    QXmlStreamReader::SkipChildElements);
            } else if (xml.name() == QLatin1String("LIBRARY_FILE")) {
                libraryUri = xml.attributes()
                                 .value(QLatin1String("URI"))
                                 .toString();
                xml.skipCurrentElement();
            }
        }

        if (!result.providerFound && shortName == supportedShortName) {
            result.providerFound = true;
            result.uri = libraryUri;
        }
    }

    result.validXml = !xml.hasError();
    return result;
}

std::optional<QString> localPathFromLegacyFileUri(const QString &uri)
{
    static const QRegularExpression localDrivePattern(
        QStringLiteral("^file:/([A-Za-z]:[\\\\/].*)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = localDrivePattern.match(uri);
    if (!match.hasMatch())
        return std::nullopt;

    const QString encodedPath = match.captured(1);
    QString decodedPath;
    decodedPath.reserve(encodedPath.size());
    for (qsizetype index = 0; index < encodedPath.size(); ++index) {
        const QChar character = encodedPath.at(index);
        if (character == QLatin1Char('%')) {
            if (index + 2 >= encodedPath.size()
                || encodedPath.mid(index + 1, 2).compare(
                       QStringLiteral("20"), Qt::CaseInsensitive) != 0) {
                return std::nullopt;
            }
            decodedPath.append(QLatin1Char(' '));
            index += 2;
            continue;
        }
        if (character.unicode() < 0x20
            || character == QLatin1Char('?')
            || character == QLatin1Char('#')) {
            return std::nullopt;
        }
        decodedPath.append(character);
    }

    const QString normalized = QDir::cleanPath(
        QDir::fromNativeSeparators(decodedPath));
    static const QRegularExpression normalizedDrivePattern(
        QStringLiteral("^[A-Za-z]:/"));
    if (!normalizedDrivePattern.match(normalized).hasMatch()
        || !QDir::isAbsolutePath(normalized)
        || normalized.startsWith(QStringLiteral("//"))) {
        return std::nullopt;
    }
    return normalized;
}

std::optional<QString> readRegistryRootFile(
    const Xc2RegistryRequest &request)
{
    QSettings registry(request.key, request.format);
    const QVariant value = registry.value(request.valueName);
    if (!value.isValid())
        return std::nullopt;

    const QString path = value.toString();
    if (path.trimmed().isEmpty())
        return std::nullopt;
    return path;
}

Xc2PrerequisiteReport inspectInstallation(
    const QString &installRoot,
    const Xc2ValidationPolicy &policy,
    const QString &supportedShortName,
    Xc2RegistryReader registryReader)
{
    Xc2PrerequisiteReport report;

    const bool backendPolicyValid = isValidSha256Hex(policy.backendSha256Hex);
    const bool providerPolicyValid = isValidSha256Hex(policy.providerSha256Hex);
    if (!backendPolicyValid) {
        addIssue(report, QStringLiteral("invalid_policy"),
                 QStringLiteral("backendSha256Hex"),
                 QStringLiteral("The expected backend SHA-256 is invalid."));
    }
    if (!providerPolicyValid) {
        addIssue(report, QStringLiteral("invalid_policy"),
                 QStringLiteral("providerSha256Hex"),
                 QStringLiteral("The expected provider SHA-256 is invalid."));
    }

    if (installRoot.trimmed().isEmpty()) {
        addIssue(report, QStringLiteral("missing_root"), {},
                 QStringLiteral("The XC2 application root is not configured."));
        return report;
    }

    const QString cleanedRoot = QDir::cleanPath(
        QDir::fromNativeSeparators(installRoot));
    report.layout.root = cleanedRoot;
    if (!QDir::isAbsolutePath(cleanedRoot)) {
        addIssue(report, QStringLiteral("invalid_install_root"), cleanedRoot,
                 QStringLiteral("The XC2 application root must be absolute."));
        return report;
    }

    const QFileInfo rootInfo(cleanedRoot);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        addIssue(report, QStringLiteral("root_not_directory"), cleanedRoot,
                 QStringLiteral("The XC2 application root is not a directory."));
        return report;
    }
    if (!hasApplicationRootSuffix(cleanedRoot)) {
        addIssue(report, QStringLiteral("invalid_install_root"), cleanedRoot,
                 QStringLiteral("The XC2 application root must end in resources/app."));
        return report;
    }

    const QDir rootDir(cleanedRoot);
    report.layout.javaExe = QDir::cleanPath(
        rootDir.filePath(QStringLiteral("jre/bin/java.exe")));
    report.layout.backendJar = QDir::cleanPath(
        rootDir.filePath(QStringLiteral("xc2_backend_patched.jar")));
    report.layout.configDir = QDir::cleanPath(
        rootDir.filePath(QStringLiteral("config")));
    report.layout.logConfig = QDir::cleanPath(
        rootDir.filePath(QStringLiteral("config/log.xml")));
    report.layout.runtimeConfig = QDir::cleanPath(
        rootDir.filePath(QStringLiteral("config/custom-cloud.properties")));

    const bool hasJava = requireRegularFile(
        report, report.layout.javaExe, QStringLiteral("missing_java"),
        QStringLiteral("bundled jre/bin/java.exe"));
    const bool hasBackend = requireRegularFile(
        report, report.layout.backendJar, QStringLiteral("missing_backend"),
        QStringLiteral("xc2_backend_patched.jar"));
    requireRegularFile(report, report.layout.logConfig,
                       QStringLiteral("missing_log_config"),
                       QStringLiteral("config/log.xml"));
    requireRegularFile(report, report.layout.runtimeConfig,
                       QStringLiteral("missing_runtime_config"),
                       QStringLiteral("config/custom-cloud.properties"));

    if (hasJava) {
        validateI386Pe(report, report.layout.javaExe,
                       QStringLiteral("wrong_java_arch"));
    }

    if (hasBackend) {
        const std::optional<QByteArray> actualHash =
            sha256File(report, report.layout.backendJar);
        if (actualHash.has_value() && backendPolicyValid
            && actualHash.value() != policy.backendSha256Hex.toUpper()) {
            addIssue(report, QStringLiteral("backend_hash_mismatch"),
                     report.layout.backendJar,
                     QStringLiteral("The backend SHA-256 is not approved."));
        }
    }

    const Xc2RegistryRequest request;
    const std::optional<QString> registryRoot = registryReader
        ? registryReader(request)
        : std::nullopt;
    if (!registryRoot.has_value() || registryRoot->trimmed().isEmpty()) {
        addIssue(report, QStringLiteral("missing_dpdu_registry"),
                 request.key + QLatin1Char('\\') + request.valueName,
                 QStringLiteral("The 32-bit D-PDU root file is not registered."));
        return report;
    }

    const QString rootXml = QDir::cleanPath(
        QDir::fromNativeSeparators(registryRoot.value()));
    report.layout.dpduRootXml = rootXml;
    if (!QDir::isAbsolutePath(rootXml)) {
        addIssue(report, QStringLiteral("invalid_dpdu_xml"), rootXml,
                 QStringLiteral("The D-PDU root XML path must be absolute."));
        return report;
    }

    QFile xmlFile(rootXml);
    if (!QFileInfo(rootXml).isFile()
        || !xmlFile.open(QIODevice::ReadOnly)) {
        addIssue(report, QStringLiteral("file_unreadable"), rootXml,
                 QStringLiteral("The D-PDU root XML cannot be read."));
        return report;
    }

    const ProviderXmlLookup lookup = lookupProviderUri(
        &xmlFile, supportedShortName);
    if (!lookup.validXml) {
        addIssue(report, QStringLiteral("invalid_dpdu_xml"), rootXml,
                 QStringLiteral("The D-PDU root XML is malformed."));
        return report;
    }
    if (!lookup.providerFound) {
        addIssue(report, QStringLiteral("unsupported_dpdu_provider"), rootXml,
                 QStringLiteral("The approved AVL VCI2K provider is absent."));
        return report;
    }

    const std::optional<QString> providerPath =
        localPathFromLegacyFileUri(lookup.uri);
    if (!providerPath.has_value()) {
        addIssue(report, QStringLiteral("unsafe_provider_uri"), lookup.uri,
                 QStringLiteral("The provider URI is not an absolute local drive path."));
        return report;
    }
    report.layout.providerDll = providerPath.value();

    if (!QFileInfo(report.layout.providerDll).isFile()) {
        addIssue(report, QStringLiteral("file_unreadable"),
                 report.layout.providerDll,
                 QStringLiteral("The D-PDU provider DLL cannot be read."));
        return report;
    }

    const std::optional<QByteArray> actualProviderHash =
        sha256File(report, report.layout.providerDll);
    if (!actualProviderHash.has_value())
        return report;

    if (providerPolicyValid
        && actualProviderHash.value() != policy.providerSha256Hex.toUpper()) {
        addIssue(report, QStringLiteral("provider_hash_mismatch"),
                 report.layout.providerDll,
                 QStringLiteral("The provider SHA-256 is not approved."));
    }
    validateI386Pe(report, report.layout.providerDll,
                   QStringLiteral("wrong_provider_arch"));

    return report;
}

} // namespace

bool Xc2PrerequisiteReport::ok() const
{
    return std::none_of(issues.cbegin(), issues.cend(),
                        [](const Xc2PrerequisiteIssue &issue) {
        return issue.blocking;
    });
}

Xc2PrerequisiteReport Xc2InstallationProbe::inspectProduction(
    const QString &installRoot)
{
    const Xc2ContractProfile &profile = Xc2ContractProfile::approved();
    return inspectInstallation(
        installRoot,
        {profile.backendSha256Hex(), profile.providerSha256Hex()},
        profile.supportedPduApiShortName(), readRegistryRootFile);
}

Xc2PrerequisiteReport Xc2InstallationProbe::inspectForTest(
    const QString &installRoot,
    const Xc2ValidationPolicy &policy,
    Xc2RegistryReader registryReader)
{
    return inspectInstallation(
        installRoot, policy,
        Xc2ContractProfile::approved().supportedPduApiShortName(),
        std::move(registryReader));
}

} // namespace ktm::xc2
