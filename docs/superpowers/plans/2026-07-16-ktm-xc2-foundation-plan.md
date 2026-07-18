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

    void routesAndTopicsAreUniqueAndRelative()
    {
        QSet<QString> routes;
        for (Endpoint endpoint : Xc2ContractProfile::allEndpoints()) {
            const EndpointSpec spec = Xc2ContractProfile::approved().endpoint(endpoint);
            QVERIFY2(!spec.path.startsWith(QStringLiteral("http")), qPrintable(spec.path));
            const QString routeKey = QStringLiteral("%1:%2")
                .arg(static_cast<int>(spec.method))
                .arg(spec.path);
            QVERIFY2(!routes.contains(routeKey), qPrintable(routeKey));
            routes.insert(routeKey);
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

    void manualVehicleSelectionAndVehicleFlowRoutesAreFrozen()
    {
        const auto &p = Xc2ContractProfile::approved();
        QCOMPARE(p.endpoint(Endpoint::VehicleManufacturers).path,
                 QStringLiteral("vehicle/manufacturer"));
        QCOMPARE(p.endpoint(Endpoint::VehicleSeries).path,
                 QStringLiteral("vehicle/series"));
        QCOMPARE(p.endpoint(Endpoint::VehicleModels).path,
                 QStringLiteral("vehicle/vehicle"));
        QCOMPARE(p.endpoint(Endpoint::VehicleSelect).path,
                 QStringLiteral("vehicle/select"));
        QCOMPARE(p.endpoint(Endpoint::VehicleExecuteFlow).path,
                 QStringLiteral("vehicle/executeFlow"));
        QCOMPARE(p.endpoint(Endpoint::VehicleExecuteFlow).method,
                 HttpMethod::PostForm);
    }

    void callableProfileExcludesStaleFrontendGetFunctions()
    {
        const QString deadRoute = QStringLiteral("ecu/getFunctions/{ecuId}");
        for (Endpoint endpoint : Xc2ContractProfile::allEndpoints()) {
            const EndpointSpec spec =
                Xc2ContractProfile::approved().endpoint(endpoint);
            QVERIFY2(spec.path != deadRoute, qPrintable(spec.path));
        }
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
    VehicleDetect, VehicleManufacturers, VehicleSeries, VehicleModels,
    VehicleSelect, VehicleInfo, AutoScan,
    EcuDomains, EcuOpen, EcuClose, EcuScan, EcuClearDtc,
    EcuMeasurementsGet, EcuMeasurementsStart,
    EcuMeasurementsStop, EcuExecuteFlow, VehicleExecuteFlow, FlowUpdateGui,
    DownloadMapping, FlashAutomatic, FlashFile
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

Implement every enum value with a callable path/topic supported by the pinned
patched backend JAR. Dynamic paths use named templates such as
`ecu/open/{ecuId}`; later clients replace only declared placeholders after
percent-encoding. Classify discovery/jobs, selection, clear, flow, and flash
endpoints as `StateChanging` with zero retries. `ServiceStatus`, `CurrentUser`,
settings reads, vehicle info, ECU-domain reads, and measurement-definition reads
are `ReadOnly`; keep their profile retry value zero because retry policy belongs
to the caller.

The served stale frontend/profile mentions `GET ecu/getFunctions/{ecuId}`, but
the pinned patched JAR exposes no matching JAX endpoint. Keep that route and
`Endpoint::EcuFunctions` out of the callable profile. Function and actuator
metadata comes from `EcuDomain`, and execution uses the existing DIFLOW
`ecu/executeFlow`/`vehicle/executeFlow` routes.

Manual selection consumes the observed JSON POST routes
`vehicle/manufacturer`, `vehicle/series`, and `vehicle/vehicle`, followed by
state-changing `vehicle/select`. Classify the first three list queries as
`ReadOnly` even though XC2 exposes them as POST. Preserve both form POST flow
routes: `ecu/executeFlow` and `vehicle/executeFlow`.

Use the observed device routes exactly: `device/lookup` (GET), `device/get`
(GET), `device/getSelected` (GET), `device/apply` (JSON POST), and
`device/close` (JSON POST). Device apply/close are state-changing. Later device
filtering and apply code must compare the selected candidate's `internalName`
to `supportedPduApiShortName()` before issuing `device/apply`; do not expose an
arbitrary provider override. The apply/close JSON body is the complete strict
`Xc2VciDevice` object (`id`, `name`, `internalName`, and optional
`additionalModuleInformation`), never just a device ID.

File selection is a native romHEX14 UI concern and is deliberately absent from
`Endpoint`: the observed XC2 frontend uses a local file input and sends only
the resulting path through form POST `ecu/flashFile`. Do not invent a
`selectFlashFile` backend route.

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
- Create: `tests/ktm/fixtures/rest/job-progress.json`
- Create: `tests/ktm/fixtures/rest/device-get.json`
- Create: `tests/ktm/fixtures/rest/error.json`
- Create: `tests/ktm/fixtures/rest/contract-missing-field.json`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Endpoint` and `Xc2ContractProfile` from Task 1.
- Produces: `Xc2Error`, `Xc2Result<T>`, `Xc2ServiceStatus`, `Xc2CurrentUser`, `Xc2VciDevice`, `Xc2JobAccepted`, `Xc2LocalizedText`, `Xc2JobProgress`, and strict `Xc2JsonCodec` parse/serialization functions.

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
    QCOMPARE(ok.value->loginName, QStringLiteral("xcd"));
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
    QCOMPARE(result.xc2Status, 403);
    QCOMPARE(result.xc2Code, 1007);
    QCOMPARE(result.developerMessage, QStringLiteral("synthetic detail"));
    QCOMPARE(result.info, QStringLiteral("synthetic info"));
    QVERIFY(!result.rawPayload.isEmpty());
}

void Xc2JsonCodecTest::vciDeviceRoundTripsCompleteApplyPayload()
{
    const auto devices = Xc2JsonCodec::vciDevices(
        loadFixture("rest/device-get.json"));
    QVERIFY(devices.ok());
    QCOMPARE(devices.value->size(), 1);
    const Xc2VciDevice &device = devices.value->front();
    QCOMPARE(device.internalName,
             QStringLiteral("AVL Ditest VCI2K_DPDU_API"));
    const QJsonObject body = Xc2JsonCodec::vciDeviceJson(device);
    QCOMPARE(body.value(QStringLiteral("id")).toString(), device.id);
    QCOMPARE(body.value(QStringLiteral("name")).toString(), device.name);
    QCOMPARE(body.value(QStringLiteral("internalName")).toString(),
             device.internalName);
    QVERIFY(!device.additionalModuleInformation.has_value());
    QVERIFY(body.value(QStringLiteral("additionalModuleInformation")).isNull());
}

void Xc2JsonCodecTest::vciOptionalInformationIsStrict()
{
    QVERIFY(Xc2JsonCodec::vciDevices(
        R"([{"id":"a","name":"A","internalName":"AVL Ditest VCI2K_DPDU_API"}])").ok());
    QVERIFY(Xc2JsonCodec::vciDevices(
        R"([{"id":"a","name":"A","internalName":"AVL Ditest VCI2K_DPDU_API","additionalModuleInformation":"synthetic"}])").ok());
    QVERIFY(!Xc2JsonCodec::vciDevices(
        R"([{"id":"a","name":"A","internalName":"AVL Ditest VCI2K_DPDU_API","additionalModuleInformation":7}])").ok());
}

void jobAcceptanceUsesExactWireFieldName();
void jobProgressFixtureUsesLocalizedTextObject();
void jobProgressAcceptsLocalizedTextObjectAndNull_data();
void jobProgressAcceptsLocalizedTextObjectAndNull();
void jobProgressRejectsInvalidLocalizedText_data();
void jobProgressRejectsInvalidLocalizedText();
void jobProgressPreservesInt64LocalizedTextId_data();
void jobProgressPreservesInt64LocalizedTextId();
void jobProgressAcceptsInt32CounterBoundaries_data();
void jobProgressAcceptsInt32CounterBoundaries();
void jobProgressRejectsInvalidCounters_data();
void jobProgressRejectsInvalidCounters();
void jobProgressAcceptsOnlyFiveWireStatuses_data();
void jobProgressAcceptsOnlyFiveWireStatuses();
void jobProgressRejectsStatusAliasesAndFieldCasing_data();
void jobProgressRejectsStatusAliasesAndFieldCasing();
void jobProgressRetainsUnknownTopLevelFieldsInRaw();
```

All status and field-casing rows must keep every unrelated field valid under the
authoritative schema. Accept `message` only as a complete localized-text object
or JSON null. Reject a legacy string, missing message, arrays/scalars, missing
localized fields, wrong types, non-integral IDs, and values outside signed
64-bit range. Test exact signed 64-bit ID boundaries. Test `ticks` and
`totalTicks` independently at signed 32-bit minimum/maximum and reject overflow,
fractional, string, null, and boolean values. Every failure retains the exact
input in `rawPayload`.

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
    int xc2Status = 0;
    int xc2Code = 0;
    QString developerMessage;
    QString info;
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
    QString loginName;
    QString name;
    std::optional<QString> dealerId;
    std::optional<QString> country;
    std::optional<QString> address1;
    std::optional<QString> address2;
    std::optional<QString> dealerType;
    std::optional<QString> audience;
    QStringList permissions;
    std::optional<QString> sessionIndex;
};
struct Xc2JobAccepted { QString jobId; };

struct Xc2VciDevice {
    QString id;
    QString name;
    QString internalName;
    std::optional<QString> additionalModuleInformation;
};

enum class Xc2JobState {
    Created, InProgress, Finished, Canceled, Error, NotAuthorized
};

struct Xc2LocalizedText {
    qint64 id = 0;
    QString text;
};

struct Xc2JobProgress {
    QString jobId;
    Xc2JobState state = Xc2JobState::Created;
    qint32 ticks = 0;
    qint32 totalTicks = 0;
    std::optional<Xc2LocalizedText> message;
    QJsonObject raw;
};
```

Place every type and codec in `namespace ktm::xc2`. Put
`Q_DECLARE_METATYPE(ktm::xc2::...)` declarations outside that namespace for
`Xc2Error`, `Xc2LocalizedText`, each signal-carried model, and the concrete `Xc2Result<T>`
specializations used by Tasks 5-8. Use `std::optional`, not sentinel empty
strings, for result success or nullable DTO properties.

- [ ] **Step 4: Implement strict codec functions**

Expose:

```cpp
class Xc2JsonCodec final {
public:
    static Xc2Result<Xc2ServiceStatus> serviceStatus(const QByteArray &body);
    static Xc2Result<Xc2CurrentUser> currentUser(const QByteArray &body);
    static Xc2Result<QList<Xc2VciDevice>> vciDevices(const QByteArray &body);
    static QJsonObject vciDeviceJson(const Xc2VciDevice &device);
    static Xc2Result<Xc2JobAccepted> jobAccepted(const QByteArray &body);
    static Xc2Result<Xc2JobProgress> jobProgress(const QByteArray &body);
    static Xc2Error error(const QByteArray &body, int httpStatus,
                          const QString &endpoint);
};
```

Each JSON function must use `QJsonParseError`, require the documented top-level
kind, require every documented field with its exact type, reject unknown job
terminal states, preserve raw bytes on failure, and never treat `{}` or `null`
as a successful user/job payload. `currentUser()` maps wire `loginName` directly
and requires string `loginName`, string `name`, and a string array
`permissions`; it must not consume the mock data-source fields `id`,
`dealerbrand`, or `rights`. The other documented user fields are nullable or
absent strings. `jobProgress()` reads only wire field `status` and accepts only
`IN_PROGRESS`, `FINISHED`, `CANCELED`, `ERROR`, and `NOT_AUTHORIZED`; `Created`
is an internal registry state and is never accepted from the wire.
Its exact wire object is
`{jobId:string,status:string,ticks:int32,totalTicks:int32,message:{id:int64,text:string}|null}`.
Both counter fields are required signed Java `int` values. `message` is required
but nullable; a non-null value requires exact signed 64-bit integer `id` and
string `text`. Reject the old string message representation, aliases, field
casing changes, non-integral numbers, and numeric overflow. Qt 6.8 can saturate
an out-of-range JSON integer at the signed 64-bit minimum, so validate the
original integer token before conversion. Preserve unknown top-level fields
only in `Xc2JobProgress::raw` and preserve the original bytes on every failure.
`vciDevices()` requires an array whose items
contain string `id`, `name`, and `internalName`; it accepts
`additionalModuleInformation` only as string, JSON null, or absent. Its
serializer emits all four documented DTO property names and no arbitrary
unknown fields. Accept both observed `jobID` and progress `jobId` only in their
corresponding schemas; do not silently alias arbitrary casing. `error()` keeps
the HTTP status argument separate from wire `status` and `code`, parses the
five wire properties `status`, `message`, `code`, `devMessage`, and `info`, and
maps the last two into `developerMessage` and `info` without equating the XC2
code to the HTTP status.

- [ ] **Step 5: Add sanitized golden fixtures**

Use these fixture semantics:

```json
{
  "loginName": "xcd",
  "name": "Development User",
  "dealerId": "DEALER-REDACTED",
  "country": "AT",
  "address1": "REDACTED",
  "address2": "",
  "dealerType": "DEALER",
  "audience": "KTM",
  "permissions": ["VehicleDetectExecute", "AutoscanExecute", "EcuDiagnosticRead", "EcuFlashAuto", "EcuFlashCustom", "EcuFlashFile"],
  "sessionIndex": "session-redacted"
}
```

`job-accepted.json` contains
`{"jobID":"00000000-0000-0000-0000-000000000001"}`.
`job-progress.json` uses that ID plus exact fields `status:"IN_PROGRESS"`,
signed 32-bit integer `ticks`/`totalTicks`, and
`message:{"id":42,"text":"synthetic progress"}`. `error.json` contains the
five wire fields with `status:403`, `code:1007`, and only synthetic text.
`manifest.json` records the approved backend profile ID, a manifest revision
date, route/topic, and that VIN/dealer identifiers were replaced. The REST and
STOMP progress fixtures are `synthetic-contract-fixture`, have
`schemaAuthority:"served-app+patched-backend-jar"`, and set
`liveCapture:false`; a manifest-wide date must never imply they are live
captures. Do not copy real VINs, credentials, or dealer data.

`device-get.json` contains one synthetic device with all four documented DTO
properties and `internalName` exactly `AVL Ditest VCI2K_DPDU_API`; no real VCI
serial number or network address is retained.

- [ ] **Step 6: Run codec and profile tests**

```powershell
rtk cmake --build build-test --target test_Xc2JsonCodec test_Xc2ContractProfile --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: both tests pass.

- [ ] **Step 7: Commit models, codec, and fixtures**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2Models.* src/ktm/xc2/Xc2JsonCodec.* tests/ktm/test_Xc2JsonCodec.cpp tests/ktm/fixtures/README.md tests/ktm/fixtures/manifest.json tests/ktm/fixtures/rest
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
- Consumes: `Xc2Error` and strict `Xc2JsonCodec::jobProgress()` from Task 2.
- Produces: `Xc2StompFrame`, `Xc2StompDecodeResult`, and `Xc2StompCodec::feed/encode`.

- [ ] **Step 1: Write failing parser tests**

Cover all of these exact cases in `test_Xc2StompCodec.cpp`:

```cpp
void fragmentedAndCoalescedFrames();
void heartbeatIsNotAFrame();
void contentLengthAllowsEmbeddedNull();
void contentLengthWaitsForFragmentedTerminator();
void connectHeadersRemainUnescaped();
void messageHeaderKeysAndValuesEscapeRoundTrip();
void duplicateHeadersKeepFirstValue();
void crlfHeartbeatAndTrailingEolAreIncremental();
void malformedFrameReturnsProtocolError();
void goldenConnectedProgressAndErrorFramesParse();
```

The fragmented test feeds `"MESS"`, then the remainder of a MESSAGE plus a
complete ERROR frame, and asserts that zero then two frames are returned in
order. The content-length test uses a body containing `A\0B` and asserts a
three-byte body rather than stopping at the embedded NUL; construct it as
`QByteArray("A\0B", 3)`, never with the NUL-truncating one-argument constructor.
The terminator-fragment test feeds exactly through the declared body first and
asserts no error, then feeds the terminating NUL and asserts one frame. A byte
other than NUL after the complete declared body is the error case.
The golden test requires the progress frame to be exactly 298 bytes with
`content-length:144`, a 144-byte body, and a final real `0x00`; it then passes
the decoded body through `Xc2JsonCodec::jobProgress()` and checks localized
message ID/text rather than merely searching for the job ID.

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

Implement STOMP 1.2 framing without globally rewriting the byte stream:

- `CONNECT` and `CONNECTED` header keys/values remain byte-for-byte unescaped;
  all other commands escape/unescape both keys and values for
  `\\`, `\n`, `\r`, and `\c`. Unknown escapes are fatal and spaces are never
  trimmed.
- Duplicate decoded headers keep the first wire value.
- Accept LF or CRLF line endings. A CR and LF split across `feed()` calls is
  one heartbeat/EOL. Permit CRLF/LF after a terminating NUL before the next
  frame without touching binary body bytes.
- Parse `content-length` into a checked 64-bit value. A declared value over
  8 MiB fails immediately; total buffered data over 8 MiB also emits
  `Contract` and resets.
- Once the complete declared body is buffered, wait for another fragment when
  no terminator byte is available. Reject only when an available next byte is
  not NUL.
- `encode()` always replaces a caller-provided `content-length` with the actual
  body size and adds it when the body contains NUL.

Reject negative/non-numeric content length, missing command, invalid escapes,
and a complete declared body followed by a non-NUL byte.

- [ ] **Step 4: Add and parse sanitized STOMP fixtures**

`connected.frame` records the observed read-only handshake negotiation
`version:1.2` and `heart-beat:0,0` after the client offered
`heart-beat:10000,10000`. Compatibility evidence is a
read-only `CONNECT`/`DISCONNECT` at `ws://127.0.0.1:8082/xc2-websocket` using
subprotocol `v12.stomp` on `2026-07-16`; record it in the fixture manifest.
`progress-message.frame` targets `/topic/progress`, includes required
`destination`, `message-id`, and `subscription` headers, and carries the
synthetic job ID and 144-byte compact authoritative progress JSON from Task 2.
It is a `synthetic-contract-fixture` whose schema authority is the served app
plus pinned patched backend JAR, not a live MESSAGE capture. `connected.frame`
alone is `observed-read-only-handshake`; `error.frame` is a
`synthetic-protocol-fixture`. Every `.frame` is a binary fixture ending in a
real `0x00` byte, not the two text characters `\\0`; tests assert the terminator
and exact byte count (298 bytes for progress).

- [ ] **Step 5: Run all pure contract tests**

```powershell
rtk cmake --build build-test --target test_Xc2StompCodec --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: all KTM tests pass with no network process.

- [ ] **Step 6: Commit the STOMP codec**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2StompCodec.* tests/ktm/test_Xc2StompCodec.cpp tests/ktm/fixtures/manifest.json tests/ktm/fixtures/stomp
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
- Produces: `Xc2Settings`, `Xc2InstallLayout`, `Xc2ValidationPolicy`, `Xc2RegistryRequest`, `Xc2PrerequisiteReport`, and separate production/test inspection entry points.

- [ ] **Step 1: Write failing tests with a temporary synthetic installation**

Test these independent results:

```cpp
void emptyRootReportsMissingRoot();
void missingAndNonDirectoryRootsNeverResolveAgainstCwd();
void missingJavaAndJarAreSeparateIssues();
void missingRuntimeConfigFilesAreSeparateIssues();
void hashMismatchBlocksProduction();
void invalidHashPolicyIsBlocking();
void x64JavaIsRejected();
void x64ProviderIsRejected();
void malformedAndTruncatedPeFilesAreRejected();
void registryLookupUsesLogicalKeyAndRegistry32View();
void dpduRootXmlSelectsAvlVci2kAmongDecoys();
void dpduLocalDriveUriAcceptsSlashVariantsAndSpaces();
void dpduRemoteOrRelativeUriIsRejected();
void settingsRoundTripUsesCanonicalStore();
```

Create a synthetic PE by writing `MZ`, an `e_lfanew` at offset `0x3c`, `PE\0\0`,
and machine `0x014c` or `0x8664`. Inject temporary expected hashes through
the explicitly test-only `Xc2ValidationPolicy`; do not weaken the production
approved profile. Prepend a decoy `MVCI_PDU_API` node to the D-PDU XML so the
test proves selection by `SHORT_NAME`, not by element order. Assert stable issue
codes, not localized message text.

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
```

`Xc2Settings` reads/writes only `rx14::appSettings()` key
`ktm/xc2InstallRoot`. The stored path is specifically the XC2 application root
whose path ends in `resources/app`, not the outer `XC_2` or `XC_2_Prog`
directory. Resolve exactly `jre/bin/java.exe`, `xc2_backend_patched.jar`,
`config/log.xml`, and `config/custom-cloud.properties` beneath it.

Before the first settings object is created, the test saves the global
QSettings format, switches the default to `IniFormat`, redirects
`IniFormat/UserScope` to a `QTemporaryDir`, and verifies
`rx14::appSettings().fileName()` lies there. Cleanup removes the key, calls
`sync()`, and restores the prior default format; the redirected path remains
process-local to the dedicated test executable.

- [ ] **Step 4: Implement deterministic installation inspection**

Expose:

```cpp
struct Xc2ValidationPolicy {
    QByteArray backendSha256Hex;
    QByteArray providerSha256Hex;
};

struct Xc2RegistryRequest {
    QSettings::Format format = QSettings::Registry32Format;
    QString key = QStringLiteral(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\D-PDU API");
    QString valueName = QStringLiteral("Root File");
};

using Xc2RegistryReader = std::function<std::optional<QString>(
    const Xc2RegistryRequest &request)>;

class Xc2InstallationProbe final {
public:
    static Xc2PrerequisiteReport inspectProduction(
        const QString &installRoot);
    static Xc2PrerequisiteReport inspectForTest(
        const QString &installRoot,
        const Xc2ValidationPolicy &policy,
        Xc2RegistryReader registryReader);
};
```

`inspectProduction()` always derives both hashes and the supported provider
short name from `Xc2ContractProfile::approved()` and uses the real Windows
reader. `inspectForTest()` is the only injectable-policy entry point. An
expected hash that is not exactly 64 hexadecimal characters, including an
empty value, produces blocking `invalid_policy`; it never disables validation.

The real registry reader opens the logical key
`HKEY_LOCAL_MACHINE\\SOFTWARE\\D-PDU API` with
`QSettings::Registry32Format` and reads value `Root File`. Do not combine the
32-bit view with an explicit `Wow6432Node` segment. The injected reader receives
`Xc2RegistryRequest`, allowing tests to assert all three fields before returning
a temporary root XML path.

In the XML, find the `MVCI_PDU_API` whose `SHORT_NAME` exactly equals
`AVL Ditest VCI2K_DPDU_API`, then read `LIBRARY_FILE URI` from that same node.
Accept only an absolute local drive form matching
`file:/[A-Za-z]:[\\/...]`; normalize slash direction and spaces deliberately.
Reject authority/host components, non-file schemes, UNC/network locations, and
relative paths rather than passing this legacy value through a permissive
generic URL resolver.

Blank, missing, or non-directory roots produce a blocking root issue before
any relative path is formed, so `QDir("")` can never fall back to the current
working directory. For a valid root, collect independent Java, JAR, config,
registry, XML, provider, hash, and PE issues in one report. Define `ok()` as
"no blocking issue". Use stable codes including `missing_root`,
`root_not_directory`, `missing_java`, `missing_backend`, `missing_log_config`,
`missing_runtime_config`, `missing_dpdu_registry`, `invalid_dpdu_xml`,
`unsupported_dpdu_provider`, `unsafe_provider_uri`, `file_unreadable`,
`invalid_pe`, `wrong_java_arch`, `wrong_provider_arch`,
`backend_hash_mismatch`, and `provider_hash_mismatch`.

PE inspection checks minimum length, `MZ`, bounded `e_lfanew`, `PE\0\0`, exact
reads, and machine `0x014c` independently for the bundled Java executable and
provider DLL. The same package contains unrelated x64 Java, so path and machine
checks are both mandatory. Hash with `QCryptographicHash::Sha256`. Every file
is opened read-only; the provider DLL is never loaded.

- [ ] **Step 5: Run the prerequisite tests**

```powershell
rtk cmake --build build-test --target test_Xc2InstallationProbe --parallel
rtk ctest --test-dir build-test --output-on-failure -R xc2_installation
```

Expected: all listed cases pass.

- [ ] **Step 6: Commit settings and the read-only probe**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2Settings.* src/ktm/xc2/Xc2InstallationProbe.* tests/ktm/test_Xc2InstallationProbe.cpp
rtk git commit -m "feat: validate external XC2 installation"
```

---

## Task 5: Strict Loopback REST Client

**Files:**
- Modify: `src/ktm/xc2/Xc2Models.h`
- Create: `src/ktm/xc2/Xc2RestClient.h`
- Create: `src/ktm/xc2/Xc2RestClient.cpp`
- Create: `tests/ktm/FakeHttpServer.h`
- Create: `tests/ktm/FakeHttpServer.cpp`
- Create: `tests/ktm/test_Xc2RestClient.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 endpoint specs and Task 2 strict codecs/models.
- Produces: `Xc2TransportReason`, `Xc2RequestId`,
  `Xc2RestClient::requestServiceStatus/requestCurrentUser/requestShutdown`,
  reply signals, raw-byte `setBaseUrl`, exact authority/path-bound
  `webSocketUrl`/`cookieHeaderFor`, exact shutdown-REST
  `shutdownCookieHeaderFor`, and `abort`.

**Evidence-locked transport constraints:**

- In Qt 6.8, constructing a `QUrl` irreversibly canonicalizes IPv4 aliases such
  as `127.1`, integer, and octal forms to `127.0.0.1`, and decodes unreserved
  path escapes such as `%78` and `%2e`. Therefore the trust-boundary input is
  `QByteArray`, and validation happens on the original visible-ASCII bytes
  before any `QUrl` is constructed.
- A live fake against Qt's private `QNetworkAccessManager` HTTP channel proved
  that one close-before-status can transparently replay the complete GET or
  POST request. The private channel permits up to two reconnects and has no
  public no-retry switch. Manual redirect policy does not remove that replay.
- Consequently this client never uses `QNetworkAccessManager`. Every operation
  gets one fresh `QTcpSocket`, explicit `QNetworkProxy::NoProxy`, one numeric
  loopback destination, one HTTP/1.1 request, and no reconnect, retry, or
  connection reuse.

- [ ] **Step 1: Write a complete loopback fake server before the client**

`FakeHttpServer` binds an explicitly supplied `QHostAddress::LocalHost` or
`QHostAddress::LocalHostIPv6` on port zero. It incrementally parses the request
line, headers, `Content-Length`, and body; a request enters the capture list and
increments `requestCount()` exactly once only after the complete body has been
received. Each capture exposes the exact method, request-target, headers, and
body. Normal queued responses always include an exact `Content-Length` and
`Connection: close`; special scripted responses support close-before-status,
declared-length truncation, drip bytes without completion, no bytes, delayed
completion, arbitrary raw responses, and raw responses fragmented at any byte
boundary. Raw scripts cover fragmented headers, chunked bodies and trailers,
204/no-body, close-delimited bodies, malformed framing, and repeated
`Set-Cookie`. A redirect script supplies `Location`. A trap server independently
counts requests so redirect tests prove that it was never contacted.

The server must let a test wait for `requestCaptured` before calling `abort()`;
do not use a timing guess that can cancel before the request reaches the wire.
Expose enough connection/request counts to prove that a close-before-status
response caused exactly one captured GET or POST and no hidden reconnect.

- [ ] **Step 2: Write failing raw-URL, rebase, and cookie-authority tests**

Add these data-driven cases to `test_Xc2RestClient.cpp`:

```cpp
void acceptsIpv4Ipv6AndLocalhostBases_data();
void acceptsIpv4Ipv6AndLocalhostBases();
void rejectsUnsafeOrAliasedBases_data();
void rejectsUnsafeOrAliasedBases();
void cookieExportRequiresDerivedWebSocketAuthority();
void shutdownCookieExportRequiresExactRestTarget();
void shutdownCookieExportUsesRestPathScope();
void successfulCrossAuthorityRebaseClearsCookieJar();
void rebaseWhilePendingFailsAndPreservesAuthorityAndCookies();
```

The accepted table contains all of these forms, with and without the one
permitted trailing slash where shown:

```text
http://127.0.0.1:<port>/xc2/1.0
http://127.0.0.1:<port>/xc2/1.0/
http://[::1]:<port>/xc2/1.0
http://LOCALHOST:<port>/xc2/1.0/
```

Call `setBaseUrl()` with the original `QByteArray`; the test must not construct a
`QUrl` first. The rejected table contains empty/relative input, bytes outside
visible ASCII `0x21..0x7e`, `https` and `ws`, a missing, zero, signed,
leading-zero, or overflowing port, user info, query, fragment, `localhost.`,
`foo.localhost`, `localhost.localdomain`, `127.1`, integer and octal IPv4
aliases, non-canonical bracket/IPv6 spellings, a non-loopback numeric IPv4/IPv6
address, `/xc2/1.0//`, a case-changed path, an extra segment, a literal dot
segment, and every percent-encoded path spelling, including unreserved `%78`
and `%2e` variants. Include regression rows proving that passing those aliases
through `QUrl` first would have hidden the distinction.

Validation performs no DNS lookup. Parse scheme, authority, explicit canonical
decimal port, and path directly from the raw bytes. Accept only `http`, port
`1..65535`, no user info/query/fragment, and a raw path byte-for-byte equal to
`/xc2/1.0` or `/xc2/1.0/`. The host is either case-insensitive exact
`localhost` or a numeric address whose input spelling is canonical and whose
parsed `QHostAddress` is loopback; never resolve a hostname alias. Only after
these checks may the implementation construct canonical internal `QUrl`
values. Canonicalize `localhost` to numeric IPv4 loopback and store the REST
context without a trailing slash.

The cookie test returns a synthetic session cookie, then accepts export only
for the one URL derived from the REST base:
`ws://<same validated host>:<same explicit port>/xc2-websocket`. The supplied
argument is also a `QByteArray` and must equal
`webSocketUrl().toEncoded(QUrl::FullyEncoded)` byte-for-byte before cookie
matching. Reject wrong/missing port, host aliases, alternate case or encoding,
`wss`, path suffix, query, and every semantically equivalent but non-identical
spelling. This exact comparison is mandatory because cookie matching itself is
not port-scoped. Join only the dedicated jar's returned cookies with
`QNetworkCookie::NameAndValueOnly`.

The shutdown export is a separate, equally exact API. Before configuration it
fails with `Contract`; afterwards its supplied `QByteArray` must equal the
internally derived
`http://<same validated host>:<same explicit port>/xc2/1.0/serviceStatus/shutdown`
target's `QUrl::toEncoded(QUrl::FullyEncoded)` bytes. Reject the WebSocket URL,
REST base, wrong scheme/host/port/path, user info, query, fragment, alternate
encoding, and every semantically equivalent but non-identical spelling. Match
cookies against that exact REST shutdown URL so default-path cookies from the
service-status route plus `Path=/xc2/1.0` and `Path=/` are exported, while
`Path=/xc2-websocket` is excluded. Conversely the WebSocket export includes
only cookies matching `/xc2-websocket`. There is no generic URL cookie-export
API and neither exact export may be retargeted to the other's path.

Once any request is pending, every `setBaseUrl()` call fails with `Contract` and
leaves `baseUrl()`, `webSocketUrl()`, and all cookies unchanged. A successful
change to another canonical authority replaces the dedicated cookie jar before
the new authority becomes usable, so neither REST nor WebSocket export can
inherit cookies from the old authority.

- [ ] **Step 3: Write failing exact-wire, no-retry, and redirect tests**

Add these cases:

```cpp
void sendsExactRequestTargetsAndHeaders();
void freshSocketBypassesApplicationProxy();
void closeBeforeStatusSendsOneRequestOnly_data();
void closeBeforeStatusSendsOneRequestOnly();
void restCookieContinuesAcrossRequests();
void get302DoesNotFollowRedirect();
void shutdown307DoesNotFollowRedirect();
```

For every endpoint build the request target as exactly
`restContext() + '/' + EndpointSpec::path`; never call `QUrl::resolved()`.
Assert targets are `/xc2/1.0/serviceStatus/status`,
`/xc2/1.0/auth/currentUser`, and `/xc2/1.0/serviceStatus/shutdown`, without a
doubled slash or inherited suffix. Every request carries `Host` with the
explicit port and IPv6 brackets when applicable, `Connection: close`, and
`Accept-Encoding: identity`. A shutdown is one `POST` with
`Content-Type: application/x-www-form-urlencoded`, `Content-Length: 0`, and an
empty body. A cookie established by one complete REST response is sent as
`Cookie` on the next matching REST request.

Temporarily install a trap application proxy and prove the fresh socket still
connects directly to numeric loopback. For both a GET and the shutdown POST,
have the origin capture the complete request and then close before any status
line. Wait beyond the request deadline and require exactly one request and one
connection: neither operation may be transparently replayed.

Queue a GET `302` and shutdown POST `307`, each pointing at an independent trap
server. The client has no redirect-following code: both operations fail, each
origin records exactly one request, and each trap remains at zero through the
full deadline.

- [ ] **Step 4: Write failing strict HTTP/1.1 parser and cookie-ingest tests**

Add parser rows for:

```cpp
void fragmentedHttpResponses_data();
void fragmentedHttpResponses();
void fragmentedChunkedResponseExportsMultipleCookies();
void malformedOrTruncatedChunkedIsTransportFailure_data();
void malformedOrTruncatedChunkedIsTransportFailure();
void acceptsCloseDelimitedResponse();
void rejectsAmbiguousOrOversizedResponses_data();
void rejectsAmbiguousOrOversizedResponses();
```

Fragment the status line, each header delimiter, body, chunk-size lines, chunk
terminators, zero chunk, and trailers across arbitrary socket reads. Require a
strict incremental parser for `Content-Length`, `Transfer-Encoding: chunked`
with syntactically valid trailers, 204/no-body, and EOF-terminated
close-delimited responses. A complete chunked response can deliver multiple
separate `Set-Cookie` headers; all are parsed and installed in the dedicated
jar, and their REST/WS export is deterministic.

Reject duplicate or invalid lengths, any `Transfer-Encoding` plus
`Content-Length`, unsupported transfer codings, malformed status/header/trailer
syntax, an invalid/overflowing chunk size, a malformed chunk terminator, bytes
after the terminating trailers, and EOF in any incomplete fixed-length or
chunked state. Cap the status/headers, each chunk line, and trailers at 64 KiB;
cap the decoded body at 8 MiB for every framing mode. A declared or accumulated
body above 8 MiB fails before unbounded buffering. A 204 carrying forbidden
body framing or bytes fails.

Only a syntactically complete response may mutate the cookie jar. Iterate every
`Set-Cookie` field separately, parse it with `QNetworkCookie`, and install it
against the canonical endpoint URL. A timeout, cancel, truncation, malformed
response, or over-limit response installs no cookies visible through either
the WebSocket or exact shutdown-REST export.

- [ ] **Step 5: Write failing deadline, exactly-once, and response-matrix tests**

Add these cases:

```cpp
void totalDeadlineStopsDripAndNeverResponses_data();
void totalDeadlineStopsDripAndNeverResponses();
void abortAfterCaptureCompletesCanceledExactlyOnce();
void timeoutAndFinishedRaceCompletesExactlyOnce();
void deletingClientInsideFinishedSlotIsSafeAndExactlyOnce();
void classifiesCompleteResponseMatrix_data();
void classifiesCompleteResponseMatrix();
void shutdownIsOneExactEmptyFormPost();
```

Use short injected test deadlines and `QElapsedTimer`. One server sends a byte
often enough to stay below the transfer inactivity timeout but never completes;
another accepts the request and never sends a byte. Each must finish once near
the independent total wall-clock deadline, not remain alive indefinitely.
The inactivity timer starts with the request and restarts only when response
bytes arrive; the total timer never restarts and remains authoritative.
After `requestCaptured`, `abort(id)` must produce exactly one result with
`Transport/Canceled`. Exercise timeout, socket error/disconnect, parser failure,
and abort in the same event-loop turn and assert one terminal signal and removal
of the pending request. Deleting the client from that signal must not access a
freed pending record or emit again.

The data-driven response matrix is exact:

| Wire result | Required result |
|---|---|
| connection closes before an HTTP status | `Transport/Network`, `httpStatus == 0` |
| HTTP 200 plus a truncated declared body | `Transport/Network`, preserving HTTP 200 and raw bytes |
| health HTTP 200 body `alive` with any or missing MIME type | success |
| health HTTP 204 empty | `Contract`, preserving HTTP 204/raw body |
| current-user HTTP 204 empty | `Contract`, preserving HTTP 204/raw body |
| shutdown complete HTTP 200 or 204 | success regardless of body/MIME |
| non-2xx with a valid XC2 error JSON | `Backend`, preserving HTTP status, wire `status`, wire `code`, and raw bytes |
| a malformed success payload or malformed non-2xx error | `Contract`, preserving any HTTP status and raw bytes |
| any 3xx | failure and no redirect; valid XC2 error JSON is classified `Backend` |

Framing/protocol failure and truncation are `Transport/Network` and take
precedence over the HTTP/body matrix while preserving any parsed status and raw
payload. Cancellation and timeout take precedence over the socket error caused
by `abort()`. The shutdown capture repeats the Step 3 exact-wire assertions;
close without a response and wait beyond the deadline to prove
`requestCount() == 1`.

- [ ] **Step 6: Run the REST target to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2RestClient --parallel
```

Expected: missing-client compile failure.

- [ ] **Step 7: Implement the raw-byte authority and domain interface**

```cpp
enum class Xc2TransportReason { None, Canceled, Timeout, Network };

using Xc2RequestId = quint64;

struct Xc2RestClientOptions {
    int totalDeadlineMs = 10000;
    int transferTimeoutMs = 3000;
};

class Xc2RestClient final : public QObject {
    Q_OBJECT
public:
    explicit Xc2RestClient(Xc2RestClientOptions options = {},
                           QObject *parent = nullptr);
    bool setBaseUrl(const QByteArray &encodedLoopbackRestBase,
                    Xc2Error *error = nullptr);
    QUrl baseUrl() const;
    QUrl webSocketUrl() const;

    Xc2RequestId requestServiceStatus();
    Xc2RequestId requestCurrentUser();
    Xc2RequestId requestShutdown();
    Xc2Result<QByteArray> cookieHeaderFor(
        const QByteArray &encodedWsUrl) const;
    Xc2Result<QByteArray> shutdownCookieHeaderFor(
        const QByteArray &encodedShutdownUrl) const;
    void abort(Xc2RequestId id);

signals:
    void serviceStatusFinished(Xc2RequestId,
        const Xc2Result<Xc2ServiceStatus> &);
    void currentUserFinished(Xc2RequestId,
        const Xc2Result<Xc2CurrentUser> &);
    void shutdownFinished(Xc2RequestId, const Xc2Error &);
};
```

Declare `Xc2TransportReason` immediately before the existing `Xc2Error` in
`Xc2Models.h` and insert the exact member
`Xc2TransportReason transportReason = Xc2TransportReason::None;` into
`Xc2Error`; no localized message parsing may substitute for this field.

Implement the raw validation and rebase transaction from Step 2 before creating
the canonical internal `QUrl` values. The client owns one dedicated
`QNetworkCookieJar`; there is no constructor or setter for an external jar. It
uses the jar manually for matching REST request cookies, installing every
complete-response `Set-Cookie`, exporting the exact derived WebSocket Cookie,
and separately exporting cookies for only the exact derived shutdown REST
target. It does not own or instantiate a `QNetworkAccessManager`, and exposes
no generic cookie-export URL.

- [ ] **Step 8: Implement one-shot sockets, the incremental parser, and one completion path**

Create one pending record per request containing the ID, endpoint, fresh
`QTcpSocket`, request bytes, total and inactivity timers, forced transport
reason, incremental parser/framing state, accumulated bounded payload, and a
completed flag. Set `QNetworkProxy::NoProxy` on the socket and call
`connectToHost(QHostAddress(canonicalHost), explicitPort)` so no DNS or proxy
path exists. Serialize one HTTP/1.1 request with the exact Step 3 target and
headers. Never create a replacement socket, reconnect, reuse a connection, or
resend bytes after any close or error.

Feed `readyRead` bytes into the strict Step 4 state machine. A fixed-length or
chunked response completes only at its exact terminator; a close-delimited
response completes only at EOF. A status line alone never proves transfer
completion. When a complete message is available, stop using the socket and
classify it once through the Step 5 response matrix.

All socket, parser, timer, and caller-cancel paths converge on one private
`completeOnce(id)` gate. `abort(id)` records `Canceled`, deadline expiry records
`Timeout`, and both abort the socket and schedule that gate; socket errors and
disconnects only record evidence and schedule the same gate. The gate stops
timers, removes the pending record before any terminal signal, cleans up the
socket/state, and emits exactly once. A non-forced incomplete or malformed
transfer maps to `Transport/Network`; successful HTTP/body classifications
retain `Xc2TransportReason::None`.

- [ ] **Step 9: Run REST and contract tests**

```powershell
rtk cmake --build build-test --target test_Xc2RestClient --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: raw-URL/rebase, exact wire/Host headers, one-request GET/POST,
redirect traps, manual cookie continuity/export, fragmented framing, parser
limits, response matrix, deadline, and exactly-once tests all pass. No test or
production path instantiates `QNetworkAccessManager`; shutdown is one exact
request and both redirect traps remain at zero.

- [ ] **Step 10: Commit the REST client**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2Models.h src/ktm/xc2/Xc2RestClient.* tests/ktm/FakeHttpServer.* tests/ktm/test_Xc2RestClient.cpp
rtk git commit -m "feat: add strict XC2 REST transport"
```

---

## Task 6: Owned Sidecar Process Manager

**Files:**
- Create: `src/ktm/xc2/Xc2BackendManager.h`
- Create: `src/ktm/xc2/Xc2BackendManager.cpp`
- Create: `src/ktm/xc2/internal/Xc2ProcessOutput.h`
- Create: `src/ktm/xc2/internal/Xc2ProcessOutput.cpp`
- Create: `tests/ktm/fake_xc2_sidecar.cpp`
- Create: `tests/ktm/xc2_manager_exit_helper.cpp`
- Create: `tests/ktm/test_Xc2BackendManager.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: fresh production inspection from Task 4 and encoded-base REST health
  and current-user requests from Task 5. The manager deliberately does not use
  Task 5's shutdown request because the state-changing write needs a
  connection-specific Windows PID proof.
- Produces: `Xc2BackendState`, `Xc2BackendEndpoints`, the exact validated startup
  user, bounded sanitized process output, and an asynchronous HANDLE/PID-owned
  `Xc2BackendManager` lifecycle. No public report, layout, launch spec, program,
  argv callback, candidate allocator, or lock-path seam has spawn authority.

- [ ] **Step 1: Implement the fake sidecar test executable before manager code**

The fake sidecar is a `QCoreApplication` plus `QTcpServer` that binds exactly
IPv4 `127.0.0.1`. It accepts:

```text
--port <n>
--ready-delay-ms <n>
--health-delay-ms <n>
--never-health
--late-alive-ms <n>
--exit-before-ready
--ignore-shutdown
--fragment-output
--oversize-output
--sensitive-output
--hold-lock <absolute path>
--user-mode <valid|empty-permissions|blank-login|blank-name|missing-permissions|malformed>
--release-listener-after-ready <absolute trigger path>
--event-log <absolute json-lines path>
```

It returns plain `alive` for exact
`GET /xc2/1.0/serviceStatus/status`, a strict synthetic identity with a
non-empty `permissions` array for exact `GET /xc2/1.0/auth/currentUser`, and
exits cleanly after exact empty-form
`POST /xc2/1.0/serviceStatus/shutdown`. It never opens a non-loopback socket.
The JSON-lines event log records `LISTENING`, every complete request, concurrent
and maximum concurrent health counts, listener release, shutdown bytes, and
process exit. It flushes `LOCKED` only after `--hold-lock` has actually obtained
the `QLockFile`. Listener handoff tests create the trigger file only after Ready,
wait for `LISTENER_RELEASED`, and then start a decoy on that exact port.

The user modes make missing/malformed/blank identity and empty permissions
independently testable. Output modes emit all nine sensitive field names on both
streams, but process-pipe write boundaries are not treated as deterministic:
exact fragmentation is tested by feeding chunks directly to the pure
`Xc2ProcessOutput` accumulator. `--hold-lock` waits after its flushed handshake,
allowing a test to kill the helper and leave a real stale lock file.

Pass `$<TARGET_FILE:fake_xc2_sidecar>` and
`$<TARGET_FILE:xc2_manager_exit_helper>` to the test through compile definitions
`FAKE_XC2_SIDECAR_PATH` and `XC2_MANAGER_EXIT_HELPER_PATH`.

- [ ] **Step 2: Write failing launch-profile and bind tests**

Add these cases first:

```cpp
void productionArgumentsOverrideEveryEmbedded8082();
void productionArgumentsKeepRequiredJavaOrderingAndProfile();
void productionStartCannotConsumeForgedOrTestReports();
void productionStartRunsFreshInspectionBeforeAnySideEffect();
void productionBuilderUsesExactApprovedPaths();
void productionProcessEnvironmentRemovesInjectionVariables();
void privateFakeArgumentsReceiveManagerSelectedPort();
void rejectsEveryBindAddressExceptIpv4Localhost_data();
void rejectsEveryBindAddressExceptIpv4Localhost();
```

The production argv builder is a pure private function exposed only through the
friend test-access class. It returns a `QStringList`; it cannot construct a
`QProcess`, acquire a lock, select a port, or start the manager. Call it with a
synthetic already-validated layout and port `49123`, then require these exact
effective JVM system properties, all before `-jar`:

```text
-Dserver.address=127.0.0.1
-Dserver.port=49123
-Donelogin.saml2.sp.assertion_consumer_service.url=http://127.0.0.1:49123/xc2/1.0/auth/samlACS
-Donelogin.saml2.sp.single_logout_service.url=http://127.0.0.1:49123/xc2/1.0/auth/samlLogoutSLO
-Donelogin.saml2.idp.single_sign_on_service.url=http://127.0.0.1:49123/xc2/1.0/auth/mock
-Donelogin.saml2.idp.single_logout_service.url=http://127.0.0.1:49123/xc2/1.0/auth/mock
-Dcom.avl.ditest.xc2.gripsresource.prefix=http://127.0.0.1:49123/xc2/1.0/mock/streamer?file={0}
```

These five approved templates override the four fixed-port URLs in
`config/custom-cloud.properties` and the GRIPS prefix in patched-JAR resource
`BOOT-INF/classes/xc2.properties`. Do not generically preserve or rewrite an
arbitrary source URL path/query. The vendor files remain read-only. Assert the
joined generated argv and the five effective override values contain no
`:8082`, every callback/GRIPS authority uses the selected port, and every
approved property key occurs exactly once. Require one `-jar` token, no JVM or
application argument after the absolute JAR path, and no duplicate key whose
last-one-wins behavior could weaken the profile.

Also require `-Dloader.main=com.avl.ditest.xc2.Xc2NgApplication` before `-jar`,
`-Dspring.profiles.active=dev`, and
`-Dcom.avl.ditest.xc2.developer=true`. The latter is the explicitly approved
local diagnostic launch profile only; it does not claim or simulate DealerNet
authentication.

Retain the existing required arguments
`-Dssc.includezip=true`, `-Dlogging.config=config/log.xml`, and
`-Dspring.config.additional-location=file:./config/custom-cloud.properties`;
all JVM `-D` arguments, including `loader.main`, the dynamic URL overrides, the
`dev` profile, and the diagnostic flag, precede the single `-jar` token and the
absolute `xc2_backend_patched.jar` path.

The public production entry point is only
`startProduction(const QString &installRoot)`. It performs a fresh
`Xc2InstallationProbe::inspectProduction(installRoot)` inside the start attempt
and uses the returned layout immediately; it never accepts a public
`Xc2PrerequisiteReport`, `Xc2InstallLayout`, mutable launch spec, program, or
argv callback. A hand-built empty-issue report, an `inspectForTest()` report,
and a previously valid report whose layout was later mutated therefore have no
call path into production startup. Assert that the startup deadline begins
before the fresh inspection and that failed production inspection does not
create the lock directory, invoke the candidate allocator, or spawn.

Separately exercise the private pure builder with a synthetic already-validated
production layout; no unit test depends on an external XC2 installation. Assert
the direct `QProcess` program is the absolute checked `jre/bin/java.exe`, the
working directory is the exact `resources/app` root, and the final argument is
its absolute approved `xc2_backend_patched.jar`; never use `cmd.exe`,
PowerShell, `start`, `javaw`, or PATH lookup. Build a child environment from the
current environment but remove, case-insensitively on Windows,
`JAVA_TOOL_OPTIONS`, `_JAVA_OPTIONS`, `JDK_JAVA_OPTIONS`,
`SPRING_APPLICATION_JSON`, `SPRING_CONFIG_LOCATION`,
`SPRING_CONFIG_ADDITIONAL_LOCATION`, `SPRING_PROFILES_ACTIVE`, `SERVER_ADDRESS`,
and `SERVER_PORT`. The private fake verifies these variables are absent without
requiring Java.

The fake launch seam, lock override, bind-address rows, and deterministic
candidate sequence exist only behind a friend `Xc2BackendManagerTestAccess` in
the test binary. The production path always uses exact IPv4
`QHostAddress::LocalHost`; the private table rejects `Any`, `AnyIPv4`, `AnyIPv6`,
`LocalHostIPv6`, `127.0.0.2`, and non-loopback addresses before locking or
spawning. Internally, probing may construct only the canonical encoded bytes
`http://127.0.0.1:<selected>/xc2/1.0` and pass
`QUrl(...).toEncoded(QUrl::FullyEncoded)` to Task 5's encoded-base
`setBaseUrl(const QByteArray &)`. Public `endpoints()` remains empty until the
current attempt, listener PID, health, and user have all passed; only then expose
`http://127.0.0.1:<selected>/xc2/1.0` and
`ws://127.0.0.1:<selected>/xc2-websocket`.

- [ ] **Step 3: Write failing PID ownership, collision, and lock tests**

Use real child processes, `GetExtendedTcpTable`, and bounded
`QSignalSpy`/`QTRY_COMPARE_WITH_TIMEOUT` checks for:

```cpp
void readyRequiresExactIpv4ListenerOwnedByStableChildHandle();
void ownerTableUsesNetworkByteOrderAndRejectsDuplicateRows();
void validAliveDecoyOn8082IsIgnoredAndSurvives();
void injectedCandidate8082IsRejectedBeforeArgumentsOrSpawn();
void portStealRaceCleansChildAndRetriesANewPort();
void threePortCollisionsExhaustWithoutResidue();
void shutdownConnectionProvesServerFourTupleBeforeWriting();
void listenerHandoffWritesZeroBytesToDecoy();
void ignoreShutdownIsTerminatedThenKilledWithoutResidue();
void perUserLockRejectsSecondManagerUntilChildFinished();
void livePerUserLockOlderThanThirtySecondsIsNeverRemoved();
void stalePerUserLockIsRecovered();
void processErrorRevokesNetworkButRetainsCleanupOwnership();
```

Start a fake sidecar decoy on `127.0.0.1:8082` that returns a completely valid
`alive`. Start the manager and prove its selected port is not 8082, the Windows
listener owner PID equals `ownedProcessId()`, that PID and executable belong to
the manager's direct fake `QProcess`, and the decoy remains alive after the owned
child stops with zero requests in its event log. A health response alone is not
proof of ownership. Separately inject candidate sequence `8082, 49123` while
8082 is free and prove 8082 is skipped without calling its argv callback or
spawning; an occupied decoy alone cannot prove the explicit rejection branch.

For the port-steal race, the private candidate sequence and fake ready handshake
put a separate process on the first selected candidate before the managed child
can bind. The valid response is therefore from the wrong PID. The manager must
never emit `ready`, must asynchronously terminate/kill and reap its failed
child, then select a different port and become Ready only for the new
child/HANDLE/PID. Give every attempt a distinct `attemptId` and fresh Task 5
client/cookie jar. Do not start attempt N+1 until attempt N is NotRunning and
its REST client is destroyed. Bound collision retries to exactly three; steal
all three candidates and assert one final failure, no Ready, every stealer and
managed-attempt process HANDLE signaled, and no orphan. Retry only a proven
listener collision, not Java/config/argv/job-object failure.

Production uses one fixed absolute path
`QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
+ "/ktm-xc2-vci2k.lock"`, independent of install root, selected port, and run.
Fail closed if the location is empty or its parent cannot be created. Construct
the long-lived `QLockFile`, call `setStaleLockTime(0)`, then `tryLock(0)` before
selecting a port or spawning. Never automatically call `removeStaleLockFile()`
after a live-owner failure. The private friend seam may supply a temporary
absolute lock path but production callers cannot.

Give two managers the same private test path. The first obtains it before
spawning; the second reports busy without spawning and returns through the
closed `Failed -> Stopped` path. Keep the lock through verified shutdown,
terminate, kill, collision cleanup, and manager-to-reaper transfer; release only
after the actual child emitted `finished`, its stable process HANDLE is signaled,
and `QProcess::state() == NotRunning`. Hold a live helper lock for more than the
default 30 seconds and prove it is never removed. For stale recovery, wait for
the fake's flushed `LOCKED` event, kill and wait for the helper HANDLE, then
prove `tryLock()` safely removes the dead-PID stale file; never remove a live
owner's lock.

At `QProcess::started`, open and retain a Windows process HANDLE with the rights
needed for `SYNCHRONIZE`, limited query, job assignment, and termination. Holding
that HANDLE until finished prevents PID reuse from authorizing a later process.
Every network proof requires the HANDLE to be unsignaled and the captured PID to
equal the current attempt. Query `GetExtendedTcpTable` with `AF_INET` and
`TCP_TABLE_OWNER_PID_LISTENER`, retry its size query if the table grows, and
match exactly one row. Convert `dwLocalPort` with `ntohs`, `dwLocalAddr` with
`ntohl`, require `127.0.0.1`, and reject zero, wildcard, `127.0.0.2`, byte-swapped,
duplicate, or conflicting rows. Include `<winsock2.h>` before Windows/IP Helper
headers under MinGW and link `Iphlpapi` and `Ws2_32` explicitly.

The manager does not call Task 5's `requestShutdown()`. It creates a dedicated
`QTcpSocket`, sets `QNetworkProxy::NoProxy`, and connects only to literal
`127.0.0.1:selected`. After `connected`, and before writing any byte, repeatedly
query `GetExtendedTcpTable(AF_INET, TCP_TABLE_OWNER_PID_ALL)` within the one
shutdown deadline. Match exactly one `ESTABLISHED` server row whose local tuple
is `127.0.0.1:selected`, remote tuple is
`127.0.0.1:<socket.localPort()>`, owning PID is the captured PID, and stable
HANDLE is still live. Convert both row addresses with `ntohl` and both row ports
with `ntohs`; zero, wildcard, byte-swapped, reversed, duplicate, or conflicting
rows fail the proof. Only then serialize with exact CRLF delimiters and write
once on that same socket:

```text
POST /xc2/1.0/serviceStatus/shutdown HTTP/1.1
Host: 127.0.0.1:<selected>
Content-Type: application/x-www-form-urlencoded
Content-Length: 0
Connection: close

```

An optional Cookie header may contain only the current attempt's exact
shutdown-REST Task 5 cookie export. Resolve it once from the current attempt
before connecting by passing the manager-generated canonical
`http://127.0.0.1:<selected>/xc2/1.0/serviceStatus/shutdown` target's
`QUrl::toEncoded(QUrl::FullyEncoded)` bytes to
`shutdownCookieHeaderFor(const QByteArray &)`: a failed export writes zero
bytes, an empty success omits the header, and a non-empty success adds exactly
one `Cookie:` header. The WebSocket-only `cookieHeaderFor()` is never used for
shutdown. Cookie bytes are never logged. A proof failure, socket error, timeout,
duplicate row, closed listener, signaled HANDLE, or handoff writes zero bytes
and falls directly to terminate then kill of the owned HANDLE/QProcess. A
partial or failed write is never retried. The fake event logs prove the normal
path receives one exact empty-form POST and every decoy/handoff path receives
zero bytes.

- [ ] **Step 4: Write failing startup, lifecycle, and output tests**

Add:

```cpp
void healthPollingIsSingleFlightWithinOneTotalDeadline();
void startupTimeoutAbortsReplyAndLateAliveCannotReviveRun();
void currentUserAndPermissionsAreValidatedBeforeReady();
void emptyPermissionsRemainReadyButAuthorizeNothing();
void earlyCrashReportsBackendErrorAndLeavesNoOwnedRun();
void stopDuringStartNeverEmitsReady();
void repeatedStartIsRejectedWithoutDisturbingCurrentRun();
void stopIsIdempotentAndRestartWaitsForStopped();
void destroyRunningManagerReapsWithoutUiThreadWait();
void coordinatedQuitReapsChildBeforeUnlockAndExit();
void forcedApplicationExitJobObjectContainsChildTree();
void stdoutAndStderrUseIndependentFragmentBuffers();
void outputLinesAndRingsAreBounded();
void outputControlsAndSensitiveFieldsAreRemoved();
```

At most one health request may be pending. A single startup wall-clock deadline
covers fresh production inspection, process start, all health attempts, listener
ownership verification, and the strict `currentUser` request. Each run owns an
immutable `runId`; each collision attempt owns an immutable `attemptId`. Timeout
or stop first sets `mayBecomeReady=false`, revokes `networkAuthorized`, removes
the expected pending request ID, and cancels every poll/deadline timer. Only
after those mutations may it call Task 5 `abort(id)`, because abort completion
may re-enter synchronously. The immutable run/attempt contexts remain valid for
`errorOccurred`, output draining, `finished`, HANDLE close, lock release, and
`Stopped`; canceling readiness must never suppress cleanup callbacks.

Every Task 5 result matches the emitting client pointer, runId, attemptId,
request ID, state, and `mayBecomeReady` before acting. Health polling schedules
the next request only after the prior terminal signal. A stopped/timed-out first
attempt may deliver canceled or scripted late success after a new attempt exists;
it cannot poll, request current user, publish endpoints, emit Ready, or release
the new attempt's resources.

Before Ready, parse `currentUser`, require non-blank `loginName` and `name`, and
retain the returned permissions exactly, including order, spelling, duplicates,
unknown values, and an empty list. Empty permissions still allow backend Ready
but authorize no operation; later controllers require their own exact
case-sensitive permissions. Missing/malformed/blank identity fails startup.
Neither Ready nor a non-empty permissions list is described as DealerNet
authentication, and `currentUser()`/public endpoints are cleared as soon as stop
or failure begins.

`Xc2ProcessOutput` is a pure, non-QObject, non-spawning accumulator with wholly
independent stdout/stderr state: incremental decoder/sanitizer state, bounded
sanitized pending-line bytes, oversized-line discard flags, and rings. Tests
feed exact chunk boundaries directly. The control/ANSI filter and sensitive
field recognizer run incrementally before bytes enter the pending line or ring;
never retain a whole unsanitized process chunk. Emit only complete logical lines
and one final partial line at EOF, handle CRLF split across feeds, and never
append unbounded `readAll()` data before scanning. Cap each pending prefix so the
stored/emitted UTF-8 logical line is at most 16 KiB including one `[truncated]`
suffix, then discard the remainder through its delimiter. Retain at most 256
sanitized lines per stream and finalize both streams before `stopped`.

Incrementally decode bounded bytes, strip ANSI CSI/OSC sequences and Unicode
C0/C1 controls other than tab, then recognize and redact fields with bounded
look-behind. Once a sensitive name is recognized, discard its value bytes as
they arrive and retain only the replacement marker. Data-drive all nine names
`Authorization`, `Proxy-Authorization`, `Cookie`, `Set-Cookie`, `password`,
`token`, `sessionIndex`, `SAMLRequest`, and `SAMLResponse`, case-insensitively in
header, `key=value`, and JSON-string forms, for both streams. Split both field
name and value across feeds and insert ANSI inside names. Assert no secret occurs
in `outputLine`, `recentOutput()`, pending storage, truncated lines, or EOF
partials, and assert both pending byte buffers plus rings stay within their
documented bounds.

`startProduction()` is single-flight: only `Stopped` accepts it. A second start
in `Starting`, `Probing`, `Ready`, `Stopping`, or transient `Failed` is rejected
through a separate start-rejection result without mutating the current run,
spawning, or emitting that run's `failed`. Invalid input, busy lock, inspection
failure, early crash, collision exhaustion, and shutdown failure enter `Failed`
only while cleanup is outstanding, emit one run failure, and always transition
to `Stopped` after NotRunning/HANDLE signal and lock release. `stop()` is
idempotent in every state and never issues a second shutdown write. Every
accepted start clears both sanitized output rings and their pending partials;
while a run is active `recentOutput()` exposes only that run, and after cleanup
reaches `Stopped` it retains the just-finished run until the next accepted
start. If an accepted run never starts a child, its single terminal signal is
`stopped(-1, QProcess::CrashExit)` after cleanup and lock release.

Deleting a running manager transfers the complete process/HANDLE/job/lock
context to an application-lifetime reaper without waiting or running a nested
event loop. While it owns a live child, that reaper installs one application
`QEvent::Quit` filter. The first and repeated Quit events are consumed
idempotently, trigger the same bounded asynchronous terminate-then-kill path,
and leave the helper process and `QLockFile` alive. Only after `QProcess` is
NotRunning, the stable HANDLE is signaled, and output is finalized may the
reaper release the lock and repost exactly one Quit event. The exit helper uses
its test-binary-local friend to start the real manager with the private fake,
publishes PID/lock path, destroys the manager to transfer the live ownership
context to the real reaper, and then requests quit; it never copies manager
ownership logic. The parent proves helper and lock remain live until the child
HANDLE signals, then observes unlock and helper exit.

Separately, every successfully started child is assigned before network
authorization to a Windows Job Object configured with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`; job creation/configuration/assignment
failure is fatal. A forced `QCoreApplication::exit`, process crash, or OS
termination may prevent further Qt cleanup, so only Job Object child-tree
containment is promised there. No claim is made that a dead owner process's
`QLockFile` remains unrecoverable until the asynchronously terminated child
HANDLE signals; crash leftovers are dead-owner stale locks. Strict lock
retention across forced owner death would require an external guardian process
and is outside this scope.

- [ ] **Step 5: Run the manager target to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2BackendManager --parallel
```

Expected: missing manager compile failure.

- [ ] **Step 6: Implement the provenance-safe manager interface**

```cpp
namespace ktm::xc2 {

enum class Xc2BackendState {
    Stopped, Starting, Probing, Ready, Stopping, Failed
};

struct Xc2BackendEndpoints {
    QUrl restBaseUrl;
    QUrl webSocketUrl;
};

class Xc2BackendManagerTestAccess;

class Xc2BackendManager final : public QObject {
    Q_OBJECT
public:
    explicit Xc2BackendManager(QObject *parent = nullptr);
    ~Xc2BackendManager() override;
    bool startProduction(const QString &installRoot,
                         Xc2Error *error = nullptr);
    void stop();
    Xc2BackendState state() const;
    bool ownsProcess() const;
    qint64 ownedProcessId() const;
    Xc2BackendEndpoints endpoints() const;
    std::optional<Xc2CurrentUser> currentUser() const;
    QStringList recentOutput(bool standardError) const;

signals:
    void stateChanged(Xc2BackendState);
    void ready(const Xc2BackendEndpoints &);
    void outputLine(bool standardError, const QString &line);
    void failed(const Xc2Error &);
    void stopped(int exitCode, QProcess::ExitStatus);

private:
    friend class Xc2BackendManagerTestAccess;
    struct PrivateLaunchPlan;
    struct RunContext;
    bool startPrivateForTest(PrivateLaunchPlan plan,
                             Xc2Error *error = nullptr);
};

} // namespace ktm::xc2

Q_DECLARE_METATYPE(ktm::xc2::Xc2BackendState)
Q_DECLARE_METATYPE(ktm::xc2::Xc2BackendEndpoints)
```

`PrivateLaunchPlan` and the pure production argv builder are private. Only the
test-binary-local fully-qualified friend class defined independently in
`test_Xc2BackendManager.cpp` and `xc2_manager_exit_helper.cpp` can construct a
fake program, bind-address row, lock override, timeout, environment observer,
or candidate sequence. The two definitions live in separate test executables;
the helper uses that seam to exercise the real manager and never reimplements
launch, Job, lock, or reaping logic. `startProduction()` validates non-empty
absolute input, performs fresh production inspection, and immediately copies
only that inspection's verified paths into the private run; it never accepts
Task 4 DTOs.
Validate positive bounded startup/shutdown timeouts, `maxPortAttempts` in
`1..3`, an absolute existing working directory, direct executable/JAR paths,
absolute test lock paths, exact IPv4 bind address, non-empty callback, and
candidate ports before acquiring the lock. Clear a supplied error on every
accepted start. Register both public metatypes before queued signals/QSignalSpy.

- [ ] **Step 7: Implement run-scoped ownership and Windows listener proof**

`RunContext` owns immutable `runId`, one fixed `QLockFile`, failure/stopped
completion flags, public-state snapshots, and the active attempt. Each attempt
owns immutable `attemptId`, direct `QProcess`, fresh `Xc2RestClient`, selected
port, captured PID and stable HANDLE, Job Object, startup/shutdown/poll timers,
pending request IDs, dedicated shutdown socket, output accumulator, and the
separate booleans `mayBecomeReady`, `networkAuthorized`,
`ownsChildForCleanup`, and `cleanupCompleted`. A process error immediately
revokes only `networkAuthorized`/`mayBecomeReady`; `ownsChildForCleanup` and the
run lock remain true until NotRunning and the HANDLE is signaled.

`startProduction()` is accepted only in `Stopped`. Perform fresh inspection and
all private option validation, derive the fixed production lock path, create its
parent, set stale time zero, and acquire it before candidate allocation. The
production allocator briefly binds `QTcpServer` to exact `127.0.0.1:0`, rejects
8082, closes the reservation, then builds argv and starts the direct process.
The unavoidable close-to-child-bind TOCTOU is handled only by the bounded,
PID-proven collision path. The friend allocator supplies exact candidates for
tests. Connect process and REST signals with both QObject receiver context and
captured runId/attemptId so manager destruction disconnects callbacks while the
transferred reaper context continues cleanup independently.

After `started`, capture the stable Windows HANDLE, prove the executable is the
direct configured child, create/configure/assign the kill-on-close Job Object,
then begin timer-driven health probing. Configure Task 5 using the internally
generated canonical encoded base bytes, never a caller `QUrl`. A successful
health response is followed by an exact listener-owner proof before issuing
current user; a successful user is followed by a second exact listener/HANDLE
proof before setting `networkAuthorized`, publishing endpoints/user, and
emitting Ready. Any missing, duplicate, wildcard, wrong-port, wrong-address, or
wrong-PID row prevents Ready. Collision cleanup waits for the old child and
destroys its client before advancing to the next attempt under the same lock.

`stop()` first makes Ready impossible, clears public endpoints/user, cancels
startup timers, and detaches pending request IDs before aborting Task 5 replies.
If the run had network authorization it attempts the dedicated, same-socket
four-tuple-proven shutdown exactly once; otherwise it writes no network bytes.
Whether proof, connect, write, or response succeeds or fails, cleanup advances
without blocking through shutdown deadline, terminate deadline, and kill. Child
`finished` drains/finalizes both output streams; cleanup verifies that the
stable HANDLE is signaled, then closes the process/job handles in normal order,
releases the lock, emits one `stopped`, and transitions to `Stopped`. Old
callbacks may finish their own context but cannot observe or mutate a later
run.

The destructor never calls `waitForStarted`, `waitForFinished`, `processEvents`,
or a nested event loop. It disconnects manager-facing callbacks and transfers
the entire attempt plus lock to the application-lifetime reaper, which performs
the same bounded asynchronous terminate/kill/NotRunning sequence. During
coordinated normal Quit, the reaper's application event filter consumes first
and repeated Quit events idempotently, keeps the helper and lock alive until
NotRunning, stable-HANDLE signal, and output finalization, then releases the
lock and reposts exactly one Quit. The helper's independently declared
`Xc2BackendManagerTestAccess` friend starts the real manager through its private
fake seam, publishes the proof data, destroys the manager to exercise the real
reaper transfer, and then requests quit; it contains no copied launch, Job,
lock, or cleanup logic. Forced application exit, owner crash, and OS termination
rely only on `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` child-tree containment. They
make no strict post-owner lock-ordering promise; an external guardian would be
required and is outside scope.

- [ ] **Step 8: Run manager and all KTM tests**

```powershell
rtk cmake --build build-test --target test_Xc2BackendManager --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: Ready is gated by the direct child's stable HANDLE, exact selected
IPv4 listener PID, strict health, and non-blank current user; empty permissions
are retained without authorizing operations. Fresh production provenance,
unique dynamic-port overrides, sanitized environment, injected candidate
collisions, long-lived/stale locks, run/attempt stale-callback suppression,
same-socket shutdown proof, output bounds/redaction, and normal destruction all
pass with every captured child HANDLE signaled. Coordinated Quit proves the
child signal precedes unlock and helper exit; forced application exit proves
Job Object containment only. The valid 8082 decoy, handoff decoy, and port-steal
traps remain alive until their test-owned cleanup and record zero unauthorized
shutdown bytes.

- [ ] **Step 9: Commit the owned manager**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2BackendManager.* src/ktm/xc2/internal/Xc2ProcessOutput.* tests/ktm/fake_xc2_sidecar.cpp tests/ktm/xc2_manager_exit_helper.cpp tests/ktm/test_Xc2BackendManager.cpp
rtk git commit -m "feat: manage the XC2 sidecar lifecycle"
```

---

## Task 7: STOMP WebSocket Client and Topic Allowlist

**Files:**
- Create: `src/ktm/xc2/Xc2StompClient.h`
- Create: `src/ktm/xc2/Xc2StompClient.cpp`
- Create: `tests/ktm/FakeXc2TransportServer.h`
- Create: `tests/ktm/FakeXc2TransportServer.cpp`
- Create: `tests/ktm/test_Xc2StompClient.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: STOMP codec from Task 3, the authority-bound REST session from
  Task 5, and the topic allowlist from Task 1.
- Produces: `Xc2StompState`, `Xc2StompSession`, `Xc2StompMessage`, bounded
  connection generations, sent-subscription events, errors, and visibility-loss
  events. It never reconnects itself.

**Observed compatibility and intentional boundaries:**

- The actively served application and patched JAR both use the native endpoint
  `/xc2-websocket`; the JAR registers a `/topic` simple broker and no SockJS
  transport. Do not append a SockJS session or `/websocket` suffix.
- A direct local probe of the approved patched sidecar required WebSocket
  subprotocol `v12.stomp` and returned STOMP `version:1.2`. The actively served
  application bundles an older STOMP library that advertises v10/v11; live
  runtime evidence wins, so do not copy that obsolete offer.
- The observed `CONNECTED` frame has `heart-beat:0,0`. That is a successful
  negotiation with both heartbeat directions disabled, not a timeout condition.
- The eight Task 1 topics match the in-scope status, vehicle, ECU, job, flow, and
  measurement topics. `/topic/dealernet` and `/topic/semantic` also exist in the
  legacy application but remain deliberately outside the approved product scope.
- Subscription IDs are client-scoped opaque values. Stable semantic IDs need not
  reproduce the legacy client's dynamic `sub-N` values; the golden progress
  fixture already freezes `progress-subscription`.
- Task 3 already proves STOMP byte-stream fragmentation/coalescing, CRLF,
  content-length/NUL handling, duplicate-header precedence, and the 8 MiB codec
  cap. This task proves the WebSocket/message/generation integration without
  duplicating those parser tests.

- [ ] **Step 1: Build deterministic loopback WebSocket test servers**

Create `FakeXc2TransportServer` with one `QTcpServer` bound only to
`QHostAddress::LocalHost` on port zero. The same listener authority must serve
the minimal strict REST route used to establish the Task 5 cookie and the
`/xc2-websocket` upgrade; two independently bound fake ports cannot prove the
cookie-authority invariant. Peek, but do not consume, bytes through the complete
HTTP header. Consume and answer a REST request, including a synthetic
`Set-Cookie`, or pass an untouched upgrade socket to
`QWebSocketServer::handleConnection()`. Configure that QWebSocketServer with
`QNetworkProxy::NoProxy` and
`setSupportedSubprotocols({QStringLiteral("v12.stomp")})`.

Retain each accepted WebSocket and expose its `requestUrl()`, negotiated
subprotocol, upgrade request headers, text/binary message kind, and incrementally
decoded STOMP frames. Script CONNECTED, MESSAGE, RECEIPT, ERROR, heartbeat,
fragmented/coalesced payloads, delayed close, and no-response behavior. Before
handoff, the same TCP listener can instead accept without an upgrade response or
return a `302` to an independent loopback trap listener. Neither listener may
bind wildcard or non-loopback. Tests use signals and bounded `QSignalSpy` waits,
never sleep-based guesses.

Use `QTEST_GUILESS_MAIN`, not `QTEST_APPLESS_MAIN`, so `QCoreApplication`,
timers, sockets, and the event dispatcher exist. Add `Q_DECLARE_METATYPE` for
the public state/session/message types used by `QSignalSpy`.

- [ ] **Step 2: Write failing authority, handshake, protocol, and state tests**

Add these cases first:

```cpp
void derivesExactUrlAndCookieFromRestClient();
void cannotRetargetCookieToAnotherAuthority();
void ownedSocketBypassesApplicationProxy();
void handshakeRedirectNeverReachesTrapOrLeaksCookie();
void offersAndRequiresOnlyV12StompSubprotocol();
void sendsTextConnectWithExactRequiredHeaders();
void subscribesOnlyAfterValidConnected();
void rejectsInvalidConnectedFrames_data();
void rejectsInvalidConnectedFrames();
void errorFrameTerminatesGenerationExactlyOnce();
void totalDeadlineCoversUpgradeAndStompNegotiation_data();
void totalDeadlineCoversUpgradeAndStompNegotiation();
void connectIsSingleFlightAndStaleGenerationSignalsAreIgnored();
```

The Task 5 integration test first obtains a synthetic session cookie from the
loopback HTTP fake. Name that configured instance `rest`. The STOMP client must
open exactly `rest.webSocketUrl()` and send exactly the successful
`rest.cookieHeaderFor(rest.webSocketUrl().toEncoded(QUrl::FullyEncoded))`
result. There is no public API that accepts an arbitrary WebSocket URL and
Cookie pair. Temporarily install an application proxy whose trap listener counts
connections and prove the WebSocket still connects directly. A raw origin
returning `302` to a second loopback port must fail without contacting the trap
or replaying Cookie.

The positive server selects `v12.stomp`. Negative rows select no subprotocol or
a different subprotocol and assert that no STOMP CONNECT is sent. A successful
upgrade must receive one text WebSocket message containing CONNECT with exact
headers:

```text
accept-version:1.2
host:<canonical validated URL host, without port>
heart-beat:10000,10000
```

The client remains in STOMP negotiation and sends no SUBSCRIBE until one valid
CONNECTED frame has `version:1.2`. Reject missing/downgraded/duplicate version,
CONNECTED in the wrong state, MESSAGE or RECEIPT before CONNECTED, malformed
heartbeat values, and unknown server commands. An ERROR before or after
CONNECTED preserves its `message` header and body in one `Xc2Error`, terminates
that generation, and cannot double-complete when the server then closes.

Use short injected deadlines for three total-deadline rows: TCP accepted but no
upgrade response, WebSocket upgraded but no CONNECTED, and periodic partial
STOMP bytes that never complete CONNECTED. All must terminate once near the one
wall-clock deadline measured from `connectToBackend`; activity never extends it.
Call connect twice while the first call is in flight and prove one server
connection. Then abort generation A, connect generation B, and make A deliver a
queued error/close/timer callback; B must remain unaffected.

- [ ] **Step 3: Write failing heartbeat, subscription, routing, fragmentation,
  disconnect, and visibility tests**

Add these cases:

```cpp
void negotiatesHeartbeatMatrix_data();
void negotiatesHeartbeatMatrix();
void zeroZeroHeartbeatNeverTimesOut();
void incomingActivityUsesMonotonicDeadline();
void outgoingHeartbeatIsSentOnlyAfterOutboundSilence();
void websocketAndStompFragmentationDeliverExactlyOnce_data();
void websocketAndStompFragmentationDeliverExactlyOnce();
void codecBufferDoesNotCrossConnectionGeneration();
void stableSubscriptionsQueueDeduplicateAndKeepOrder();
void unsubscribeUsesTheStableIdExactlyOnce();
void rejectsInvalidTopicValueWithoutWriting();
void messageRoutingRequiresConsistentHeaders_data();
void messageRoutingRequiresConsistentHeaders();
void matchingDisconnectReceiptClosesGracefully();
void wrongOrMissingDisconnectReceiptUsesBoundedAbort();
void disconnectReceiptCloseAndTimeoutRaceCompletesOnce();
void abortCurrentGenerationIsImmediateAndIdempotent();
void unexpectedEstablishedLossEmitsVisibilityOnceAndNeverReconnects();
void connectFailureAndIntentionalDisconnectDoNotLoseVisibility();
```

Heartbeat data rows cover server `0,0`, `0,N`, `N,0`, and `N,M`, plus a missing
header and negative, non-numeric, incomplete, and overflow values. The observed
`0,0` row must remain connected past the injected grace window with no heartbeat
or visibility-loss event. Incoming heartbeat, partial STOMP, and ordinary
MESSAGE traffic each reset the monotonic incoming-activity deadline. A real
outgoing frame resets outbound activity so an LF is not sent early.

Fragmentation rows cover one STOMP frame split across several WebSocket
messages, several STOMP frames coalesced into one WebSocket message, one
WebSocket message fragmented into frames with `setOutgoingFrameSize()`, and
both text and binary WebSocket messages. Each logical STOMP frame is decoded
once. A partial frame left by generation A cannot complete from bytes received
on generation B.

Freeze the subscription map and ordering:

| Topic | Subscription ID |
|---|---|
| `VciStatus` | `vci-status-subscription` |
| `VehicleInfo` | `vehicle-info-subscription` |
| `Ecu` | `ecu-subscription` |
| `Progress` | `progress-subscription` |
| `MeasurementValues` | `measurement-values-subscription` |
| `FlowGui` | `flow-gui-subscription` |
| `FlowProgress` | `flow-progress-subscription` |
| `Login` | `login-subscription` |

When several topics are desired, emit SUBSCRIBE frames in
`Xc2ContractProfile::allTopics()` order. Each has exact `id`, `destination`, and
`ack:auto` headers. Duplicate subscribe is idempotent. Unsubscribe before
CONNECTED removes only the queued desire; after CONNECTED it sends one
UNSUBSCRIBE containing the same `id`. A later controller-requested connection
generation resubscribes the still-desired topics once, but the transport never
opens that generation itself.

MESSAGE rows require non-empty `destination`, `subscription`, and `message-id`.
The subscription must be active in the current generation and its Topic must
map to exactly that destination in Task 1. Cover missing headers, unknown
subscription, allowed destination paired with another subscription, known
subscription paired with another or out-of-profile destination, and a message
after unsubscribe. Each invalid row reports a Contract error and drops the
body without `messageReceived`; the golden progress MESSAGE is delivered once
with its message ID preserved.

Graceful disconnect sends one receipt-bearing DISCONNECT, waits only for the
matching receipt-id, and closes normally. A wrong/stale receipt cannot complete
it. No receipt and a receipt/close/timeout collision both finish once within the
injected disconnect deadline; timeout force-aborts. An unexpected loss after
CONNECTED emits one visibility-loss event and never opens another connection.
A connection that never reached CONNECTED and an intentional disconnect have no
established visibility to lose.

- [ ] **Step 4: Run the client target to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2StompClient --parallel
```

Expected: missing-client compile failure.

- [ ] **Step 5: Implement the bounded public interface**

```cpp
using Xc2StompGeneration = quint64;

enum class Xc2StompState {
    Disconnected,
    WebSocketConnecting,
    StompConnecting,
    Connected,
    Disconnecting,
    Failed
};

struct Xc2StompClientOptions {
    int connectDeadlineMs = 10000;
    int disconnectDeadlineMs = 2000;
    qint64 clientOutgoingHeartbeatMs = 10000;
    qint64 clientIncomingHeartbeatMs = 10000;
    int heartbeatGraceMultiplier = 2;
    quint64 maximumIncomingMessageBytes = 8 * 1024 * 1024;
};

struct Xc2StompSession {
    Xc2StompGeneration generation = 0;
    QString version;
    qint64 outgoingHeartbeatMs = 0;
    qint64 incomingHeartbeatMs = 0;
};

struct Xc2StompMessage {
    Xc2StompGeneration generation = 0;
    Topic topic = Topic::VciStatus;
    QString destination;
    QString subscriptionId;
    QString messageId;
    QByteArray body;
};

class Xc2StompClient final : public QObject {
    Q_OBJECT
public:
    explicit Xc2StompClient(Xc2StompClientOptions options = {},
                            QObject *parent = nullptr);
    bool connectToBackend(const Xc2RestClient &rest,
                          Xc2Error *error = nullptr);
    bool subscribe(Topic topic, Xc2Error *error = nullptr);
    bool unsubscribe(Topic topic, Xc2Error *error = nullptr);
    void disconnectFromBackend();
    void abortCurrentGeneration();
    Xc2StompState state() const;
    Xc2StompGeneration generation() const;

signals:
    void stateChanged(Xc2StompGeneration, Xc2StompState);
    void connected(const Xc2StompSession &);
    void subscriptionSent(Topic, const QString &subscriptionId);
    void unsubscriptionSent(Topic, const QString &subscriptionId);
    void messageReceived(const Xc2StompMessage &);
    void errorOccurred(Xc2StompGeneration, const Xc2Error &);
    void visibilityLost(Xc2StompGeneration, const Xc2Error &);
    void disconnected(Xc2StompGeneration);
};
```

- [ ] **Step 6: Implement authority-bound handshake and one connection generation**

`connectToBackend` accepts only a configured `Xc2RestClient` named `rest`. In
the same call, copy its canonical `rest.webSocketUrl()` and request
`rest.cookieHeaderFor(rest.webSocketUrl().toEncoded(QUrl::FullyEncoded))`; a
failed cookie result fails synchronously without creating a socket. There is no
overload accepting caller-provided URL or Cookie bytes. This preserves the Task
5 guarantee that REST and WebSocket use the same validated host and explicit
port while satisfying its byte-for-byte encoded-URL export gate.

Create a fresh `QWebSocket` for each accepted call. Set its proxy to
`QNetworkProxy::NoProxy` before opening it. Build `QNetworkRequest` from only the
canonical URL, set `QNetworkRequest::ManualRedirectPolicy`, set maximum redirects
to zero, and add the derived Cookie header. Open it with
`QWebSocketHandshakeOptions` containing only `v12.stomp`. After the upgrade,
require `subprotocol()` to equal `v12.stomp` before changing to
`StompConnecting` or sending CONNECT. Configure the WebSocket incoming message
and frame limits before open.

Start one total connect timer at method entry. It is not an inactivity timer and
no network or STOMP activity restarts it. Reject another connect while state is
WebSocketConnecting, StompConnecting, Connected, or Disconnecting, with no
second socket. A call from Disconnected or a fully cleaned Failed state begins a
new monotonically increasing generation.

Every socket callback and timer captures both the generation and socket
identity. Ignore callbacks that do not match the current pair. Reset the codec,
active subscriptions, heartbeat clocks, and all timers at generation start and
terminal cleanup. Route timeout, WebSocket error, disconnected, STOMP ERROR, and
protocol failure through one `finishOnce(generation, ...)` path so each
generation emits at most one terminal error, visibility-loss event, and
disconnected event. `abortCurrentGeneration()` immediately invalidates and
force-aborts the current socket, is idempotent, and completes with
`Transport/Canceled`; it gives an owner such as the contract probe a hard stop
when its outer REST-plus-WebSocket deadline expires.

- [ ] **Step 7: Implement strict STOMP negotiation and frame dispatch**

Encode all client frames with `Xc2StompCodec` and send CONNECT, SUBSCRIBE,
UNSUBSCRIBE, DISCONNECT, and heartbeat LF as text WebSocket messages. CONNECT
uses only `accept-version:1.2`, the canonical URL host in `host`, and the two
configured heartbeat offer values. The production defaults therefore remain
the observed `10000,10000` offer.

Only a single CONNECTED while StompConnecting can establish the session. Its
required `version` must equal `1.2`; parse an absent `heart-beat` as `0,0` and
otherwise require exactly two checked non-negative decimal 64-bit values.
Reject overflow before converting a duration or multiplying the grace factor.
Stop the total connect deadline only after the valid CONNECTED has been fully
processed. Then emit connected and flush desired subscriptions.

Recognize MESSAGE, RECEIPT, and ERROR after negotiation. ERROR is terminal and
preserves its header/body evidence. A decoder error, duplicate or misplaced
CONNECTED, or unknown server command is a terminal Contract failure. An
unrouteable MESSAGE and an unrelated RECEIPT are reported and dropped without
being treated as a valid application event.

Feed only complete `textMessageReceived` and `binaryMessageReceived` payloads
to the incremental codec. Use text/binary frame signals only to update incoming
activity; never feed both frame and message signals, which would decode bytes
twice. Complete-message delivery lets QWebSocket reassemble TCP and WebSocket
frame fragmentation, while the codec independently handles STOMP frames split
across WebSocket messages or coalesced in one message.

- [ ] **Step 8: Implement heartbeat, stable subscriptions, and strict routing**

For CONNECT `heart-beat:<cx>,<cy>` and CONNECTED
`heart-beat:<sx>,<sy>`, calculate:

```text
outgoing = disabled if cx == 0 or sy == 0, otherwise max(cx, sy)
incoming = disabled if sx == 0 or cy == 0, otherwise max(sx, cy)
```

Never create a timer for a disabled direction. Use `QElapsedTimer` for incoming
and outgoing activity. Any incoming data frame, including a heartbeat or a
partial STOMP payload, refreshes incoming activity. Any successfully queued
client STOMP frame or LF refreshes outgoing activity. On the outgoing timer,
send LF only after a full negotiated interval of silence. On an incoming timer
firing before the checked grace threshold, reschedule the remaining duration;
only silence exceeding `heartbeatGraceMultiplier * incoming` aborts the socket
and reports visibility loss once.

Keep a persistent desired-topic set and a per-generation active-subscription
map using the frozen table from Step 3. Validate every Topic against
`Xc2ContractProfile::allTopics()`; an invalid enum value returns false without a
write. Queue valid desires before CONNECTED, flush them once in profile order,
and make duplicate subscribe/unsubscribe idempotent. `subscriptionSent` and
`unsubscriptionSent` mean that the exact frame was queued, not that the server
confirmed it.

For MESSAGE, compare the raw decoded `destination` and `subscription` values to
the active map before converting them for the public struct. Require and retain
the non-empty `message-id`. Only a consistent current-generation tuple produces
`messageReceived`; report and drop every mismatch or missing field. Never expose
an arbitrary string subscription API.

- [ ] **Step 9: Implement receipt-bounded disconnect and visibility semantics**

From Connected, stop heartbeat timers, enter Disconnecting, and send exactly one
DISCONNECT with `receipt:disconnect-<generation>`. Keep the socket open until a
RECEIPT has exactly that `receipt-id`, then request a normal WebSocket close. A
single disconnect deadline remains authoritative through the close handshake;
on expiry call the socket's `abort()` and emit one `Transport/Timeout` error. A
matching receipt followed by a normal close emits no error. A server close,
error, matching receipt, and timeout in the same event-loop turn still complete
once. From WebSocketConnecting or StompConnecting, disconnect aborts without
sending a STOMP DISCONNECT. From Disconnected it is idempotent. The explicit
`abortCurrentGeneration()` path never waits for the graceful deadline.

`Xc2StompClient` has no destructive-job setter and never schedules or opens a
reconnection. Unexpected socket loss or heartbeat timeout after a valid
CONNECTED emits one generic visibilityLost event. Pre-session failures and
intentional disconnect do not. The later session controller consumes this event
and may explicitly start a fresh generation only when its destructive-job gate
allows it; Task 8 records degraded visibility without changing job state.

- [ ] **Step 10: Run only STOMP client and codec tests**

```powershell
rtk cmake --build build-test --target test_Xc2StompClient test_Xc2StompCodec --parallel
rtk ctest --test-dir build-test --output-on-failure -R "^xc2_(stomp_client|stomp_codec)$"
```

Expected: authority/cookie traps, v12 handshake, protocol/state/deadline races,
heartbeat matrix, fragmentation, stable routing, receipt-bounded disconnect,
and visibility semantics all pass with no external process.

- [ ] **Step 11: Commit the WebSocket client**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2StompClient.* tests/ktm/FakeXc2TransportServer.* tests/ktm/test_Xc2StompClient.cpp
rtk git commit -m "feat: add XC2 STOMP WebSocket transport"
```

---

## Task 8: Job Registry and Foundation Contract Probe

**Files:**
- Modify: `src/ktm/xc2/Xc2ContractProfile.h`
- Modify: `src/ktm/xc2/Xc2ContractProfile.cpp`
- Create: `src/ktm/xc2/Xc2JobRegistry.h`
- Create: `src/ktm/xc2/Xc2JobRegistry.cpp`
- Modify: `tests/ktm/test_Xc2ContractProfile.cpp`
- Create: `tests/ktm/test_Xc2JobRegistry.cpp`
- Create: `tests/ktm/xc2_contract_probe.cpp`
- Create: `tests/ktm/test_Xc2ContractProbe.cpp`
- Create: `tests/ktm/run_xc2_mock_contract.ps1`
- Modify: `tests/ktm/FakeXc2TransportServer.h`
- Modify: `tests/ktm/FakeXc2TransportServer.cpp`
- Create: `.github/workflows/ktm-xc2-foundation.yml`
- Modify: `CMakeLists.txt`
- Modify: `docs/superpowers/specs/2026-07-16-ktm-xc2-integration-design.md`
- Modify: `docs/superpowers/plans/2026-07-16-ktm-xc2-foundation-plan.md`

**Interfaces:**
- Consumes: the frozen profile from Task 1, strict progress model/codec from
  Task 2, authority-bound REST session from Task 5, and generation/message
  identity plus bounded abort/disconnect behavior from Task 7.
- Produces: `Xc2JobEvent`, `Xc2JobRecord`, deterministic `Xc2JobRegistry`, an
  explicitly scoped read-only live probe, its real-process fake integration
  suite, and three-platform completion evidence.

**Intentional boundaries:**

- The registry records backend facts. It does not infer that an event belongs to
  the only active job, enforce VCI ownership, or synthesize progress after a
  visibility gap. The later session controller owns concurrency gates.
- Probe success validates only the approved read-only foundation surface. It
  neither proves the selected service came from the approved JAR hash nor
  authorizes device, vehicle, ECU, flow, measurement, or flash operations.
- The probe never includes or instantiates `Xc2InstallationProbe`,
  `Xc2BackendManager`, `QProcess`, Java, or a JAR. CI starts only the probe child
  and the in-process loopback fake; it never starts an XC2 artifact.

- [x] **Step 1: Write failing job-registry tests**

Use `QSignalSpy` and data rows for all four terminal states. Add:

```cpp
void acceptedJobStartsCreated();
void duplicateAcceptIsIdempotentAndNeverResets();
void rejectsBlankAndRetiredJobIdsAtomically();
void progressTransitionsCreatedToInProgressToFinished();
void everyTerminalStateRejectsEveryLaterProgress();
void unknownJobIsCreatedFromProgressWithoutLosingEvent();
void progressNeverAttachesToAnotherActiveJob();
void duplicateDeliveryIsIdempotent();
void reusedDeliveryIdentityWithDifferentPayloadIsContractError();
void identicalPayloadWithDifferentIdentityRemainsOrdered();
void semanticValidationRejectsInvalidAndRegressiveProgress();
void terminalTransitionEmitsChangedThenTerminalExactlyOnce();
void activeJobsHaveStableFirstObservedOrder();
void repeatedVisibilityLossIsIdempotentAndOrdered();
void laterGenerationProgressRecoversOnlyItsExactJob();
void staleGenerationCannotRecoverVisibility();
void clearRemovesOnlyRequestedTerminalAndEmitsRemoved();
void clearedTerminalCannotBeRecreatedByLateMessages();
void malformedOrNonProgressMessageDoesNotMutate();
```

Construct events with explicit Task 7 generation and `message-id`. Tests compare
complete before/after snapshots on every rejected call and assert zero signals.
An identical `(generation, message-id, payload)` replay is successful but has no
mutation or signal. Reusing the same `(generation, message-id)` with a different
payload is `Contract`; an equal payload under a different delivery identity is a
distinct accepted event and is preserved. This avoids unsafe content-based
deduplication.

- [x] **Step 2: Run registry tests to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2JobRegistry --parallel
```

Expected: missing-registry compile failure.

- [x] **Step 3: Implement the delivery envelope and registry interface**

```cpp
struct Xc2JobEvent {
    Xc2StompGeneration generation = 0;
    QString messageId;
    Xc2JobProgress progress;
};

struct Xc2JobRecord {
    QString jobId;
    Xc2JobState state = Xc2JobState::Created;
    QList<Xc2JobEvent> events;
    bool visibilityLost = false;
    bool terminal() const;
};

class Xc2JobRegistry final : public QObject {
    Q_OBJECT
public:
    explicit Xc2JobRegistry(QObject *parent = nullptr);
    bool accept(const Xc2JobAccepted &job, Xc2Error *error = nullptr);
    bool apply(const Xc2JobEvent &event, Xc2Error *error = nullptr);
    bool applyMessage(const Xc2StompMessage &message,
                      Xc2Error *error = nullptr);
    void markVisibilityLost(Xc2StompGeneration generation);
    std::optional<Xc2JobRecord> job(const QString &jobId) const;
    QList<Xc2JobRecord> activeJobs() const;
    bool clearTerminal(const QString &jobId, Xc2Error *error = nullptr);

signals:
    void jobChanged(const Xc2JobRecord &);
    void jobTerminal(const Xc2JobRecord &);
    void jobRemoved(const QString &jobId);
};
```

Declare `Q_DECLARE_METATYPE` outside the namespace for `Xc2JobEvent` and
`Xc2JobRecord`, and register them before `QSignalSpy` use. `applyMessage()`
requires `Topic::Progress`, the exact profile destination, non-empty Task 7
`messageId`, and a strict successful `Xc2JsonCodec::jobProgress()` result before
constructing an event. Do not trust topic text or parse the body a second way.

- [x] **Step 4: Implement one atomic transition and signal policy**

Validate the complete input before changing maps, ordering, visibility,
tombstones, or event lists. Apply this exact table:

| Operation | Result and mutation | Signals |
|---|---|---|
| first valid `accept(jobId)` | append jobId to first-observed order; create `Created`, empty events, visible | one `jobChanged` |
| `accept` for an existing retained record, including terminal | successful idempotent replay; never reset state/events/visibility | none |
| blank/whitespace or tombstoned `accept` | fail without mutation | none |
| first valid event for an unknown job | create the exact jobId, preserve the event, set its reported state | `jobChanged`, then `jobTerminal` only if terminal |
| exact delivery replay with identical payload | successful idempotent replay | none |
| same delivery identity with different payload | `Contract` failure | none |
| valid event for `Created`/`InProgress` | append once, transition, and clear visibility only under the generation rule below | `jobChanged`, then `jobTerminal` only if terminal |
| any event for a terminal record | `Job` failure, including an identical terminal payload under a new identity | none |
| first `markVisibilityLost(generation)` | set the independent flag on each non-terminal record in stable order; retain backend state | one ordered `jobChanged` per changed record |
| repeated loss or loss with no active jobs | no-op | none |
| clear unknown/non-terminal job | return false without mutation | none |
| clear terminal job | remove only that visible record and retain a registry-lifetime tombstone | one `jobRemoved` |

Delivery identity is registry-global, not per job: reuse of the same pair for a
different job is the same conflicting-payload `Contract` failure. Require a
non-zero generation and non-empty message ID, and compare every progress field
plus raw JSON when deciding whether a replay is identical.

`jobId` is an exact opaque key: never trim, normalize, case-fold, or attach an
unknown event to the only active record. Reject an ID whose trimmed form is
empty, but preserve every other accepted byte-for-byte QString value. Set
`Xc2Error::jobId` on failures. Model-shape/event-identity failures are
`Contract`; illegal lifecycle transitions are `Job`. Clear a supplied error on
every successful call, including idempotent success.

`Created` is internal and invalid in an event. Require non-negative `ticks` and
`totalTicks`; when total is positive require `ticks <= totalTicks`. For repeated
`InProgress`, ticks cannot decrease, and a previously known positive total
cannot return to zero or decrease. A terminal event remains authoritative after
those basic range checks and is not rejected solely because its counters are
lower than the last non-terminal counters; error/cancel payloads may report
terminal-local counts.

Store first-observed job IDs separately from lookup storage so `activeJobs()` is
deterministic across processes and excludes terminal records without reordering
the remainder. `markVisibilityLost(g)` remembers the lost generation. Only a
valid event for that same job with `event.generation > g` clears the flag; an
older/equal generation is stale and cannot mutate or restore visibility. A
record from another job never clears it. Tombstones live for the registry's
session lifetime; a later controller creates a new registry for a new sidecar
session rather than silently reusing retired IDs.

- [x] **Step 5: Write the real-process probe integration tests before the probe**

Extend Task 7's single-authority `FakeXc2TransportServer`; do not create an HTTP
fake and WebSocket fake on different ports. Add scripts for strict health and
current-user responses, current-user 204, valid/malformed error JSON, truncated
and no-response REST, WebSocket/subprotocol failures, missing CONNECTED,
STOMP ERROR, wrong/missing disconnect receipt, and close/deadline races. Capture
all REST method/targets/bodies, upgrade headers, selected subprotocol, and STOMP
frames. Count both profile-classified state-changing REST calls and every
unexpected REST/STOMP operation.

`test_Xc2ContractProbe` launches the actual `$<TARGET_FILE:xc2_contract_probe>`
with `QProcess`; no in-process call may substitute for process exit behavior.
Pass the target and script paths through compile definitions. Add:

```cpp
void happyPathUsesExactReadOnlyAllowlist();
void happyPathUsesDerivedCookieAndV12Stomp();
void invalidCliOrBaseExitsTwoBeforeNetwork_data();
void invalidCliOrBaseExitsTwoBeforeNetwork();
void restTransportAndOverallDeadlineExitTwo_data();
void restTransportAndOverallDeadlineExitTwo();
void malformedRestContractsExitThree_data();
void malformedRestContractsExitThree();
void stompFailuresExitFour_data();
void stompFailuresExitFour();
void unauthenticated204AndMissingPermissionExitFive_data();
void unauthenticated204AndMissingPermissionExitFive();
void finishAndDeadlineRaceExitsOnceAndCleansConnections();
void outputNeverContainsCookieOrUserSentinels();
void wrapperPreservesEveryProbeExitCode_data();
void wrapperPreservesEveryProbeExitCode();
```

For every row, require `QProcess::NormalExit`, the exact exit code, bounded wall
time, no open fake connection after cleanup, zero state-changing requests, and
zero unexpected requests/frames. The success capture is exactly:

```text
GET /xc2/1.0/serviceStatus/status
GET /xc2/1.0/auth/currentUser
GET /xc2-websocket  (WebSocket upgrade, derived Cookie, v12.stomp)
CONNECT
SUBSCRIBE /topic/vci/status
SUBSCRIBE /topic/login
DISCONNECT receipt:disconnect-<generation>
RECEIPT receipt-id:disconnect-<generation>
normal WebSocket close
```

The fake returns observed `CONNECTED heart-beat:0,0`, so no heartbeat obscures
the exact frame allowlist. Require zero shutdown, login, logout, device, vehicle,
autoscan, ECU, measurement, flow, flash, SEND, ACK, NACK, or UNSUBSCRIBE calls.
Success does not wait for a topic MESSAGE: the matching DISCONNECT receipt proves
the broker processed the preceding ordered subscriptions without triggering a
domain operation.

- [x] **Step 6: Register probe tests and verify both new targets are red**

Create `xc2_job_registry` with labels `unit;contract;ktm`. Create
`xc2_contract_probe_integration` with labels `contract;integration;ktm` and a
CTest `TIMEOUT` greater than the test's short injected deadlines but low enough
to fail a hung child. Do not register the live probe itself as a CTest.

```powershell
rtk cmake --build build-test --target test_Xc2JobRegistry test_Xc2ContractProbe --parallel
```

Expected: missing registry/probe compilation fails after CMake knows both
targets; the optional port-8082 service is never contacted.

- [x] **Step 7: Implement the exact read-only contract probe**

Add these immutable Task 1 policy values and tests:

```cpp
QList<Topic> foundationProbeTopics() const;          // VciStatus, Login
QStringList foundationProbePermissions() const;     // EcuDiagnosticRead
```

Both lists are exact and ordered. Permission comparison is case-sensitive.
Do not use `allTopics()` or accept arbitrary permission/topic CLI values.

Parse arguments without letting `QCommandLineParser::process()` choose exit 1.
Require exactly one `--base-url <raw-string>`, allow at most one
`--timeout-ms <100..120000>` with default `30000`, reject all positional,
duplicate, unknown, empty, overflow, and malformed values with exit 2 and zero
network. `--help` is the conventional informational exit 0 with zero network and
does not represent a compatibility result. Preserve the `--base-url` value as
`rawBaseUrlBytes`: first require every original command-line code unit to be
visible ASCII, then convert those ASCII code units byte-for-byte to
`QByteArray`. Do not trim, case-fold, percent-decode, parse, reconstruct, or
otherwise normalize it, and never construct a `QUrl` from it. Call Task 5 as
`restClient.setBaseUrl(rawBaseUrlBytes, &error)` with those exact bytes.

After validation, start one `QDeadlineTimer`/single-shot wall-clock authority and
run this asynchronous state sequence without nested event loops or blocking
waits:

1. `requestServiceStatus()` and require strict `alive`.
2. `requestCurrentUser()` on the same REST client and dedicated cookie jar.
3. Require non-blank `loginName` and `name`, then exact `EcuDiagnosticRead`
   permission. Blank identity is a REST contract failure, not an authenticated
   permission failure.
4. Queue exactly `VciStatus` and `Login` desires, then call Task 7
   `connectToBackend(restClient)` so URL and Cookie cannot be retargeted.
5. Wait for valid `v12.stomp` CONNECTED and exactly the two current-generation
   `subscriptionSent` signals in profile order.
6. Call `disconnectFromBackend()` and succeed only after Task 7 reports the
   matching receipt and normal close with no terminal error.

Track the current REST request ID, STOMP generation, stage, and a completed flag.
Every callback first verifies identity and checks whether the outer deadline has
expired. One `finishOnce(code)` stops timers, aborts a pending Task 5 request,
calls Task 7 `abortCurrentGeneration()` when needed, closes resources, and exits
the process once. The outer deadline includes Task 7's internal connect and
graceful-disconnect work; those are backstops, not permission to exceed it.

Use this complete result table:

| Condition | Exit |
|---|---:|
| normal read-only sequence and receipt-confirmed close | `0` |
| invalid CLI/base URL; REST transport/timeout; health or user backend unavailable; overall deadline while in REST | `2` |
| REST `Contract`, including malformed success/error payload and health 204 | `3` |
| WebSocket upgrade/subprotocol, STOMP negotiation/frame/subscription/receipt/close failure, or overall deadline after STOMP begins | `4` |
| exact current-user HTTP 204 with empty body; strictly decoded HTTP 401/403 XC2 auth error; valid user missing exact `EcuDiagnosticRead` | `5` |

Task 5 deliberately retains current-user 204 as `Contract`. Only the probe's
known current-user stage plus exact `httpStatus == 204` and empty raw body maps
that environment result to 5. A malformed 401/403 error remains 3; other backend
errors remain 2. Health 204 never maps to 5.

Write only a bounded diagnostic containing stage, category, HTTP status, XC2
code, and sanitized summary. Never print response raw bytes, Cookie/Set-Cookie,
login/name/dealer fields, permissions, sessionIndex, developer detail, or an
arbitrary backend message. Exit 0 must not persist approval or enable any later
state-changing capability.

- [x] **Step 8: Implement the strict PowerShell wrapper**

`run_xc2_mock_contract.ps1` uses `[CmdletBinding()]` with mandatory raw string
`-BaseUrl`, optional literal `-ProbePath` defaulting to the repository's exact
`build-test/xc2_contract_probe.exe`, and validated `-TimeoutMs` defaulting to
`30000`. Do not type `BaseUrl` as `[Uri]`, because PowerShell normalization would
hide inputs that the C++ validator must reject. Resolve and require one existing
leaf ProbePath, invoke it with an argument array, then immediately assign
`$probeExitCode = $LASTEXITCODE` and finish with
`exit [int]$probeExitCode` before running another command.

The wrapper contains no default backend URL, Java/JAR lookup, installation
inspection, process manager, `Start-Process`, login, or shutdown. A missing probe
path is wrapper exit 2; it must never fall back to another build
or executable. The integration test supplies the exact CMake target path and
proves pass-through of `0`, `2`, `3`, `4`, and `5`, including a path containing
spaces.

- [x] **Step 9: Run narrow Windows verification and the optional live probe**

```powershell
rtk cmake --build build-test --target test_Xc2JobRegistry test_Xc2ContractProbe --parallel
rtk ctest --test-dir build-test --output-on-failure -R "^xc2_(job_registry|contract_probe_integration)$"
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\ktm\run_xc2_mock_contract.ps1 -BaseUrl http://127.0.0.1:8082/xc2/1.0 -ProbePath .\build-test\xc2_contract_probe.exe -TimeoutMs 30000
```

The first two commands are mandatory and deterministic. The third is optional
environment evidence only. It returns 0 only for the narrowly defined read-only
surface and a current user with `EcuDiagnosticRead`; an unauthenticated local
mock returns 5 and is recorded, never converted into a passing test. It neither
starts nor stops the explicitly selected service.

- [ ] **Step 10: Add required three-platform CI evidence**

Create `ktm-xc2-foundation.yml` for `pull_request` and branch pushes; do not use
the manual packaging/release workflow as the only test gate. Use read-only
`contents` permission and three independent jobs. Each checks out
`${{ github.sha }}` and asserts `git rev-parse HEAD` equals the event SHA. The
Windows job is
pinned to `windows-2022`, installs Qt WebSockets, configures
`BUILD_TESTING=ON` and `RX14_KTM_XC2=ON`, performs the full build, and runs
`ctest --test-dir build --output-on-failure -L ktm --no-tests=error`. It runs
only fake/golden tests and never downloads or launches XC2/Java.

Before configure, Linux and macOS jobs create the CMake file API
`codemodel-v2` query, then explicitly configure
`-DBUILD_TESTING=ON -DRX14_KTM_XC2=OFF`. Require the exact cache entry
`RX14_KTM_XC2:BOOL=OFF`, no `Qt6Test_DIR` or `Qt6WebSockets_DIR` cache entry,
and no reply-codemodel target name matching `(ktm|xc2)` case-insensitively.
Each job performs the full build and an unfiltered CTest run. A successful
Windows `RX14_KTM_XC2=OFF` configure is useful local evidence for the option-off
branch, but it is not evidence of a non-Windows build. Keep this step unchecked
until the Linux/macOS job URLs and the common exact commit SHA are retained.

- [x] **Step 11: Self-review and commit the implementation to trigger CI**

```powershell
rtk python .superpowers/sdd/task-8c-workflow-check.py
rtk rg -n "T[B]D|T[O]DO|F[I]XME|P[L]ACEHOLDER" .github/workflows/ktm-xc2-foundation.yml docs/superpowers/plans/2026-07-16-ktm-xc2-foundation-plan.md docs/superpowers/specs/2026-07-16-ktm-xc2-integration-design.md
rtk git diff --check
rtk git status --short
rtk git add .github/workflows/ktm-xc2-foundation.yml docs/superpowers/specs/2026-07-16-ktm-xc2-integration-design.md docs/superpowers/plans/2026-07-16-ktm-xc2-foundation-plan.md
rtk git diff --cached --check
rtk git diff --cached --name-only
rtk git commit -m "feat: complete the XC2 communication foundation"
```

Before the implementation commit, status must contain exactly the three Task 8c
tracked files staged above. Leave Steps 10 and 12 and the Phase 1 completion gate
open until the pushed implementation commit's three jobs finish for one exact
SHA.

- [ ] **Step 12: Record CI evidence, finish checkboxes, and commit docs**

After the implementation commit is pushed, require green Windows, Linux, and
macOS jobs for that exact SHA. Record their run URLs/SHA beside this step, mark
only evidence-backed checkboxes, rerun `rtk git diff --check`, and commit the
plan-only follow-up:

```powershell
rtk git add docs/superpowers/plans/2026-07-16-ktm-xc2-foundation-plan.md
rtk git commit -m "docs: record XC2 foundation verification"
```

If any platform job is unavailable or red, keep this task and the Phase 1 gate
open. A local Windows run, source inspection, or configured workflow file is not
a substitute for non-Windows execution evidence.

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
- Job delivery identity, atomic signals, stable ordering, visibility recovery,
  terminal tombstones, and non-regressive progress behave deterministically.
- The real probe executable passes the single-authority fake allowlist with zero
  state-changing/unexpected calls and every documented exit code.
- The optional live probe performs only the two approved REST reads, two status
  subscriptions, and receipt-confirmed disconnect under one deadline.
- Green Windows-ON and Linux/macOS-OFF CI jobs exist for the same commit; the
  main application builds with `RX14_KTM_XC2=OFF` without KTM targets or Qt
  WebSockets/Test discovery.
