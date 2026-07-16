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
    EcuFunctions, EcuMeasurementsGet, EcuMeasurementsStart,
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

Implement every enum value with the observed XC2 path/topic. Dynamic paths use named templates such as `ecu/open/{ecuId}`; later clients replace only declared placeholders after percent-encoding. Classify discovery/jobs, selection, clear, flow, and flash endpoints as `StateChanging` with zero retries. `ServiceStatus`, `CurrentUser`, settings reads, vehicle info, ECU-domain/function reads, and measurement-definition reads are `ReadOnly`; keep their profile retry value zero because retry policy belongs to the caller.

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
- Produces: `Xc2Error`, `Xc2Result<T>`, `Xc2ServiceStatus`, `Xc2CurrentUser`, `Xc2VciDevice`, `Xc2JobAccepted`, `Xc2JobProgress`, and strict `Xc2JsonCodec` parse/serialization functions.

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

void Xc2JsonCodecTest::jobSchemasUseExactWireFieldNamesAndStates()
{
    const auto accepted = Xc2JsonCodec::jobAccepted(
        loadFixture("rest/job-accepted.json"));
    QVERIFY(accepted.ok());

    const auto progress = Xc2JsonCodec::jobProgress(
        loadFixture("rest/job-progress.json"));
    QVERIFY(progress.ok());
    QCOMPARE(progress.value->state, Xc2JobState::InProgress);

    QVERIFY(!Xc2JsonCodec::jobProgress(
        R"({"jobId":"synthetic","state":"IN_PROGRESS","ticks":1,"totalTicks":2,"message":"x"})").ok());
    QVERIFY(!Xc2JsonCodec::jobProgress(
        R"({"jobId":"synthetic","status":"CREATED","ticks":1,"totalTicks":2,"message":"x"})").ok());
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

struct Xc2JobProgress {
    QString jobId;
    Xc2JobState state = Xc2JobState::Created;
    qint64 ticks = 0;
    qint64 totalTicks = 0;
    QString message;
    QJsonObject raw;
};
```

Place every type and codec in `namespace ktm::xc2`. Put
`Q_DECLARE_METATYPE(ktm::xc2::...)` declarations outside that namespace for
`Xc2Error`, each signal-carried model, and the concrete `Xc2Result<T>`
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
integer `ticks`/`totalTicks`, and string `message`. `error.json` contains the
five wire fields with `status:403`, `code:1007`, and only synthetic text.
`manifest.json` records the approved backend profile ID, capture date
`2026-07-16`, route/topic, and that VIN/dealer identifiers were replaced. Do
not copy real VINs, credentials, or dealer data.

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
- Consumes: `Xc2Error` from Task 2.
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

`connected.frame` records the observed local mock negotiation
`version:1.2` and `heart-beat:0,0` after the client offered
`heart-beat:10000,10000`. Compatibility evidence is a
read-only `CONNECT`/`DISCONNECT` at `ws://127.0.0.1:8082/xc2-websocket` using
subprotocol `v12.stomp` on `2026-07-16`; record it in the fixture manifest.
`progress-message.frame` targets `/topic/progress`, includes required
`destination`, `message-id`, and `subscription` headers, and carries the
synthetic job ID from Task 2. `error.frame` has an artificial protocol error
only. Every `.frame` is a binary fixture ending in a real `0x00` byte, not the
two text characters `\\0`; tests assert the terminator and exact byte count.

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
  reply signals, authority-bound `webSocketUrl`/`cookieHeaderFor`, and `abort`.

- [ ] **Step 1: Write a complete loopback fake server before the client**

`FakeHttpServer` binds an explicitly supplied `QHostAddress::LocalHost` or
`QHostAddress::LocalHostIPv6` on port zero. It incrementally parses the request
line, headers, `Content-Length`, and body; a request enters the capture list and
increments `requestCount()` exactly once only after the complete body has been
received. Each capture exposes the exact method, request-target, headers, and
body. Normal queued responses always include an exact `Content-Length` and
`Connection: close`; special scripted responses support close-before-status,
declared-length truncation, drip bytes without completion, no bytes, and a
redirect `Location`. A trap server independently counts requests so redirect
tests prove that it was never contacted.

The server must let a test wait for `requestCaptured` before calling `abort()`;
do not use a timing guess that can cancel before the request reaches the wire.

- [ ] **Step 2: Write failing URL, request-target, redirect, and cookie tests**

Add these data-driven cases to `test_Xc2RestClient.cpp`:

```cpp
void acceptsIpv4Ipv6AndLocalhostBases_data();
void acceptsIpv4Ipv6AndLocalhostBases();
void rejectsUnsafeOrAliasedBases_data();
void rejectsUnsafeOrAliasedBases();
void sendsExactRequestTargets();
void ownedManagerBypassesApplicationProxy();
void get302DoesNotFollowRedirect();
void shutdown307DoesNotFollowRedirect();
void cookieExportRequiresDerivedWebSocketAuthority();
```

The accepted table contains all of these forms, with and without the one
permitted trailing slash where shown:

```text
http://127.0.0.1:<port>/xc2/1.0
http://127.0.0.1:<port>/xc2/1.0/
http://[::1]:<port>/xc2/1.0
http://LOCALHOST:<port>/xc2/1.0/
```

The rejected table contains a relative URL, `https` and `ws`, a missing or zero
port, user info, query, fragment, `localhost.`, `foo.localhost`,
`localhost.localdomain`, `127.1`, integer/octal IPv4 aliases, a non-loopback
numeric IPv4/IPv6 address, `/xc2/1.0//`, a case-changed path, an extra segment,
a dot segment, and percent-encoded path variants. Validation performs no DNS
lookup. It accepts only an absolute `http` URL with an explicit port in
`1..65535`, no user info/query/fragment, and a fully encoded path exactly equal
to `/xc2/1.0` or `/xc2/1.0/`. The host is either case-insensitive exact
`localhost` or a numeric address parsed by `QHostAddress` and proven loopback;
host aliases are never resolved. Store the context without a trailing slash.

For every endpoint construct the URL as the validated authority plus exactly
`restContext() + '/' + EndpointSpec::path`; never call `QUrl::resolved()`.
Assert captured targets are exactly
`/xc2/1.0/serviceStatus/status`, `/xc2/1.0/auth/currentUser`, and
`/xc2/1.0/serviceStatus/shutdown`, with no doubled slash or inherited suffix.

Every request sets `QNetworkRequest::RedirectPolicyAttribute` to
`QNetworkRequest::ManualRedirectPolicy`. Queue a GET `302` and shutdown POST
`307`, each pointing at the trap server. Both operations must fail, the origin
must record one request, and the trap must record zero requests after the full
deadline. There is no automatic retry.

The cookie test returns a synthetic session cookie, then accepts export only
for the one URL derived from the REST base:
`ws://<same validated host>:<same explicit port>/xc2-websocket`. The supplied
WebSocket URL must be absolute `ws`, have no user info/query/fragment, match the
validated REST host and explicit port, and have the exact case-sensitive path
`/xc2-websocket`. Canonicalize the validated REST host once, derive
`webSocketUrl()` from it, and require the supplied URL to equal that canonical
result rather than resolving another spelling. Reject wrong/missing port, host
alias, `wss`, path suffix, and query before calling
`QNetworkCookieJar::cookiesForUrl()`; this explicit port check is mandatory
because cookie matching itself is not port-scoped. Join only the dedicated
jar's returned cookies with
`QNetworkCookie::NameAndValueOnly`.

- [ ] **Step 3: Write failing deadline, exactly-once, and response-matrix tests**

Add these cases:

```cpp
void totalDeadlineStopsDripAndNeverResponses_data();
void totalDeadlineStopsDripAndNeverResponses();
void abortAfterCaptureCompletesCanceledExactlyOnce();
void timeoutAndFinishedRaceCompletesExactlyOnce();
void classifiesCompleteResponseMatrix_data();
void classifiesCompleteResponseMatrix();
void shutdownIsOneExactEmptyFormPost();
```

Use short injected test deadlines and `QElapsedTimer`. One server sends a byte
often enough to stay below the transfer inactivity timeout but never completes;
another accepts the request and never sends a byte. Each must finish once near
the independent total wall-clock deadline, not remain alive indefinitely.
After `requestCaptured`, `abort(id)` must produce exactly one result with
`Transport/Canceled`. Exercise timeout and `finished` in the same event-loop
turn and assert one terminal signal and removal of the pending request.

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

Transport/truncation takes precedence over the HTTP/body matrix. Cancellation
and timeout take precedence over a network error produced by `abort()`. The
shutdown capture must be exactly one `POST` with request-target
`/xc2/1.0/serviceStatus/shutdown`, content type
`application/x-www-form-urlencoded`, `Content-Length: 0`, an empty body, and no
retry. Close without a response and wait beyond the request deadline to prove
`requestCount() == 1`.

- [ ] **Step 4: Run the REST target to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2RestClient --parallel
```

Expected: missing-client compile failure.

- [ ] **Step 5: Implement the owned transport and domain interface**

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
    bool setBaseUrl(const QUrl &loopbackRestBase, Xc2Error *error = nullptr);
    QUrl baseUrl() const;
    QUrl webSocketUrl() const;

    Xc2RequestId requestServiceStatus();
    Xc2RequestId requestCurrentUser();
    Xc2RequestId requestShutdown();
    Xc2Result<QByteArray> cookieHeaderFor(const QUrl &url) const;
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

The client always creates and owns its own `QNetworkAccessManager` and its own
`QNetworkCookieJar`; there is no constructor or setter for an external manager
or jar. Set the manager proxy explicitly to `QNetworkProxy::NoProxy` so system,
application, or injected proxy state cannot weaken the loopback/redirect/cookie
guarantees. The proxy test temporarily installs a trap application proxy and
proves the request still goes directly to the loopback fake.

- [ ] **Step 6: Implement one pending record and one completion path**

Create one pending record per request containing ID, endpoint, reply pointer,
deadline timer, forced transport reason, and a completed flag. A single private
`completeOnce(id)` reads/classifies the finished reply, stops the timer, removes
the record, emits the endpoint's terminal signal, and calls `deleteLater()`.
Only `QNetworkReply::finished` may call `completeOnce`; `errorOccurred` merely
records evidence. `abort(id)` sets `Canceled` and calls `reply->abort()`.
Deadline expiry sets `Timeout` and calls `reply->abort()`. Neither path emits or
deletes directly, so abort/error/finished races cannot double-complete.

Start a per-request single-shot wall-clock timer from request creation and also
set Qt's transfer timeout as an inactivity backstop. The total timer is the
authority even when bytes continue to arrive. A non-forced network failure maps
to `Xc2ErrorCategory::Transport` plus `Xc2TransportReason::Network`; successful
HTTP/body classifications retain `None`. Apply the response matrix from Step 3
without treating a present HTTP status as proof that a truncated transfer was
complete.

- [ ] **Step 7: Run REST and contract tests**

```powershell
rtk cmake --build build-test --target test_Xc2RestClient --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: all matrix, strict-URL, redirect-trap, cookie-authority, deadline, and
exactly-once tests pass; shutdown is one exact request and both redirect traps
remain at zero requests.

- [ ] **Step 8: Commit the REST client**

```powershell
rtk git add CMakeLists.txt src/ktm/xc2/Xc2Models.h src/ktm/xc2/Xc2RestClient.* tests/ktm/FakeHttpServer.* tests/ktm/test_Xc2RestClient.cpp
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
- Produces: `Xc2LaunchSpec`, `Xc2BackendState`, `Xc2BackendEndpoints`, the
  validated startup user, and an asynchronous, PID-owned `Xc2BackendManager`
  lifecycle.

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
```

It returns plain `alive` for exact
`GET /xc2/1.0/serviceStatus/status`, a strict synthetic identity with a
non-empty `permissions` array for exact `GET /xc2/1.0/auth/currentUser`, and
exits cleanly after exact empty-form
`POST /xc2/1.0/serviceStatus/shutdown`. It never opens a non-loopback socket.
The output modes split logical lines across writes, emit a line larger than the
client cap, and emit synthetic `Authorization`, `Cookie`, `password`, `token`,
`sessionIndex`, and `SAMLResponse` fields on both streams. `--hold-lock` obtains
a `QLockFile` and waits, allowing a test to kill the helper and leave a real
stale per-user lock file.

Pass `$<TARGET_FILE:fake_xc2_sidecar>` to the test through compile definition
`FAKE_XC2_SIDECAR_PATH`.

- [ ] **Step 2: Write failing launch-profile and bind tests**

Add these cases first:

```cpp
void productionArgumentsOverrideEveryEmbedded8082();
void productionArgumentsKeepRequiredJavaOrderingAndProfile();
void invalidPrerequisiteReportCannotCreateProductionSpec();
void fakeArgumentsReceiveManagerSelectedPort();
void rejectsEveryBindAddressExceptIpv4Localhost_data();
void rejectsEveryBindAddressExceptIpv4Localhost();
```

`productionArgumentsOverrideEveryEmbedded8082()` calls `argumentsForPort(49123)`
and requires these exact effective JVM system properties, all before `-jar`:

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
`:8082`, every callback/GRIPS authority uses the selected port, and
`-Dserver.port` is before `-jar`.

Also require `-Dloader.main=com.avl.ditest.xc2.Xc2NgApplication` before `-jar`,
`-Dspring.profiles.active=dev`, and
`-Dcom.avl.ditest.xc2.developer=true`. The latter is the explicitly approved
local diagnostic launch profile only; it does not claim or simulate DealerNet
authentication. `productionLaunchSpec` consumes a successful Task 4
`Xc2PrerequisiteReport`, so the checked x86 Java path/PE validation cannot be
bypassed by passing an arbitrary layout.

Retain the existing required arguments
`-Dssc.includezip=true`, `-Dlogging.config=config/log.xml`, and
`-Dspring.config.additional-location=file:./config/custom-cloud.properties`;
all JVM `-D` arguments, including `loader.main`, the dynamic URL overrides, the
`dev` profile, and the diagnostic flag, precede the single `-jar` token and the
absolute `xc2_backend_patched.jar` path.

The fake spec's `argumentsForPort` must return `--port <selected>`. The bind
table accepts only `QHostAddress::LocalHost` (`127.0.0.1`) and rejects
`Any`, `AnyIPv4`, `AnyIPv6`, `LocalHostIPv6`, `127.0.0.2`, and non-loopback
addresses before acquiring a lock or spawning. Endpoints may be derived only as
`http://127.0.0.1:<selected>/xc2/1.0` and
`ws://127.0.0.1:<selected>/xc2-websocket` after the address, generation, and
listener PID have all been validated.

- [ ] **Step 3: Write failing PID ownership, collision, and lock tests**

Use real child processes, `GetExtendedTcpTable`, and bounded
`QSignalSpy`/`QTRY_COMPARE_WITH_TIMEOUT` checks for:

```cpp
void readyRequiresSelectedListenerOwnedByCurrentChildPid();
void validAliveDecoyOn8082IsIgnoredAndSurvives();
void portStealRaceCleansChildAndRetriesANewPort();
void shutdownIsSentOnlyToCurrentChildPid();
void ignoreShutdownIsTerminatedThenKilledWithoutResidue();
void perUserLockRejectsSecondManagerUntilChildFinished();
void stalePerUserLockIsRecovered();
void finishedAndErrorRevokeOwnership();
```

Start a fake sidecar decoy on `127.0.0.1:8082` that returns a completely valid
`alive`. Start the manager and prove its selected port is not 8082, the Windows
listener owner PID equals `ownedProcessId()`, that PID is the manager's current
fake `QProcess`, and the decoy remains alive after the owned child stops. A
health response alone is not proof of ownership.

For the port-steal race, let the first `argumentsForPort` callback launch a
separate fake process on the just-selected candidate before the managed child
can bind it. The valid response is therefore from the wrong PID. The manager
must never emit `ready`, asynchronously terminate/kill and reap its failed child,
select a different port, call `argumentsForPort` again, and become Ready only
for the new child/PID. Bound retries to three candidates; exhaustion is a
startup failure with no orphan. The test explicitly reaps its stealer.

Give both managers the same temporary lock path. The first obtains the
`QLockFile` before spawning; the second emits a busy failure without spawning.
The lock remains held through graceful shutdown, terminate, kill, and collision
cleanup, and is released only after the actual child has emitted `finished` and
`QProcess::state() == NotRunning`. Then the second manager can start. For stale
recovery, kill the `--hold-lock` helper without running its destructor and prove
the manager safely recovers that dead-PID lock; it must never remove a live
owner's lock. Production uses a per-user path under
`QStandardPaths::AppLocalDataLocation`, while the explicit lock path in
`Xc2LaunchSpec` is a deterministic test seam.

- [ ] **Step 4: Write failing startup, lifecycle, and output tests**

Add:

```cpp
void healthPollingIsSingleFlightWithinOneTotalDeadline();
void startupTimeoutAbortsReplyAndLateAliveCannotReviveRun();
void currentUserAndPermissionsAreValidatedBeforeReady();
void earlyCrashReportsBackendErrorAndLeavesNoOwnedRun();
void stopDuringStartNeverEmitsReady();
void stopIsIdempotentAndRestartWaitsForStopped();
void destroyRunningManagerReapsWithoutUiThreadWait();
void stdoutAndStderrUseIndependentFragmentBuffers();
void outputLinesAndRingsAreBounded();
void outputControlsAndSensitiveFieldsAreRemoved();
```

At most one health request may be pending. A single startup wall-clock deadline
covers process start, all health attempts, listener ownership verification, and
the strict `currentUser` request. On timeout, abort the pending REST reply,
invalidate the run generation, cancel every poll timer, and asynchronously
terminate then kill. A scripted alive reply delivered after the deadline must
not transition the stale generation to Ready. Before Ready, parse
`currentUser`, retain the exact returned `permissions`, and test the synthetic
diagnostic identity/permission list; missing or malformed identity/permissions
fails startup rather than being described as DealerNet-authenticated.

Output parsing keeps separate incremental byte buffers and rings for stdout and
stderr. Emit only complete logical lines (and the final partial line at EOF),
handle CRLF split across reads, cap each input line at 16 KiB, discard the
remainder of an oversized line through its delimiter, append one `[truncated]`
marker, and retain at most 256 sanitized lines per stream. Strip ANSI CSI/OSC
sequences and C0/C1 controls other than tab before emission. Case-insensitively
replace values for `Authorization`, `Proxy-Authorization`, `Cookie`,
`Set-Cookie`, `password`, `token`, `sessionIndex`, `SAMLRequest`, and
`SAMLResponse` in header, `key=value`, or JSON-string form with `<redacted>`.
Tests split both the sensitive field name and value across process reads and
assert neither `outputLine` nor `recentOutput()` contains the synthetic secret.

- [ ] **Step 5: Run the manager target to verify red behavior**

```powershell
rtk cmake --build build-test --target test_Xc2BackendManager --parallel
```

Expected: missing manager compile failure.

- [ ] **Step 6: Implement launch spec and manager interface**

```cpp
struct Xc2LaunchSpec {
    QString program;
    std::function<QStringList(quint16)> argumentsForPort;
    QString workingDirectory;
    QHostAddress bindAddress = QHostAddress::LocalHost;
    QString lockFilePath;
    int startupTimeoutMs = 60000;
    int shutdownTimeoutMs = 5000;
    int maxPortAttempts = 3;
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
    ~Xc2BackendManager() override;
    static Xc2Result<Xc2LaunchSpec> productionLaunchSpec(
        const Xc2PrerequisiteReport &report);
    void start(Xc2LaunchSpec spec);
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
};
```

- [ ] **Step 7: Implement run-scoped ownership and Windows listener proof**

Represent each run with its own generation, `QProcess`, `QLockFile`, selected
port, process ID, startup/shutdown timers, poll timer, pending REST request IDs,
output buffers, and completion state. `start()` is accepted only in `Stopped`.
Validate the spec and obtain the per-user lock before selecting a port or
spawning. Select a candidate by briefly binding `QTcpServer` to
`127.0.0.1:0`, reject candidate 8082, close it, then and only then call
`argumentsForPort(selected)`. Set `QProcess` program/arguments/working directory
and connect `started`, both ready-read signals, `errorOccurred`, and `finished`
with the captured generation. Every process signal, timer callback, REST result,
and reaper/collision continuation checks that captured generation before reading
or mutating the current run.

On Windows, query `GetExtendedTcpTable(TCP_TABLE_OWNER_PID_LISTENER)` and match
the network-byte-order local port plus IPv4 `127.0.0.1`. Ready requires all of:
current generation, live current `QProcess`, successful `alive`, listener PID
equal to `QProcess::processId()`, and a strict current-user result. Re-run the
same PID check immediately before shutdown. POST shutdown only when it still
matches; if another PID owns the port, send it zero requests and clean up only
the manager-owned `QProcess`. Link `Iphlpapi`. `finished` and `errorOccurred`
immediately revoke network ownership for that generation; never let a PID or
endpoint from a prior run authorize a later callback.

Health polling is timer-driven and schedules the next request only after the
previous one finishes. `stop()` is idempotent in `Stopped`/`Stopping`, cancels
old timers, invalidates the generation's ability to become Ready, and follows
verified shutdown -> terminate -> kill without blocking. A restart is rejected
until the prior child is truly NotRunning and `stopped` has transitioned the
manager back to `Stopped`.

The destructor must not call `waitForStarted`, `waitForFinished`, or run a local
event loop on the UI thread. Move a still-running process together with its
lock into an application-lifetime internal reaper that performs asynchronous
terminate/kill, observes `finished`/`NotRunning`, and only then unlocks and
deletes the process context. Thus destruction returns promptly without either
orphaning a child or releasing the lock while that child still exists.

- [ ] **Step 8: Run manager and all KTM tests**

```powershell
rtk cmake --build build-test --target test_Xc2BackendManager --parallel
rtk ctest --test-dir build-test --output-on-failure -L ktm
```

Expected: Ready is gated by the selected child PID plus strict health/user
results; dynamic-port overrides, lock/collision recovery, stale-generation
suppression, output bounds/redaction, and all stop/destruction paths pass with
no child residue. The valid 8082 decoy and redirect/port-steal traps remain
alive and receive no unauthorized shutdown.

- [ ] **Step 9: Commit the owned manager**

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
rtk git add CMakeLists.txt src/ktm/xc2/Xc2JobRegistry.* tests/ktm/test_Xc2JobRegistry.cpp tests/ktm/xc2_contract_probe.cpp tests/ktm/run_xc2_mock_contract.ps1 docs/superpowers/plans/2026-07-16-ktm-xc2-foundation-plan.md
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
