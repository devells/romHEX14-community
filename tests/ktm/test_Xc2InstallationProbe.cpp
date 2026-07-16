#include <QtEndian>
#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <qt_windows.h>

#include <optional>

#include "appconstants.h"
#include "ktm/xc2/Xc2ContractProfile.h"
#include "ktm/xc2/Xc2InstallationProbe.h"
#include "ktm/xc2/Xc2Settings.h"

using namespace ktm::xc2;

namespace {

struct InstallationPaths {
    QString root;
    QString javaExe;
    QString backendJar;
    QString logConfig;
    QString runtimeConfig;
    QString rootXml;
    QString providerDll;
};

class ExclusiveFileHandle final {
public:
    explicit ExclusiveFileHandle(const QString &path)
        : m_handle(CreateFileW(
              reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, 0,
              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }

    ~ExclusiveFileHandle()
    {
        if (isValid())
            CloseHandle(m_handle);
    }

    bool isValid() const { return m_handle != INVALID_HANDLE_VALUE; }

    ExclusiveFileHandle(const ExclusiveFileHandle &) = delete;
    ExclusiveFileHandle &operator=(const ExclusiveFileHandle &) = delete;

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

InstallationPaths installationPaths(const QString &basePath)
{
    const QString root = QDir(basePath).filePath(QStringLiteral("resources/app"));
    return {
        root,
        QDir(root).filePath(QStringLiteral("jre/bin/java.exe")),
        QDir(root).filePath(QStringLiteral("xc2_backend_patched.jar")),
        QDir(root).filePath(QStringLiteral("config/log.xml")),
        QDir(root).filePath(QStringLiteral("config/custom-cloud.properties")),
        QDir(basePath).filePath(QStringLiteral("D-PDU API/Root File.xml")),
        QDir(basePath).filePath(QStringLiteral("Provider Files/PDUAPI_AVLDitest.dll")),
    };
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(contents) == contents.size();
}

QByteArray syntheticPe(quint16 machine)
{
    constexpr quint32 peOffset = 0x80;
    QByteArray bytes(static_cast<qsizetype>(peOffset + 6), '\0');
    bytes[0] = 'M';
    bytes[1] = 'Z';
    qToLittleEndian(peOffset,
                    reinterpret_cast<uchar *>(bytes.data() + 0x3c));
    bytes[peOffset] = 'P';
    bytes[peOffset + 1] = 'E';
    qToLittleEndian(machine,
                    reinterpret_cast<uchar *>(bytes.data() + peOffset + 4));
    return bytes;
}

QByteArray sha256Hex(const QByteArray &contents)
{
    return QCryptographicHash::hash(contents, QCryptographicHash::Sha256)
        .toHex()
        .toUpper();
}

Xc2ValidationPolicy matchingPolicy(const QByteArray &backend,
                                   const QByteArray &provider)
{
    return {sha256Hex(backend), sha256Hex(provider)};
}

Xc2ValidationPolicy arbitraryValidPolicy()
{
    return {QByteArray(64, 'A'), QByteArray(64, 'B')};
}

QString legacyFileUri(const QString &path, bool nativeSeparators = false)
{
    QString localPath = QDir::fromNativeSeparators(path);
    if (nativeSeparators)
        localPath.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return QStringLiteral("file:/") + localPath;
}

QByteArray providerXml(const QString &targetUri,
                       const QString &targetShortName =
                           QStringLiteral("AVL Ditest VCI2K_DPDU_API"))
{
    const QString xml = QStringLiteral(
        "<?xml version=\"1.0\"?>\n"
        "<D-PDU_API>\n"
        "  <MVCI_PDU_API>\n"
        "    <SHORT_NAME>Decoy Provider</SHORT_NAME>\n"
        "    <LIBRARY_FILE URI=\"file:/C:/decoy/PDUAPI.dll\"/>\n"
        "  </MVCI_PDU_API>\n"
        "  <MVCI_PDU_API>\n"
        "    <SHORT_NAME>%1</SHORT_NAME>\n"
        "    <LIBRARY_FILE URI=\"%2\"/>\n"
        "  </MVCI_PDU_API>\n"
        "</D-PDU_API>\n")
        .arg(targetShortName.toHtmlEscaped(), targetUri.toHtmlEscaped());
    return xml.toUtf8();
}

bool populateInstallation(const InstallationPaths &paths,
                          const QByteArray &java,
                          const QByteArray &backend,
                          const QByteArray &provider,
                          const QString &providerUri = {})
{
    const QString uri = providerUri.isEmpty()
        ? legacyFileUri(paths.providerDll)
        : providerUri;
    return writeFile(paths.javaExe, java)
        && writeFile(paths.backendJar, backend)
        && writeFile(paths.logConfig, QByteArray("<log/>"))
        && writeFile(paths.runtimeConfig, QByteArray("profile=local\n"))
        && writeFile(paths.providerDll, provider)
        && writeFile(paths.rootXml, providerXml(uri));
}

bool hasIssue(const Xc2PrerequisiteReport &report, const QString &code)
{
    for (const Xc2PrerequisiteIssue &issue : report.issues) {
        if (issue.code == code)
            return true;
    }
    return false;
}

bool hasIssueForPath(const Xc2PrerequisiteReport &report,
                     const QString &code,
                     const QString &path)
{
    for (const Xc2PrerequisiteIssue &issue : report.issues) {
        if (issue.code == code
            && QDir::cleanPath(issue.path) == QDir::cleanPath(path)) {
            return true;
        }
    }
    return false;
}

int issueCount(const Xc2PrerequisiteReport &report, const QString &code)
{
    int count = 0;
    for (const Xc2PrerequisiteIssue &issue : report.issues) {
        if (issue.code == code)
            ++count;
    }
    return count;
}

Xc2RegistryReader readerFor(const QString &rootXml)
{
    return [rootXml](const Xc2RegistryRequest &) -> std::optional<QString> {
        return rootXml;
    };
}

} // namespace

class Xc2InstallationProbeTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_settingsDir.isValid());
        m_previousSettingsFormat = QSettings::defaultFormat();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDir.path());

        QSettings store = rx14::appSettings();
        const QString settingsFile = QDir::fromNativeSeparators(store.fileName());
        const QString settingsPrefix =
            QDir::fromNativeSeparators(m_settingsDir.path()) + QLatin1Char('/');
        QVERIFY2(settingsFile.startsWith(settingsPrefix),
                 qPrintable(QStringLiteral("settings file %1 is not below %2")
                                .arg(settingsFile, settingsPrefix)));
        store.remove(QStringLiteral("ktm/xc2InstallRoot"));
        store.sync();
    }

    void cleanupTestCase()
    {
        Xc2Settings::clearInstallRoot();
        QSettings store = rx14::appSettings();
        store.sync();
        QSettings::setDefaultFormat(m_previousSettingsFormat);
    }

    void emptyRootReportsMissingRoot()
    {
        bool registryCalled = false;
        const Xc2PrerequisiteReport report = Xc2InstallationProbe::inspectForTest(
            QString(), arbitraryValidPolicy(),
            [&registryCalled](const Xc2RegistryRequest &)
                -> std::optional<QString> {
                registryCalled = true;
                return std::nullopt;
            });

        QVERIFY(hasIssue(report, QStringLiteral("missing_root")));
        QVERIFY(!report.ok());
        QVERIFY(report.layout.javaExe.isEmpty());
        QVERIFY(report.layout.backendJar.isEmpty());
        QVERIFY(!registryCalled);
    }

    void missingAndNonDirectoryRootsNeverResolveAgainstCwd()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());

        const QString missing = QDir(temp.path()).filePath(
            QStringLiteral("missing/resources/app"));
        Xc2PrerequisiteReport report = Xc2InstallationProbe::inspectForTest(
            missing, arbitraryValidPolicy(), {});
        QVERIFY(hasIssue(report, QStringLiteral("root_not_directory")));
        QVERIFY(report.layout.javaExe.isEmpty());
        QVERIFY(report.layout.backendJar.isEmpty());

        const QString nonDirectory = QDir(temp.path()).filePath(
            QStringLiteral("not-a-directory/resources/app"));
        QVERIFY(writeFile(nonDirectory, QByteArray("file")));
        report = Xc2InstallationProbe::inspectForTest(
            nonDirectory, arbitraryValidPolicy(), {});
        QVERIFY(hasIssue(report, QStringLiteral("root_not_directory")));
        QVERIFY(report.layout.javaExe.isEmpty());
        QVERIFY(report.layout.backendJar.isEmpty());
    }

    void installRootMustEndInResourcesApp()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString wrongRoot = QDir(temp.path()).filePath(
            QStringLiteral("XC_2_Prog"));
        QVERIFY(QDir().mkpath(wrongRoot));

        const Xc2PrerequisiteReport report =
            Xc2InstallationProbe::inspectForTest(
                wrongRoot, arbitraryValidPolicy(), {});
        QVERIFY(hasIssue(report, QStringLiteral("invalid_install_root")));
        QVERIFY(!report.ok());
        QVERIFY(report.layout.javaExe.isEmpty());
    }

    void missingJavaAndJarAreSeparateIssues()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        QVERIFY(QDir().mkpath(QDir(paths.root).filePath(QStringLiteral("config"))));

        const Xc2PrerequisiteReport report = Xc2InstallationProbe::inspectForTest(
            paths.root, arbitraryValidPolicy(), {});
        QVERIFY(hasIssue(report, QStringLiteral("missing_java")));
        QVERIFY(hasIssue(report, QStringLiteral("missing_backend")));
        QVERIFY(!report.ok());
    }

    void missingRuntimeConfigFilesAreSeparateIssues()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x014c);
        const QByteArray backend("backend");
        QVERIFY(writeFile(paths.javaExe, java));
        QVERIFY(writeFile(paths.backendJar, backend));

        const Xc2PrerequisiteReport report = Xc2InstallationProbe::inspectForTest(
            paths.root, arbitraryValidPolicy(), {});
        QVERIFY(hasIssue(report, QStringLiteral("missing_log_config")));
        QVERIFY(hasIssue(report, QStringLiteral("missing_runtime_config")));
        QVERIFY(!report.ok());
    }

    void unreadableRuntimeConfigFilesAreBlocking()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x014c);
        const QByteArray backend("backend");
        const QByteArray provider = syntheticPe(0x014c);
        QVERIFY(populateInstallation(paths, java, backend, provider));
        const Xc2ValidationPolicy policy = matchingPolicy(backend, provider);

        const QStringList configFiles = {
            paths.logConfig,
            paths.runtimeConfig,
        };
        for (const QString &configFile : configFiles) {
            const ExclusiveFileHandle lock(configFile);
            QVERIFY2(lock.isValid(), qPrintable(configFile));
            const Xc2PrerequisiteReport report =
                Xc2InstallationProbe::inspectForTest(
                    paths.root, policy, readerFor(paths.rootXml));
            QVERIFY2(hasIssueForPath(report, QStringLiteral("file_unreadable"),
                                     configFile),
                     qPrintable(configFile));
            QVERIFY(!report.ok());
        }
    }

    void hashMismatchBlocksProduction()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x014c);
        const QByteArray backend("synthetic backend cannot match production");
        QVERIFY(writeFile(paths.javaExe, java));
        QVERIFY(writeFile(paths.backendJar, backend));
        QVERIFY(writeFile(paths.logConfig, QByteArray("<log/>")));
        QVERIFY(writeFile(paths.runtimeConfig, QByteArray("profile=local\n")));

        const Xc2PrerequisiteReport report =
            Xc2InstallationProbe::inspectProduction(paths.root);
        QVERIFY(hasIssue(report, QStringLiteral("backend_hash_mismatch")));
        QVERIFY(!report.ok());
    }

    void invalidHashPolicyIsBlocking()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x014c);
        const QByteArray backend("backend");
        const QByteArray provider = syntheticPe(0x014c);
        QVERIFY(populateInstallation(paths, java, backend, provider));

        QList<Xc2ValidationPolicy> policies;
        policies.append({QByteArray(), sha256Hex(provider)});
        policies.append({QByteArray(63, 'A'), sha256Hex(provider)});
        policies.append({QByteArray(64, 'Z'), sha256Hex(provider)});
        policies.append({sha256Hex(backend), QByteArray()});

        for (const Xc2ValidationPolicy &policy : policies) {
            const Xc2PrerequisiteReport report =
                Xc2InstallationProbe::inspectForTest(
                    paths.root, policy, readerFor(paths.rootXml));
            QVERIFY2(hasIssue(report, QStringLiteral("invalid_policy")),
                     "invalid or empty hashes must never disable validation");
            QVERIFY(!report.ok());
        }
    }

    void x64JavaIsRejected()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x8664);
        const QByteArray backend("backend");
        const QByteArray provider = syntheticPe(0x014c);
        QVERIFY(populateInstallation(paths, java, backend, provider));

        const Xc2PrerequisiteReport report = Xc2InstallationProbe::inspectForTest(
            paths.root, matchingPolicy(backend, provider),
            readerFor(paths.rootXml));
        QVERIFY(hasIssue(report, QStringLiteral("wrong_java_arch")));
        QVERIFY(!hasIssue(report, QStringLiteral("wrong_provider_arch")));
        QVERIFY(!report.ok());
    }

    void x64ProviderIsRejected()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x014c);
        const QByteArray backend("backend");
        const QByteArray provider = syntheticPe(0x8664);
        QVERIFY(populateInstallation(paths, java, backend, provider));

        const Xc2PrerequisiteReport report = Xc2InstallationProbe::inspectForTest(
            paths.root, matchingPolicy(backend, provider),
            readerFor(paths.rootXml));
        QVERIFY(hasIssue(report, QStringLiteral("wrong_provider_arch")));
        QVERIFY(!hasIssue(report, QStringLiteral("wrong_java_arch")));
        QVERIFY(!report.ok());
    }

    void malformedAndTruncatedPeFilesAreRejected()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray backend("backend");
        QByteArray malformedProvider = syntheticPe(0x014c);
        malformedProvider[0x80] = 'N';
        const QByteArray truncatedJava("MZ", 2);
        QVERIFY(populateInstallation(paths, truncatedJava, backend,
                                     malformedProvider));

        Xc2PrerequisiteReport report = Xc2InstallationProbe::inspectForTest(
            paths.root, matchingPolicy(backend, malformedProvider),
            readerFor(paths.rootXml));
        QCOMPARE(issueCount(report, QStringLiteral("invalid_pe")), 2);

        QByteArray outOfBounds = syntheticPe(0x014c);
        qToLittleEndian<quint32>(0xfffffff0U,
            reinterpret_cast<uchar *>(outOfBounds.data() + 0x3c));
        QVERIFY(writeFile(paths.javaExe, outOfBounds));
        QVERIFY(writeFile(paths.providerDll, syntheticPe(0x014c)));
        report = Xc2InstallationProbe::inspectForTest(
            paths.root, matchingPolicy(backend, syntheticPe(0x014c)),
            readerFor(paths.rootXml));
        QVERIFY(hasIssue(report, QStringLiteral("invalid_pe")));
    }

    void registryLookupUsesLogicalKeyAndRegistry32View()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x014c);
        const QByteArray backend("backend");
        const QByteArray provider = syntheticPe(0x014c);
        QVERIFY(populateInstallation(paths, java, backend, provider));

        std::optional<Xc2RegistryRequest> captured;
        const Xc2PrerequisiteReport report = Xc2InstallationProbe::inspectForTest(
            paths.root, matchingPolicy(backend, provider),
            [&captured, &paths](const Xc2RegistryRequest &request)
                -> std::optional<QString> {
                captured = request;
                return paths.rootXml;
            });

        QVERIFY(captured.has_value());
        QCOMPARE(captured->format, QSettings::Registry32Format);
        QCOMPARE(captured->key,
                 QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\D-PDU API"));
        QVERIFY(!captured->key.contains(QStringLiteral("Wow6432Node"),
                                        Qt::CaseInsensitive));
        QCOMPARE(captured->valueName, QStringLiteral("Root File"));
        QVERIFY(report.ok());
    }

    void dpduRootXmlSelectsAvlVci2kAmongDecoys()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x014c);
        const QByteArray backend("backend");
        const QByteArray provider = syntheticPe(0x014c);
        QVERIFY(populateInstallation(paths, java, backend, provider));

        const Xc2PrerequisiteReport report = Xc2InstallationProbe::inspectForTest(
            paths.root, matchingPolicy(backend, provider),
            readerFor(paths.rootXml));
        QCOMPARE(QDir::cleanPath(report.layout.providerDll),
                 QDir::cleanPath(paths.providerDll));
        QVERIFY(!hasIssue(report, QStringLiteral("unsupported_dpdu_provider")));
        QVERIFY(!hasIssue(report, QStringLiteral("unsafe_provider_uri")));
        QVERIFY(report.ok());
    }

    void dpduLocalDriveUriAcceptsSlashVariantsAndSpaces()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x014c);
        const QByteArray backend("backend");
        const QByteArray provider = syntheticPe(0x014c);
        QVERIFY(populateInstallation(paths, java, backend, provider));
        const Xc2ValidationPolicy policy = matchingPolicy(backend, provider);

        const QStringList uris = {
            legacyFileUri(paths.providerDll),
            legacyFileUri(paths.providerDll, true),
            QString(legacyFileUri(paths.providerDll)).replace(
                QLatin1Char(' '), QStringLiteral("%20")),
        };

        for (const QString &uri : uris) {
            QVERIFY(writeFile(paths.rootXml, providerXml(uri)));
            const Xc2PrerequisiteReport report =
                Xc2InstallationProbe::inspectForTest(
                    paths.root, policy, readerFor(paths.rootXml));
            QCOMPARE(QDir::cleanPath(report.layout.providerDll),
                     QDir::cleanPath(paths.providerDll));
            QVERIFY2(report.ok(), qPrintable(uri));
        }
    }

    void dpduRemoteOrRelativeUriIsRejected()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const InstallationPaths paths = installationPaths(temp.path());
        const QByteArray java = syntheticPe(0x014c);
        const QByteArray backend("backend");
        const QByteArray provider = syntheticPe(0x014c);
        QVERIFY(populateInstallation(paths, java, backend, provider));
        const Xc2ValidationPolicy policy = matchingPolicy(backend, provider);

        const QStringList unsafeUris = {
            QStringLiteral("file://server/share/PDUAPI.dll"),
            QStringLiteral("file:/\\\\server\\share\\PDUAPI.dll"),
            QStringLiteral("file:relative/PDUAPI.dll"),
            QStringLiteral("file:/C:relative/PDUAPI.dll"),
            QStringLiteral("https://server/PDUAPI.dll"),
        };

        for (const QString &uri : unsafeUris) {
            QVERIFY(writeFile(paths.rootXml, providerXml(uri)));
            const Xc2PrerequisiteReport report =
                Xc2InstallationProbe::inspectForTest(
                    paths.root, policy, readerFor(paths.rootXml));
            QVERIFY2(hasIssue(report, QStringLiteral("unsafe_provider_uri")),
                     qPrintable(uri));
            QVERIFY(!report.ok());
            QVERIFY(report.layout.providerDll.isEmpty());
        }
    }

    void settingsRoundTripUsesCanonicalStore()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString rootWithTrailingSlash = QDir(temp.path()).filePath(
            QStringLiteral("XC_2/resources/app/"));
        const QString expected = QDir::cleanPath(rootWithTrailingSlash);

        Xc2Settings::clearInstallRoot();
        Xc2Settings::setInstallRoot(rootWithTrailingSlash);
        QCOMPARE(Xc2Settings::installRoot(), expected);

        QSettings store = rx14::appSettings();
        QCOMPARE(store.value(QStringLiteral("ktm/xc2InstallRoot")).toString(),
                 expected);
        QVERIFY(QDir::fromNativeSeparators(store.fileName())
                    .startsWith(QDir::fromNativeSeparators(m_settingsDir.path())
                                    + QLatin1Char('/')));

        Xc2Settings::clearInstallRoot();
        QVERIFY(!store.contains(QStringLiteral("ktm/xc2InstallRoot")));
        QVERIFY(Xc2Settings::installRoot().isEmpty());
    }

private:
    QTemporaryDir m_settingsDir;
    QSettings::Format m_previousSettingsFormat = QSettings::NativeFormat;
};

QTEST_APPLESS_MAIN(Xc2InstallationProbeTest)
#include "test_Xc2InstallationProbe.moc"
