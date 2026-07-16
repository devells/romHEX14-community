# KTM XC2 Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and verify the Windows-only XC2 contract, installation probe, owned sidecar lifecycle, strict REST transport, STOMP transport, and job registry that all KTM diagnostics and flashing features will use.

**Architecture:** A Windows-gated `rx14_ktm_xc2` static library owns no UI and loads no vendor DLL. It validates an external XC2 installation, starts the approved 32-bit Java sidecar as an owned process, and communicates only over loopback REST and STOMP/WebSocket. Pure codecs and value models remain independent of process/network classes so they can be exhaustively tested without an ECU or VCI.

**Tech Stack:** C++17, Qt 6.5+ Core/Network/WebSockets/Test, CMake/CTest, Windows MinGW 13.1 release baseline, XC2 REST `/xc2/1.0`, STOMP `/xc2-websocket`.

## Global Constraints

- The KTM XC2 feature defaults ON on Windows and OFF on macOS/Linux.
- `RX14_KTM_XC2=ON` requires Qt 6.5+ WebSockets and fails configuration with a direct prerequisite error when unavailable.
- romHEX14 must never load the 32-bit `PDUAPI_AVLDitest.dll`; it remains behind the 32-bit Java sidecar boundary.
- The feature must not start or embed `XC2.exe` or use Qt WebEngine.
- The approved initial backend SHA-256 is `B64A38C47F74D02145F462CDDA16EEA421A2170146155602575A1BD8B7E62840`.
- The approved initial D-PDU provider SHA-256 is `3F790B47D3F968FE2E757F309BFFDC0448C2FCE8A488DFA7C00F1FEDCB2A3075`.
- The only selectable D-PDU API is the root-file entry whose short name is `AVL Ditest VCI2K_DPDU_API`; other discovered interfaces are rejected before `device/apply`.
- The sidecar binds only to `127.0.0.1` on a manager-selected port and arbitrary pre-existing services on port 8082 are never reused.
- Health/read operations may be retried only by their owning controller; state-changing requests have zero automatic retries.
- External XC2 JARs, vendor DLLs, credentials, dealer data, and firmware are never committed to this repository.
- Phase 1 performs no VCI discovery, ECU requests, DTC clearing, workflow execution, or firmware programming.
- Use `rx14::appSettings()` for new settings. Do not add new values to the legacy `CT14/RX14` store.
- Every production class must have a deterministic fake or loopback test seam; no unit test may require XC2, Java, VCI2K, or vehicle hardware.

## Full Delivery Sequence

This is the first of four sequential implementation plans required by the approved design:

1. This plan: contract, sidecar, REST, STOMP, and job foundation.
2. `2026-07-16-ktm-xc2-diagnostics-plan.md`: connection UI, vehicle detection, autoscan, ECU identification, DTC, and measurements.
3. `2026-07-16-ktm-xc2-diflow-plan.md`: native dynamic flow renderer, functions, actuators, learning, coding, and guided diagnostics.
4. `2026-07-16-ktm-xc2-flashing-plan.md`: mapping resolution, artifact snapshots, preflight gates, operation journal, automatic/file flashing, and bench verification.

Each later plan consumes only the reviewed public interfaces produced here. The full thread goal remains incomplete until all four plans and the VCI2K bench audit pass.

---

## Task 1: Windows Feature Gate, CTest, and Contract Profile

**Files:**
- Modify: `CMakeLists.txt:24-46,106-326,382-435,514-530`
- Modify: `.github/workflows/release.yml:28-52`
- Create: `src/ktm/xc2/Xc2ContractProfile.h`
- Create: `src/ktm/xc2/Xc2ContractProfile.cpp`
- Create: `tests/ktm/test_Xc2ContractProfile.cpp`

**Interfaces:**
- Consumes: No KTM production interface.
- Produces: `ktm::xc2::Xc2ContractProfile`, `Endpoint`, `EndpointSpec`, `OperationSemantics`, `Topic`, and `rx14_ktm_xc2` static library target.

- [ ] **Step 1: Add the failing contract-profile test and CTest registration**

Create `tests/ktm/test_Xc2ContractProfile.cpp` with these behavioral assertions:

```cpp
#include <QtTest>

#include "ktm/xc2/Xc2ContractProfile.h"

using namespace ktm::xc2;

class Xc2ContractProfileTest final : public QObject {
    Q_OBJECT
private slots:
    void approvedHashesAreExact()
    {
        const auto &p = Xc2ContractProfile::approved();
        QCOMPARE(p.backendSha256Hex(),
                 QByteArray("B64A38C47F74D02145F462CDDA16EEA421A2170146155602575A1BD8B7E62840"));
        QCOMPARE(p.providerSha256Hex(),
                 QByteArray("3F790B47D3F968FE2E757F309BFFDC0448C2FCE8A488DFA7C00F1FEDCB2A3075"));
        QCOMPARE(p.restContext(), QStringLiteral("/xc2/1.0"));
        QCOMPARE(p.webSocketPath(), QStringLiteral("/xc2-websocket"));
    }

    void pathsAndTopicsAreUniqueAndRelative()
    {
        QSet<QString> paths;
        for (Endpoint endpoint : Xc2ContractProfile::allEndpoints()) {
            const EndpointSpec spec = Xc2ContractProfile::approved().endpoint(endpoint);
            QVERIFY2(!spec.path.startsWith(QStringLiteral("http")), qPrintable(spec.path));
            QVERIFY2(!paths.contains(spec.path), qPrintable(spec.path));
            paths.insert(spec.path);
        }
        QSet<QString> topics;
        for (Topic topic : Xc2ContractProfile::allTopics()) {
            const QString destination = Xc2ContractProfile::approved().topic(topic);
            QVERIFY(destination.startsWith(QStringLiteral("/topic/")));
            QVERIFY(!topics.contains(destination));
            topics.insert(destination);
        }
    }

    void stateChangingEndpointsNeverRetry()
    {
        for (Endpoint endpoint : Xc2ContractProfile::allEndpoints()) {
            const EndpointSpec spec = Xc2ContractProfile::approved().endpoint(endpoint);
            if (spec.semantics == OperationSemantics::StateChanging)
                QCOMPARE(spec.maxAutomaticRetries, 0);
        }
    }

    void deviceSelectionIsRestrictedToAvlVci2k()
    {
        const auto &p = Xc2ContractProfile::approved();
        QCOMPARE(p.supportedPduApiShortName(),
                 QStringLiteral("AVL Ditest VCI2K_DPDU_API"));
        const EndpointSpec apply = p.endpoint(Endpoint::DeviceApply);
        QCOMPARE(apply.path, QStringLiteral("device/apply"));
        QCOMPARE(apply.method, HttpMethod::PostJson);
        QCOMPARE(apply.semantics, OperationSemantics::StateChanging);
        QCOMPARE(apply.maxAutomaticRetries, 0);
    }
};

QTEST_APPLESS_MAIN(Xc2ContractProfileTest)
#include "test_Xc2ContractProfile.moc"
```

Add `include(CTest)`, `BUILD_TESTING`-guarded `Qt6::Test`, the platform default option, the empty `rx14_ktm_xc2` target, and the test target in `CMakeLists.txt`. Use this exact gate:

```cmake
if(WIN32)
    set(RX14_KTM_XC2_DEFAULT ON)
else()
    set(RX14_KTM_XC2_DEFAULT OFF)
endif()
option(RX14_KTM_XC2 "Enable KTM XC2 integration" ${RX14_KTM_XC2_DEFAULT})

include(CTest)

if(RX14_KTM_XC2)
    if(NOT WIN32)
        message(FATAL_ERROR "RX14_KTM_XC2 is supported only on Windows.")
    endif()
    find_package(Qt6 6.5 REQUIRED COMPONENTS WebSockets)
endif()

if(BUILD_TESTING AND RX14_KTM_XC2)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
endif()
```

Create and register KTM test targets only inside
`if(BUILD_TESTING AND RX14_KTM_XC2)`. Register this test with
`add_test(NAME xc2_contract_profile COMMAND test_Xc2ContractProfile)` and
labels `unit;ktm`. A non-Windows build with `RX14_KTM_XC2=OFF` must not search
for either Qt Test or Qt WebSockets.

- [ ] **Step 2: Configure/build to verify the test is red**

Run:

```powershell
rtk cmake -S . -B build-test -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DRX14_KTM_XC2=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64
rtk cmake --build build-test --target test_Xc2ContractProfile --parallel
```

Expected: compilation fails because `ktm/xc2/Xc2ContractProfile.h` and its types do not exist. On the current workstation, the earlier prerequisite failure `cmake: program not found` is expected until Task 1's toolchain prerequisite is installed; it is not accepted as the behavioral red test.

- [ ] **Step 3: Implement the complete immutable profile interface**

Create `Xc2ContractProfile.h` with this public surface:

```cpp
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace ktm::xc2 {

enum class OperationSemantics { ReadOnly, StateChanging };
enum class HttpMethod { Get, PostJson, PostForm, Delete };

enum class Endpoint {
    ServiceStatus, Shutdown, CurrentUser, Login, Logout,
    DeviceLookup, DeviceGet, DeviceGetSelected, DeviceApply, DeviceClose,
    SettingsGet, SettingsSet,
    VehicleDetect, VehicleSelect, VehicleInfo, AutoScan,
    EcuDomains, EcuOpen, EcuClose, EcuScan, EcuClearDtc,
    EcuFunctions, EcuMeasurementsGet, EcuMeasurementsStart,
    EcuMeasurementsStop, EcuExecuteFlow, FlowUpdateGui,
    DownloadMapping, FlashAutomatic, SelectFlashFile, FlashFile
};

enum class Topic {
    VciStatus, VehicleInfo, Ecu, Progress, MeasurementValues,
    FlowGui, FlowProgress, Login
};

struct EndpointSpec {
    QString path;
    HttpMethod method = HttpMethod::Get;
    OperationSemantics semantics = OperationSemantics::ReadOnly;
    int maxAutomaticRetries = 0;
};

class Xc2ContractProfile final {
public:
    static const Xc2ContractProfile &approved();
    static QList<Endpoint> allEndpoints();
    static QList<Topic> allTopics();

    QByteArray backendSha256Hex() const;
    QByteArray providerSha256Hex() const;
    QString profileId() const;
    QString restContext() const;
    QString webSocketPath() const;
    QString supportedPduApiShortName() const;
    EndpointSpec endpoint(Endpoint endpoint) const;
    QString topic(Topic topic) const;

private:
    Xc2ContractProfile() = default;
};

} // namespace ktm::xc2
```

Implement every enum value with the observed XC2 path/topic. Dynamic paths use named templates such as `ecu/open/{ecuId}`; later clients replace only declared placeholders after percent-encoding. Classify discovery/jobs, selection, clear, flow, and flash endpoints as `StateChanging` with zero retries. `ServiceStatus`, `CurrentUser`, settings reads, vehicle info, ECU-domain/function reads, and measurement-definition reads are `ReadOnly`; keep their profile retry value zero because retry policy belongs to the caller.

Use the observed device routes exactly: `device/lookup` (GET), `device/get`
(GET), `device/getSelected` (GET), `device/apply` (JSON POST), and
`device/close` (JSON POST). Device apply/close are state-changing. Later device
filtering and apply code must compare the selected candidate's D-PDU API short
name to `supportedPduApiShortName()` before issuing `device/apply`; do not expose
an arbitrary provider override.

- [ ] **Step 4: Build and run the profile test**

Run:

```powershell
rtk cmake --build build-test --target test_Xc2ContractProfile --parallel
rtk ctest --test-dir build-test --output-on-failure -R xc2_contract_profile
```

Expected: `100% tests passed, 0 tests failed`.

- [ ] **Step 5: Update Windows CI prerequisites**

In `.github/workflows/release.yml`, add `modules: 'qtwebsockets'` to the Windows `install-qt-action`, configure with `-DBUILD_TESTING=ON -DRX14_KTM_XC2=ON`, and run:

```yaml
      - name: KTM foundation tests
        run: ctest --test-dir build --output-on-failure -L ktm
```

Set `-DRX14_KTM_XC2=OFF` explicitly for macOS and Linux configure commands.

- [ ] **Step 6: Commit the build gate and profile**

```powershell
rtk git add CMakeLists.txt .github/workflows/release.yml src/ktm/xc2/Xc2ContractProfile.* tests/ktm/test_Xc2ContractProfile.cpp
rtk git commit -m "feat: define KTM XC2 contract profile"
```

---

## Task 2: Strict Models, JSON Codec, and Sanitized Fixtures

**Files:**
- Create: `src/ktm/xc2/Xc2Models.h`
- Create: `src/ktm/xc2/Xc2Models.cpp`
- Create: `src/ktm/xc2/Xc2JsonCodec.h`
- Create: `src/ktm/xc2/Xc2JsonCodec.cpp`
- Create: `tests/ktm/test_Xc2JsonCodec.cpp`
- Create: `tests/ktm/fixtures/README.md`
- Create: `tests/ktm/fixtures/manifest.json`
- Create: `tests/ktm/fixtures/rest/service-status-alive.txt`
- Create: `tests/ktm/fixtures/rest/current-user.json`
- Create: `tests/ktm/fixtures/rest/job-accepted.json`
- Create: `tests/ktm/fixtures/rest/error.json`
- Create: `tests/ktm/fixtures/rest/contract-missing-field.json`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Endpoint` and `Xc2ContractProfile` from Task 1.
- Produces: `Xc2Error`, `Xc2Result<T>`, `Xc2ServiceStatus`, `Xc2CurrentUser`, `Xc2JobAccepted`, `Xc2JobProgress`, and strict `Xc2JsonCodec` parse functions.

- [ ] **Step 1: Add red tests for valid, malformed, and missing-field payloads**

Create `test_Xc2JsonCodec.cpp` with fixture loading and these cases:

```cpp
void Xc2JsonCodecTest::serviceStatusRequiresAlive()
{
    QVERIFY(Xc2JsonCodec::serviceStatus("alive").ok());
    const auto bad = Xc2JsonCodec::serviceStatus("starting");
    QVERIFY(!bad.ok());
    QCOMPARE(bad.error.category, Xc2ErrorCategory::Contract);
}

void Xc2JsonCodecTest::currentUserRequiresIdentityAndPermissions()
{
    const auto ok = Xc2JsonCodec::currentUser(loadFixture("rest/current-user.json"));
    QVERIFY(ok.ok());
    QCOMPARE(ok.value->id, QStringLiteral("xcd"));
    QVERIFY(ok.value->permissions.contains(QStringLiteral("EcuDiagnosticRead")));

    const auto bad = Xc2JsonCodec::currentUser(
        loadFixture("rest/contract-missing-field.json"));
    QVERIFY(!bad.ok());
    QCOMPARE(bad.error.category, Xc2ErrorCategory::Contract);
    QVERIFY(bad.error.message.contains(QStringLiteral("permissions")));
}

void Xc2JsonCodecTest::errorRetainsContext()
{
    const auto result = Xc2JsonCodec::error(
        loadFixture("rest/error.json"), 403, QStringLiteral("ecu/scan"));
    QCOMPARE(result.httpStatus, 403);
    QCOMPARE(result.xc2Code, 403);
    QVERIFY(!result.rawPayload.isEmpty());
}
```

Register `xc2_json_codec` with labels `unit;contract;ktm` and compile definition `KTM_FIXTURE_DIR` pointing at `tests/ktm/fixtures`.

- [ ] **Step 2: Run the codec test to verify red behavior**

Run:

```powershell
rtk cmake --build build-test --target test_Xc2JsonCodec --parallel
```

Expected: compile failure for missing `Xc2Models.h`/`Xc2JsonCodec.h`.

- [ ] **Step 3: Implement value types and strict results**

Define the following exact core types in `Xc2Models.h`:

```cpp
enum class Xc2ErrorCategory {
    None, Prerequisite, Backend, Contract, Session, Vci,
    Vehicle, Job, FlashCritical, Transport
};

struct Xc2Error {
    Xc2ErrorCategory category = Xc2ErrorCategory::None;
    QString message;
    int httpStatus = 0;
    int xc2Code = 0;
    QString endpoint;
    QString jobId;
    QString ecuId;
    QJsonObject dpdu;
    QByteArray rawPayload;
};

template<typename T>
struct Xc2Result {
    std::optional<T> value;
    Xc2Error error;
    bool ok() const { return value.has_value(); }
    static Xc2Result success(T v) { return {std::move(v), {}}; }
    static Xc2Result failure(Xc2Error e) { return {std::nullopt, std::move(e)}; }
};

struct Xc2ServiceStatus { bool alive = false; };
struct Xc2CurrentUser {
    QString id;
    QString name;
    QString dealerBrand;
    QStringList permissions;
};
struct Xc2JobAccepted { QString jobId; };

enum class Xc2JobState {
    Created, InProgress, Finished, Canceled, Error, NotAuthorized
};

struct Xc2JobProgress {
    QString jobId;
    Xc2JobState state = Xc2JobState::Created;
    qint64 ticks = 0;
    qint64 totalTicks = 0;
    QString message;
    QJsonObject raw;
};
```

Add `Q_DECLARE_METATYPE` for signal-carried types. Use `std::optional`, not sentinel empty strings, for result success.

- [ ] **Step 4: Implement strict codec functions**

Expose:

```cpp
class Xc2JsonCodec final {
public:
    static Xc2Result<Xc2ServiceStatus> serviceStatus(const QByteArray &body);
    static Xc2Result<Xc2CurrentUser> currentUser(const QByteArray &body);
    static Xc2Result<Xc2JobAccepted> jobAccepted(const QByteArray &body);
    static Xc2Result<Xc2JobProgress> jobProgress(const QByteArray &body);
    static Xc2Error error(const QByteArray &body, int httpStatus,
                          const QString &endpoint);
};
```

Each JSON function must use `QJsonParseError`, require an object, require every documented field with its exact type, reject unknown job terminal states, preserve raw bytes on failure, and never treat `{}` or `null` as a successful user/job payload. Accept both observed `jobID` and progress `jobId` only in their corresponding schemas; do not silently alias arbitrary casing.

- [ ] **Step 5: Add sanitized golden fixtures**

Use these fixture semantics:

```json
{
  "id": "xcd",
  "name": "Development User",
  "dealerbrand": "ktm",
  "permissions": ["VehicleDetectExecute", "AutoscanExecute", "EcuDiagnosticRead", "EcuFlashAuto", "EcuFlashCustom", "EcuFlashFile"]
}
```

`job-accepted.json` contains `{"jobID":"00000000-0000-0000-0000-000000000001"}`. `error.json` contains only synthetic IDs and a 403 missing-permission error. `manifest.json` records the approved backend profile ID, capture date `2026-07-16`, route/topic, and that VIN/dealer identifiers were replaced. Do not copy real VINs, credentials, or dealer data.

- [ ] **Step 6: Run codec and profile tests**

```powershell
rtk cmake --build build-test --target test_Xc2JsonCodec test_Xc2ContractProfile --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: both tests pass.

- [ ] **Step 7: Commit models, codec, and fixtures**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2Models.* src/ktm/xc2/Xc2JsonCodec.* tests/ktm
rtk git commit -m "feat: add strict XC2 contract models"
```

---

## Task 3: Incremental STOMP Frame Codec

**Files:**
- Create: `src/ktm/xc2/Xc2StompCodec.h`
- Create: `src/ktm/xc2/Xc2StompCodec.cpp`
- Create: `tests/ktm/test_Xc2StompCodec.cpp`
- Create: `tests/ktm/fixtures/stomp/connected.frame`
- Create: `tests/ktm/fixtures/stomp/progress-message.frame`
- Create: `tests/ktm/fixtures/stomp/error.frame`
- Modify: `tests/ktm/fixtures/manifest.json`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Xc2Error` from Task 2.
- Produces: `Xc2StompFrame`, `Xc2StompDecodeResult`, and `Xc2StompCodec::feed/encode`.

- [ ] **Step 1: Write failing parser tests**

Cover all of these exact cases in `test_Xc2StompCodec.cpp`:

```cpp
void fragmentedAndCoalescedFrames();
void heartbeatIsNotAFrame();
void contentLengthAllowsEmbeddedNull();
void headersEscapeRoundTrip();
void malformedFrameReturnsProtocolError();
void goldenConnectedProgressAndErrorFramesParse();
```

The fragmented test feeds `"MESS"`, then the remainder of a MESSAGE plus a
complete ERROR frame, and asserts that zero then two frames are returned in
order. The content-length test uses a body containing `A\0B` and asserts a
three-byte body rather than stopping at the embedded NUL.

- [ ] **Step 2: Run the parser test to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2StompCodec --parallel
```

Expected: missing-header compile failure.

- [ ] **Step 3: Implement the codec public interface**

```cpp
struct Xc2StompFrame {
    QByteArray command;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
};

struct Xc2StompDecodeResult {
    QList<Xc2StompFrame> frames;
    QList<Xc2Error> errors;
    int heartbeats = 0;
};

class Xc2StompCodec final {
public:
    Xc2StompDecodeResult feed(const QByteArray &bytes);
    void reset();
    static QByteArray encode(const Xc2StompFrame &frame);
private:
    QByteArray m_buffer;
};
```

Implement STOMP 1.2 header escaping (`\\`/`\n`/`\r`/`\c`), LF heartbeats,
CRLF tolerance, NUL termination, and `content-length`. Cap buffered data at
8 MiB; overflow emits `Contract` error and resets. Reject invalid escape
sequences, negative/non-numeric content length, missing command, and a frame
whose declared body is not followed by NUL.

- [ ] **Step 4: Add and parse sanitized STOMP fixtures**

`connected.frame` negotiates `version:1.2` and `heart-beat:10000,10000`.
`progress-message.frame` targets `/topic/progress` and carries the synthetic
job ID from Task 2. `error.frame` has an artificial protocol error only.

- [ ] **Step 5: Run all pure contract tests**

```powershell
rtk cmake --build build-test --target test_Xc2StompCodec --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: all KTM tests pass with no network process.

- [ ] **Step 6: Commit the STOMP codec**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2StompCodec.* tests/ktm
rtk git commit -m "feat: add incremental XC2 STOMP codec"
```

---

## Task 4: XC2 Settings and Installation Prerequisite Probe

**Files:**
- Create: `src/ktm/xc2/Xc2Settings.h`
- Create: `src/ktm/xc2/Xc2Settings.cpp`
- Create: `src/ktm/xc2/Xc2InstallationProbe.h`
- Create: `src/ktm/xc2/Xc2InstallationProbe.cpp`
- Create: `tests/ktm/test_Xc2InstallationProbe.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: approved hashes/profile from Task 1 and `Xc2Error` from Task 2.
- Produces: `Xc2Settings`, `Xc2InstallLayout`, `Xc2ValidationPolicy`, `Xc2PrerequisiteReport`, and `Xc2InstallationProbe::inspect`.

- [ ] **Step 1: Write failing tests with a temporary synthetic installation**

Test these independent results:

```cpp
void emptyRootReportsMissingRoot();
void missingJavaAndJarAreSeparateIssues();
void hashMismatchBlocksProduction();
void x64ProviderIsRejected();
void dpduRootXmlResolvesAvlLibraryUri();
void settingsRoundTripUsesCanonicalStore();
```

Create a synthetic PE by writing `MZ`, an `e_lfanew` at offset `0x3c`, `PE\0\0`,
and machine `0x014c` or `0x8664`. Inject temporary expected hashes through
`Xc2ValidationPolicy`; do not weaken the production approved profile.

- [ ] **Step 2: Run the probe test to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2InstallationProbe --parallel
```

Expected: missing interface compile failure.

- [ ] **Step 3: Implement settings and report types**

```cpp
class Xc2Settings final {
public:
    static QString installRoot();
    static void setInstallRoot(const QString &root);
    static void clearInstallRoot();
};

struct Xc2InstallLayout {
    QString root;
    QString javaExe;
    QString backendJar;
    QString configDir;
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
```

`Xc2Settings` reads/writes only `rx14::appSettings()` key
`ktm/xc2InstallRoot`. Tests set temporary QSettings user scope and remove the
key in cleanup.

- [ ] **Step 4: Implement deterministic installation inspection**

Expose:

```cpp
struct Xc2ValidationPolicy {
    QByteArray backendSha256Hex;
    QByteArray providerSha256Hex;
    QString dpduRootOverride;
};

class Xc2InstallationProbe final {
public:
    static Xc2PrerequisiteReport inspect(
        const QString &installRoot,
        const Xc2ValidationPolicy &policy);
};
```

Resolve Java/JAR/config relative to the selected XC2 application root. If no
test override is provided, read the 32-bit D-PDU root registry value and parse
the root XML `LIBRARY_FILE URI`. Normalize the observed `file:/C:\\...` form
without treating it as a remote URL. Hash with `QCryptographicHash::Sha256`,
verify PE machine `0x014c`, and return all issues in one report rather than
stopping at the first error. Inspection opens files read-only and never loads
the provider DLL.

- [ ] **Step 5: Run the prerequisite tests**

```powershell
rtk cmake --build build-test --target test_Xc2InstallationProbe --parallel
rtk ctest --test-dir build-test --output-on-failure -R xc2_installation
```

Expected: all six cases pass.

- [ ] **Step 6: Commit settings and the read-only probe**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2Settings.* src/ktm/xc2/Xc2InstallationProbe.* tests/ktm/test_Xc2InstallationProbe.cpp
rtk git commit -m "feat: validate external XC2 installation"
```

---

## Task 5: Strict Loopback REST Client

**Files:**
- Create: `src/ktm/xc2/Xc2RestClient.h`
- Create: `src/ktm/xc2/Xc2RestClient.cpp`
- Create: `tests/ktm/FakeHttpServer.h`
- Create: `tests/ktm/FakeHttpServer.cpp`
- Create: `tests/ktm/test_Xc2RestClient.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 endpoint specs and Task 2 strict codecs/models.
- Produces: `Xc2RequestId`, `Xc2RestClient::requestServiceStatus/requestCurrentUser/requestShutdown`, reply signals, `cookieHeaderFor`, and `abort`.

- [ ] **Step 1: Write a loopback fake server and failing REST tests**

The fake server binds `QHostAddress::LocalHost` on port zero, queues exact HTTP
responses, captures method/path/headers/body, and exposes `requestCount()`.
Tests cover:

```cpp
void rejectsNonLoopbackBaseUrl();
void parsesPlainAliveHealth();
void parsesStrictCurrentUserJson();
void distinguishesTransportHttpAndContractErrors();
void retainsSessionCookieForWebSocketHandshake();
void shutdownPostsExactlyOnceWithoutRetry();
void abortEmitsCanceledResultOnce();
```

For shutdown, close the fake connection without a response and assert
`requestCount() == 1` after twice the read retry interval.

- [ ] **Step 2: Run the REST target to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2RestClient --parallel
```

Expected: missing-client compile failure.

- [ ] **Step 3: Implement the domain-oriented public interface**

```cpp
using Xc2RequestId = quint64;

class Xc2RestClient final : public QObject {
    Q_OBJECT
public:
    explicit Xc2RestClient(QObject *parent = nullptr,
                           QNetworkAccessManager *manager = nullptr);
    bool setBaseUrl(const QUrl &loopbackRestBase, Xc2Error *error = nullptr);
    QUrl baseUrl() const;

    Xc2RequestId requestServiceStatus();
    Xc2RequestId requestCurrentUser();
    Xc2RequestId requestShutdown();
    QByteArray cookieHeaderFor(const QUrl &url) const;
    void abort(Xc2RequestId id);

signals:
    void serviceStatusFinished(Xc2RequestId,
        const Xc2Result<Xc2ServiceStatus> &);
    void currentUserFinished(Xc2RequestId,
        const Xc2Result<Xc2CurrentUser> &);
    void shutdownFinished(Xc2RequestId, const Xc2Error &);
};
```

The client owns a dedicated `QNetworkCookieJar` when it owns the network
manager. Validate host as literal loopback/localhost, build paths only from
`Xc2ContractProfile`, attach request ID and endpoint enum as reply properties,
and delete each reply after a single terminal signal. Use a 10-second transfer
timeout for health/session reads. The client performs no automatic retry.

- [ ] **Step 4: Implement strict reply handling and cookie export**

Treat any 2xx as HTTP success, then apply the endpoint codec. A malformed body
becomes `Xc2ErrorCategory::Contract`; network failure with no HTTP status becomes
`Transport`; a JSON XC2 error preserves status/code/raw body. Build the Cookie
header by joining `QNetworkCookie::toRawForm(QNetworkCookie::NameAndValueOnly)`
for the WebSocket URL.

- [ ] **Step 5: Run REST and contract tests**

```powershell
rtk cmake --build build-test --target test_Xc2RestClient --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: all tests pass and the fake server records no retry for shutdown.

- [ ] **Step 6: Commit the REST client**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2RestClient.* tests/ktm/FakeHttpServer.* tests/ktm/test_Xc2RestClient.cpp
rtk git commit -m "feat: add strict XC2 REST transport"
```

---

## Task 6: Owned Sidecar Process Manager

**Files:**
- Create: `src/ktm/xc2/Xc2BackendManager.h`
- Create: `src/ktm/xc2/Xc2BackendManager.cpp`
- Create: `tests/ktm/fake_xc2_sidecar.cpp`
- Create: `tests/ktm/test_Xc2BackendManager.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: validated layout from Task 4 and REST health/shutdown from Task 5.
- Produces: `Xc2LaunchSpec`, `Xc2BackendState`, `Xc2BackendEndpoints`, and asynchronous `Xc2BackendManager` lifecycle.

- [ ] **Step 1: Implement the fake sidecar test executable before manager code**

The fake sidecar is a `QCoreApplication` plus `QTcpServer`. It accepts:

```text
--port <n>
--ready-delay-ms <n>
--exit-before-ready
--ignore-shutdown
```

It writes one stdout and one stderr marker, returns plain `alive` for
`GET /xc2/1.0/serviceStatus/status`, exits cleanly after
`POST /xc2/1.0/serviceStatus/shutdown`, and never opens a non-loopback socket.

- [ ] **Step 2: Write failing manager lifecycle tests**

Use `QSignalSpy`/`QTRY_COMPARE_WITH_TIMEOUT` to cover:

```cpp
void buildsProductionJavaArguments();
void selectsLoopbackPortAndBecomesReady();
void capturesStdoutAndStderr();
void startupTimeoutDoesNotReportReady();
void earlyCrashReportsBackendError();
void stopShutsDownOnlyOwnedProcess();
void arbitraryServiceOn8082IsNotReused();
```

Pass `$<TARGET_FILE:fake_xc2_sidecar>` through compile definition
`FAKE_XC2_SIDECAR_PATH`.

- [ ] **Step 3: Run the manager target to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2BackendManager --parallel
```

Expected: missing manager compile failure.

- [ ] **Step 4: Implement launch spec and manager interface**

```cpp
struct Xc2LaunchSpec {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QHostAddress bindAddress = QHostAddress::LocalHost;
    quint16 port = 0;
    int startupTimeoutMs = 60000;
    int shutdownTimeoutMs = 5000;
};

enum class Xc2BackendState {
    Stopped, Starting, Probing, Ready, Stopping, Failed
};

struct Xc2BackendEndpoints {
    QUrl restBaseUrl;
    QUrl webSocketUrl;
};

class Xc2BackendManager final : public QObject {
    Q_OBJECT
public:
    explicit Xc2BackendManager(QObject *parent = nullptr);
    static Xc2LaunchSpec productionLaunchSpec(
        const Xc2InstallLayout &layout, quint16 port);
    void start(Xc2LaunchSpec spec);
    void stop();
    Xc2BackendState state() const;
    bool ownsProcess() const;
    Xc2BackendEndpoints endpoints() const;

signals:
    void stateChanged(Xc2BackendState);
    void ready(const Xc2BackendEndpoints &);
    void outputLine(bool standardError, const QString &line);
    void failed(const Xc2Error &);
    void stopped(int exitCode, QProcess::ExitStatus);
};
```

- [ ] **Step 5: Implement non-blocking ownership and health polling**

Use `QProcess::setProgram`, `setArguments`, and `setWorkingDirectory`; connect
`started`, both ready-read signals, `errorOccurred`, and `finished`. Allocate a
port by briefly binding `QTcpServer` to `127.0.0.1:0`, then close before launch.
The production arguments are, in order:

```text
-Dserver.address=127.0.0.1
-Dserver.port=<selected>
-Dssc.includezip=true
-Dlogging.config=config/log.xml
-Dloader.main=com.avl.ditest.xc2.Xc2NgApplication
-Dspring.config.additional-location=file:./config/custom-cloud.properties
-Dspring.profiles.active=dev
-jar
<absolute xc2_backend_patched.jar>
```

Poll health with bounded timer-owned GETs until ready/deadline. `stop()` posts
shutdown only when `ownsProcess` is true, then terminates after timeout and
kills only as a final owned-process cleanup. Never call `waitForStarted` or
`waitForFinished` on the UI thread.

- [ ] **Step 6: Run manager and all KTM tests**

```powershell
rtk cmake --build build-test --target test_Xc2BackendManager --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: manager reaches Ready only after health responds and shuts down the
fake child without leaving a process.

- [ ] **Step 7: Commit the owned manager**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2BackendManager.* tests/ktm/fake_xc2_sidecar.cpp tests/ktm/test_Xc2BackendManager.cpp
rtk git commit -m "feat: manage the XC2 sidecar lifecycle"
```

---

## Task 7: STOMP WebSocket Client and Topic Allowlist

**Files:**
- Create: `src/ktm/xc2/Xc2StompClient.h`
- Create: `src/ktm/xc2/Xc2StompClient.cpp`
- Create: `tests/ktm/test_Xc2StompClient.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: STOMP codec from Task 3, cookie header from Task 5, topic allowlist from Task 1.
- Produces: `Xc2StompState`, `Xc2StompSession`, `Xc2StompMessage`, and connection/subscription events.

- [ ] **Step 1: Write failing loopback WebSocket/STOMP tests**

Use `QWebSocketServer` in the test process. Assert:

```cpp
void sendsConnectThenSubscribesAfterConnected();
void passesCookieHeaderInHandshake();
void routesAllowedTopicMessage();
void rejectsTopicOutsideProfile();
void heartbeatTimeoutEmitsVisibilityLost();
void destructiveModeDisablesAutomaticReconnect();
```

The fake server validates CONNECT headers, responds with CONNECTED, captures
SUBSCRIBE frames, and sends the golden progress MESSAGE.

- [ ] **Step 2: Run the client target to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2StompClient --parallel
```

Expected: missing-client compile failure.

- [ ] **Step 3: Implement the client interface**

```cpp
enum class Xc2StompState { Disconnected, Connecting, Connected, Failed };

struct Xc2StompSession {
    QString version;
    int outgoingHeartbeatMs = 0;
    int incomingHeartbeatMs = 0;
};

struct Xc2StompMessage {
    QString destination;
    QString subscriptionId;
    QByteArray body;
};

class Xc2StompClient final : public QObject {
    Q_OBJECT
public:
    explicit Xc2StompClient(QObject *parent = nullptr);
    void connectToBackend(const QUrl &url, const QByteArray &cookieHeader);
    bool subscribe(Topic topic, Xc2Error *error = nullptr);
    void unsubscribe(Topic topic);
    void disconnectFromBackend();
    void setDestructiveJobActive(bool active);
    Xc2StompState state() const;

signals:
    void connected(const Xc2StompSession &);
    void messageReceived(const Xc2StompMessage &);
    void protocolError(const Xc2Error &);
    void visibilityLost();
    void disconnected();
};
```

- [ ] **Step 4: Implement handshake, heartbeat, and subscription rules**

Open `QWebSocket` with a `QNetworkRequest` carrying the Cookie header. Send
STOMP CONNECT with `accept-version:1.2` and `heart-beat:10000,10000`. Queue
requested topics until CONNECTED, then send stable subscription IDs derived
from the `Topic` enum. Feed all binary/text messages to `Xc2StompCodec`.
Negotiate heartbeat intervals per STOMP 1.2, emit LF on the outgoing timer, and
emit `visibilityLost()` when incoming traffic exceeds twice the negotiated
interval. Do not implement automatic reconnect in this class; the later
controller decides whether reconnect is allowed. In destructive mode, a socket
loss emits visibility loss and remains disconnected.

- [ ] **Step 5: Run STOMP client and codec tests**

```powershell
rtk cmake --build build-test --target test_Xc2StompClient test_Xc2StompCodec --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: handshake, cookie, routing, and visibility-loss tests pass.

- [ ] **Step 6: Commit the WebSocket client**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2StompClient.* tests/ktm/test_Xc2StompClient.cpp
rtk git commit -m "feat: add XC2 STOMP WebSocket transport"
```

---

## Task 8: Job Registry and Foundation Contract Probe

**Files:**
- Create: `src/ktm/xc2/Xc2JobRegistry.h`
- Create: `src/ktm/xc2/Xc2JobRegistry.cpp`
- Create: `tests/ktm/test_Xc2JobRegistry.cpp`
- Create: `tests/ktm/xc2_contract_probe.cpp`
- Create: `tests/ktm/run_xc2_mock_contract.ps1`
- Modify: `CMakeLists.txt`
- Modify: `docs/superpowers/plans/2026-07-16-ktm-xc2-foundation-plan.md`

**Interfaces:**
- Consumes: strict job progress model/codec from Task 2 and `/topic/progress` messages from Task 7.
- Produces: `Xc2JobRegistry`, terminal job records, a read-only live contract probe, and completed plan checkboxes.

- [ ] **Step 1: Write failing job-registry tests**

Cover:

```cpp
void acceptedJobStartsCreated();
void progressTransitionsCreatedToInProgressToFinished();
void terminalJobRejectsFurtherProgress();
void unknownJobIsCreatedFromProgressWithoutLosingEvent();
void visibilityLossPreservesStateAndLaterProgressRecovers();
void clearRemovesOnlyRequestedTerminalJob();
```

- [ ] **Step 2: Run registry tests to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2JobRegistry --parallel
```

Expected: missing-registry compile failure.

- [ ] **Step 3: Implement registry interface and invariants**

```cpp
struct Xc2JobRecord {
    QString jobId;
    Xc2JobState state = Xc2JobState::Created;
    QList<Xc2JobProgress> events;
    bool visibilityLost = false;
    bool terminal() const;
};

class Xc2JobRegistry final : public QObject {
    Q_OBJECT
public:
    explicit Xc2JobRegistry(QObject *parent = nullptr);
    bool accept(const Xc2JobAccepted &job, Xc2Error *error = nullptr);
    bool apply(const Xc2JobProgress &progress, Xc2Error *error = nullptr);
    void markVisibilityLost();
    std::optional<Xc2JobRecord> job(const QString &jobId) const;
    QList<Xc2JobRecord> activeJobs() const;
    bool clearTerminal(const QString &jobId);

signals:
    void jobChanged(const Xc2JobRecord &);
    void jobTerminal(const Xc2JobRecord &);
};
```

Allowed backend-state transitions are `Created -> InProgress -> terminal` and
direct `Created -> terminal`. `markVisibilityLost()` sets the independent
`visibilityLost` flag on non-terminal records without changing their last known
backend state. The next accepted authoritative progress event for the same
`jobId` clears that flag and may continue to `InProgress` or a terminal state.
No event can change Finished/Canceled/Error/NotAuthorized to another state.
Keep every accepted ordered progress event and test loss followed by recovered
progress and completion.

- [ ] **Step 4: Add the read-only contract probe**

`xc2_contract_probe` accepts `--base-url` and performs only:

1. loopback URL validation;
2. `GET serviceStatus/status`;
3. `GET auth/currentUser`;
4. STOMP CONNECT plus subscriptions to status topics;
5. clean DISCONNECT.

It must not call device lookup, vehicle detection, autoscan, ECU endpoints, flow,
or flash. Exit codes are `0` compatible, `2` prerequisite/URL, `3` REST schema,
`4` STOMP protocol, and `5` missing local session permission.

`run_xc2_mock_contract.ps1` takes mandatory `-BaseUrl`; it never starts a JAR.
This prevents tests from selecting the wrong mock/production artifact. The
current local mock at port 8082 can be probed explicitly after login; CI uses
only fake/golden tests.

- [ ] **Step 5: Run the full foundation suite and optional local probe**

```powershell
rtk cmake --build build-test --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
rtk .\build-test\xc2_contract_probe.exe --base-url http://127.0.0.1:8082/xc2/1.0
```

Expected CTest result: `100% tests passed`. The optional probe returns `0` only
when the explicitly selected backend has a current permitted local session; an
unauthenticated mock returns exit `5` and is recorded as an environment result,
not converted into a passing contract result.

- [ ] **Step 6: Verify non-Windows builds exclude the feature**

Run in Linux/macOS CI:

```text
cmake -B build -DRX14_KTM_XC2=OFF -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Expected: romHEX14 builds without searching for Qt WebSockets or Qt Test, and
no KTM target/action is present.

- [ ] **Step 7: Mark every completed checkbox and self-review the implementation**

Run:

```powershell
rtk rg -n "TBD|TODO|FIXME|PLACEHOLDER" src/ktm tests/ktm docs/superpowers/plans/2026-07-16-ktm-xc2-foundation-plan.md
rtk git diff --check
rtk git status --short
```

Expected: no implementation placeholders, no whitespace errors, and only the
intentional plan checkbox update is uncommitted.

- [ ] **Step 8: Commit registry, probe, and completed plan state**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2JobRegistry.* tests/ktm docs/superpowers/plans/2026-07-16-ktm-xc2-foundation-plan.md
rtk git commit -m "feat: complete the XC2 communication foundation"
```

## Phase 1 Completion Gate

Before writing the diagnostics plan, verify all of the following with current
artifacts rather than intent:

- The Windows option and Qt WebSockets prerequisite behave as specified.
- Every KTM CTest target passes on Qt 6.8.3/MinGW 13.1.
- Golden fixtures contain no real VIN, credential, or dealer identity.
- Installation inspection is read-only and rejects wrong hash/PE bitness.
- The manager never reuses 8082 and shuts down only its owned fake child.
- REST is loopback-only, strict, cookie-preserving, and performs zero internal
  retry.
- STOMP handles fragmentation, heartbeats, content length, topic allowlisting,
  and visibility loss.
- Terminal job transitions cannot be rewritten.
- The optional live probe performs read-only contract operations only.
- The main application still builds with `RX14_KTM_XC2=OFF`.
