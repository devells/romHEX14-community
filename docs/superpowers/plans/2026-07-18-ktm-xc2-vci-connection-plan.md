# KTM XC2 VCI Connection Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Deliver the first operator-usable KTM diagnostic vertical slice: start the approved XC2 sidecar, establish one same-authority REST/STOMP session, discover and select an AVL DiTEST VCI2K, display connection voltage, and disconnect it safely.

**Architecture:** Extend the existing typed XC2 wire layer, then place a headless `KtmSessionController` above `Xc2BackendManager`, `Xc2RestClient`, `Xc2StompClient`, and `Xc2JobRegistry`. A modeless native Qt workspace projects controller state and owns the controller; `MainWindow` owns only the action and workspace lifetime. Production starts only an approved x86 Java sidecar from a validated install root; tests use strict loopback fakes and never start XC2 or vendor code.

**Tech Stack:** C++17, Qt 6 Core/Network/WebSockets/Widgets/Test, CMake/CTest, PowerShell, GitHub Actions.

---

## Global Constraints

- Support only AVL DiTEST VCI2K through D-PDU API. The provider name is the exact, case-sensitive string `AVL Ditest VCI2K_DPDU_API`; an actual hardware enumeration match remains a HIL question.
- The x64 Qt process communicates only over loopback REST/STOMP with the approved x86 Java sidecar. It never launches `XC2.exe`, loads a vendor DLL/JAR in-process, or connects to an arbitrary endpoint.
- Production accepts only an install root validated by `Xc2InstallationProbe`. Test endpoint/process injection stays private behind friend test access; it is never a public runtime API.
- Do not inspect, bind, stop, or reuse the unrelated listener already present on `:::8082`. Production continues to allocate and ownership-prove its own loopback port.
- Do not commit vendor binaries, firmware, credentials, captures containing secrets, or synthetic fixtures represented as live captures.
- REST and STOMP must use the same canonical loopback authority. The controller runs `serviceStatus` and then `currentUser` through its own REST client so that client receives the session Cookie, case-sensitively requires `EcuDiagnosticRead`, and passes that same cookie-owning client to STOMP.
- Device operations expose typed methods and typed completion signals only. Do not add a public generic endpoint, generic request body, URL override, retry switch, redirect, or proxy path.
- A request ID of zero is an immediate operation failure. At most one lookup/apply/close operation is active; request ID, session epoch, selection epoch, and STOMP generation reject stale callbacks.
- `RX14_KTM_XC2=OFF` remains a hard compile boundary: it does not discover Qt Test/WebSockets, create KTM/XC2 targets, add `src/ktm` to `rx14`, or expose KTM symbols from `mainwindow.*`.
- Follow strict TDD for every production behavior: add the focused test, run it and record the expected feature-missing failure, add the minimum implementation, then rerun the focused and relevant regression tests.
- Every shell command below is run from `G:\KTM\romHEX14-community\.worktrees\ktm-xc2-integration` and begins with `rtk`.

## Frozen Wire Contract

- `GET device/lookup`, no body, returns a job object whose key is exactly `jobID`.
- `/topic/progress` messages use the different key `jobId`. Subscribe before lookup. `FINISHED` and XC2-compatible `CANCELED` both continue to `GET device/get`; `ERROR` and `NOT_AUTHORIZED` fail.
- `GET device/get` returns an array. `GET device/getSelected` returns an object or JSON null; an empty 200/204 response is also represented as no selected device until HIL narrows route behavior.
- `POST device/apply` and `POST device/close` send the complete four-field device JSON with `Content-Type: application/json`. Missing/null `additionalModuleInformation` is emitted as explicit JSON null; do not send only an ID.
- XC2's wrapper accepts only HTTP 200/204, maps an empty response to null, rejects invalid nonempty JSON, uses a 30-second request deadline, and performs no automatic retry. Actual per-route 200/204/body combinations remain HIL unknown.
- The device DTO fields are exactly `id`, `name`, `internalName`, and `additionalModuleInformation`.
- `/topic/vci/status` contains lowercase `voltage` (finite JSON number, already volts) and `connected` (JSON boolean). `0.0` is valid and must not be scaled or treated as absent.

## Target Typed Interfaces

Use these shapes verbatim unless a task review proves they cannot compile against Qt/C++17:

```cpp
struct Xc2SelectedVci {
    std::optional<Xc2VciDevice> device;
};

struct Xc2VciStatus {
    double voltage = 0.0;
    bool connected = false;
    QJsonObject raw;
};

Xc2RequestId requestDeviceLookup();
Xc2RequestId requestDevices();
Xc2RequestId requestSelectedDevice();
Xc2RequestId requestApplyDevice(const Xc2VciDevice &device);
Xc2RequestId requestCloseDevice(const Xc2VciDevice &device);

void deviceLookupFinished(Xc2RequestId,
                          const Xc2Result<Xc2JobAccepted> &);
void devicesFinished(Xc2RequestId,
                     const Xc2Result<QList<Xc2VciDevice>> &);
void selectedDeviceFinished(Xc2RequestId,
                            const Xc2Result<Xc2SelectedVci> &);
void applyDeviceFinished(Xc2RequestId, const Xc2Error &);
void closeDeviceFinished(Xc2RequestId, const Xc2Error &);
```

The controller surface is operator-domain typed and has no URL/endpoint escape hatch:

```cpp
enum class KtmSessionState {
    Stopped, BackendStarting, BackendReady, SessionStarting, SessionReady,
    VciLookup, VciApplying, VciReady, VciClosing, Failed
};

bool startProduction(const QString &installRoot, Xc2Error *error = nullptr);
bool lookupVci(Xc2Error *error = nullptr);
bool applyVci(const Xc2VciDevice &device, Xc2Error *error = nullptr);
bool closeVci(Xc2Error *error = nullptr);
void stop();
```

### Task 1: Add strict VCI models, codecs, and synthetic fixtures

**Files:**
- Modify: `src/ktm/xc2/Xc2Models.h`
- Modify: `src/ktm/xc2/Xc2JsonCodec.h`
- Modify: `src/ktm/xc2/Xc2JsonCodec.cpp`
- Modify: `tests/ktm/test_Xc2JsonCodec.cpp`
- Add: `tests/ktm/fixtures/vci_selected.json`
- Add: `tests/ktm/fixtures/vci_selected_null.json`
- Add: `tests/ktm/fixtures/vci_status_connected.json`
- Modify: `tests/ktm/fixtures/manifest.json`

- [x] **Step 1: Write and run RED model/codec tests.** Add tests for selected object/null, exact four fields, characterization of the existing explicit-null device serialization, exact lowercase status keys, strict boolean/number types, finite voltage, valid `0.0`, and no second voltage scaling. Include a casing rejection such as `jobId` where lookup requires `jobID`. Run `rtk proxy powershell -NoProfile -Command '$env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_Xc2JsonCodec --parallel 3; if ($LASTEXITCODE -eq 0) { .\build-ktm\test_Xc2JsonCodec.exe; exit $LASTEXITCODE }; exit $LASTEXITCODE'` and record the expected compile/test failure because the new decode APIs do not exist.
- [x] **Step 2: Add only the typed wire models.** Introduce `Xc2SelectedVci { std::optional<Xc2VciDevice> device; }` and `Xc2VciStatus { double voltage; bool connected; QJsonObject raw; }`, plus the required `Q_DECLARE_METATYPE` declarations. Keep comparisons in tests field-wise under C++17 rather than assuming aggregate equality.
- [x] **Step 3: Implement strict decoding and preserve serialization.** Add `Xc2JsonCodec::selectedVci(const QByteArray &)`, `Xc2JsonCodec::vciStatus(const QByteArray &)`, and retain/regress `vciDeviceJson()` emitting all four DTO fields. `selectedVci` accepts object or JSON null; `vciStatus` rejects missing, wrong-case, wrong-type, NaN/infinite, object/array, and extra scaling.
- [x] **Step 4: Add honest fixtures.** Mark every new fixture `liveCapture:false` in `manifest.json`; describe it as a synthetic contract fixture and do not add machine paths, tokens, or HIL claims.
- [x] **Step 5: Run GREEN and regressions fail-fast.** Run `rtk proxy powershell -NoProfile -Command '$env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_Xc2JsonCodec --parallel 3; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; .\build-ktm\test_Xc2JsonCodec.exe; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ctest --test-dir build-ktm -R "xc2_(json_codec|contract_profile|job_registry)" --output-on-failure; exit $LASTEXITCODE'`; expect all selected tests to pass.
- [x] **Step 6: Verify scope and commit.** Run `rtk proxy powershell -NoProfile -Command 'git diff --check; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; git status --short'`, confirm only Task 1 files changed, then `rtk git add src/ktm/xc2/Xc2Models.h src/ktm/xc2/Xc2JsonCodec.h src/ktm/xc2/Xc2JsonCodec.cpp tests/ktm/test_Xc2JsonCodec.cpp tests/ktm/fixtures/vci_selected.json tests/ktm/fixtures/vci_selected_null.json tests/ktm/fixtures/vci_status_connected.json tests/ktm/fixtures/manifest.json` and `rtk git commit -m "feat: add XC2 VCI wire codecs"`.

### Task 2: Add typed VCI REST operations and strict JSON POST handling

**Files:**
- Modify: `src/ktm/xc2/Xc2RestClient.h`
- Modify: `src/ktm/xc2/Xc2RestClient.cpp`
- Modify: `tests/ktm/FakeHttpServer.h`
- Modify: `tests/ktm/FakeHttpServer.cpp`
- Modify: `tests/ktm/test_Xc2RestClient.cpp`

- [x] **Step 1: Write and run RED typed-operation tests.** Specify methods `requestDeviceLookup()`, `requestDevices()`, `requestSelectedDevice()`, `requestApplyDevice(const Xc2VciDevice &)`, and `requestCloseDevice(const Xc2VciDevice &)`, with matching typed completion signals. Assert exact method/path/body/headers, compact four-field JSON, `Content-Length`, no proxy/redirect/retry, distinct nonzero request IDs, and immediate caller handling of a returned zero. Run `rtk proxy powershell -NoProfile -Command '$env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_Xc2RestClient --parallel 3; if ($LASTEXITCODE -eq 0) { .\build-ktm\test_Xc2RestClient.exe; exit $LASTEXITCODE }; exit $LASTEXITCODE'` and record the expected compile failure.
- [x] **Step 2: Write and run RED response-matrix tests.** Freeze lookup as 200 plus nonempty `jobID` JSON, get as 200 plus array, selected as 200 object/null/empty or 204 empty, and apply/close as 200/204 with empty allowed but every nonempty body required to be valid JSON. Include valid top-level JSON scalar responses and invalid/trailing JSON. Prove a state-changing request is sent once after timeout/failure. Run `rtk proxy powershell -NoProfile -Command '$env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_Xc2RestClient --parallel 3; if ($LASTEXITCODE -eq 0) { .\build-ktm\test_Xc2RestClient.exe; exit $LASTEXITCODE }; exit $LASTEXITCODE'` and record the expected feature-missing compile/assertion failure.
- [x] **Step 3: Generalize the private request engine without exposing it.** Change private `startRequest` to accept only an approved `Endpoint` plus an internally generated JSON body. Permit `PostJson`, retain canonical loopback and cookie protections, use the XC2 device-operation 30-second total deadline, and keep automatic retries at zero.
- [x] **Step 4: Add typed completion dispatch.** Route transport, contract, HTTP, and decode results to the exact operation signal, never to `shutdownFinished`. Decode with the Task 1 codecs; validate any legal top-level JSON value for nonempty apply/close bodies rather than accepting only object/array documents.
- [x] **Step 5: Run GREEN and transport regressions fail-fast.** Run `rtk proxy powershell -NoProfile -Command '$env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_Xc2RestClient --parallel 3; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; .\build-ktm\test_Xc2RestClient.exe; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ctest --test-dir build-ktm -R "xc2_(rest_client|stomp_client)" --output-on-failure; exit $LASTEXITCODE'`; expect all tests to pass with exact one-request assertions.
- [x] **Step 6: Verify scope and commit.** Run `rtk proxy powershell -NoProfile -Command 'git diff --check; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; git status --short'`, then stage only Task 2 files and commit with `rtk git commit -m "feat: add typed XC2 VCI REST operations"`.

### Task 3: Implement the headless KTM session and VCI state machine

**Files:**
- Add: `src/ktm/KtmSessionController.h`
- Add: `src/ktm/KtmSessionController.cpp`
- Add: `tests/ktm/test_KtmSessionController.cpp`
- Modify: `CMakeLists.txt`

For every Task 3 RED/GREEN cycle, run this fail-fast focused command and record the decisive failure/pass: `rtk proxy powershell -NoProfile -Command '$env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_KtmSessionController --parallel 3; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; .\build-ktm\test_KtmSessionController.exe; exit $LASTEXITCODE'`.

- [x] **Step 1: RED the session bootstrap.** Add the test target/file first and define observable states `Stopped`, `BackendStarting`, `BackendReady`, `SessionStarting`, `SessionReady`, `VciLookup`, `VciApplying`, `VciReady`, `VciClosing`, and `Failed`. Test production start accepts only an install root, controller REST base/WebSocket authority equality, controller-owned `serviceStatus -> currentUser` Cookie sequence, exact `EcuDiagnosticRead` permission, and request ID zero as immediate failure. Use only private friend test access. Run the focused build/test fail-fast and record the expected missing-controller failure.
- [x] **Step 2: GREEN only the session bootstrap.** Add the minimum controller ownership and bootstrap: `Xc2BackendManager`, one cookie-owning `Xc2RestClient`, per-session `Xc2StompClient`, and `Xc2JobRegistry`. Expose only typed state/operation signals and the five target commands. Run the focused test fail-fast and require the session cases to pass before adding lookup behavior.
- [x] **Step 3: RED/GREEN lookup ordering.** Add tests, run them to the expected behavioral failure, then implement only lookup. The controller-specific session queues `Login`, `VciStatus`, and `Progress`; lookup waits for `subscriptionSent(Topic::Progress, ...)`. Because the signal has no generation argument, read/validate current generation in the callback and treat it only as locally queued SUBSCRIBE. Connect `jobTerminal` before lookup, accept exact REST `jobID`, then query the registry so progress-before-accept terminal events are retained. `FINISHED` and `CANCELED` continue to device get; `ERROR`/`NOT_AUTHORIZED` fail. Rerun focused GREEN.
- [x] **Step 4: RED/GREEN selection correlation.** Add tests and observe failure before implementation. Reject empty ID or nonexact provider before any network write. Each apply creates a selection epoch and requires apply success, same-ID/approved-provider selected confirmation, and current-generation/current-epoch `connected:true`. Cover both arrival orders, stale pre-apply true, valid `0.0`, and nonidentity name/module-info changes. Implement the minimum epoch correlation and rerun focused GREEN.
- [x] **Step 5: RED/GREEN revocation and cleanup.** Add tests and observe failure for one-operation ownership, `connected:false`, visibility loss, malformed status, stale request/generation/epoch callbacks, and zero hidden reconnect/retry. Test stop ordering: abort REST, abort/disconnect STOMP, recreate the session client to discard desired subscriptions, then stop backend; restart waits for `Stopped`. Implement only these guards and rerun focused GREEN.
- [x] **Step 6: Run full headless regressions fail-fast.** Register the controller test as `ktm_session_controller`, then run `rtk proxy powershell -NoProfile -Command '$env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_KtmSessionController --parallel 3; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; .\build-ktm\test_KtmSessionController.exe; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ctest --test-dir build-ktm -R "ktm_session_controller|xc2_(job_registry|backend_manager)" --output-on-failure; exit $LASTEXITCODE'`; expect all selected tests to pass.
- [x] **Step 7: Verify scope and commit.** Run `rtk proxy powershell -NoProfile -Command 'git diff --check; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; rg -n "QUrl|Endpoint|8082|XC2\\.exe" src/ktm/KtmSessionController.h src/ktm/KtmSessionController.cpp tests/ktm/test_KtmSessionController.cpp; git status --short'`, inspect the complete public section and every match, stage only Task 3 files, and commit with `rtk git commit -m "feat: orchestrate XC2 VCI sessions"`.

### Task 4: Prove the complete flow with a same-authority REST/STOMP fake

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/ktm/FakeXc2TransportServer.h`
- Modify: `tests/ktm/FakeXc2TransportServer.cpp`
- Modify: `tests/ktm/test_KtmSessionController.cpp`

For every Task 4 RED/GREEN cycle, use the same fail-fast focused `test_KtmSessionController` command defined in Task 3 and record the decisive assertion or pass.

- [ ] **Step 1: Write and run RED end-to-end tests before extending the fake.** Add the fake header and happy-path test first; run the focused target and record the expected unresolved-fake link failure because `test_KtmSessionController` does not yet compile `FakeXc2TransportServer.cpp`. Add only the fake sources to that existing test target in `CMakeLists.txt`, rerun, and record the next expected behavioral failure because the current fake cannot serve the typed REST routes or scripted interleavings. Then drive `serviceStatus -> currentUser -> WebSocket/STOMP -> subscriptions -> lookup -> progress -> device/get -> apply -> getSelected plus vci/status -> ready -> close -> disconnected` through one canonical loopback authority.
- [ ] **Step 2: Extend one strict fake authority.** Keep one existing listener/port for REST and the WebSocket/STOMP handshake; record ordered HTTP requests and STOMP frames; provide scripted status codes, bodies, delays, disconnects, and topic messages. Reject unexpected paths, methods, bodies, cookies, subscription order, retries, and cross-authority use. Preserve the existing probe behavior; add the fake sources only to the existing `test_KtmSessionController` test graph, keep every fake consumer test-only, and never add them to a production target.
- [ ] **Step 3: RED/GREEN hostile ordering and terminal compatibility one scenario at a time.** For each of progress-before-lookup-response, `CANCELED`, selected-before-status, status-before-selected, stale generation/epoch, invalid provider with zero POST, malformed/lost status, and visibility loss with zero hidden reconnect: add the assertion, run it to the expected failure, make the minimum fake/controller change, and rerun GREEN before continuing.
- [ ] **Step 4: RED/GREEN close and teardown.** First prove failure for full close JSON, local selection clearing, current-generation `connected:false` revocation, and zero live socket/timer/request/owned fake process after destruction. Then implement only the required cleanup and rerun GREEN. Do not encode a HIL-only assumption that the real backend immediately clears `getSelected`.
- [ ] **Step 5: Run GREEN repeatedly fail-fast.** Run `rtk proxy powershell -NoProfile -Command '$env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_KtmSessionController --parallel 3; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; 1..5 | ForEach-Object { .\build-ktm\test_KtmSessionController.exe; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }; ctest --test-dir build-ktm -R "ktm_session_controller|xc2_(stomp_client|rest_client)" --output-on-failure; exit $LASTEXITCODE'`; expect five clean focused passes and all selected CTest cases green.
- [ ] **Step 6: Verify scope and commit.** Run `rtk proxy powershell -NoProfile -Command 'git diff --check; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; git status --short'`, confirm the only build-graph change adds the two fake sources to `test_KtmSessionController`, stage only Task 4 files including `CMakeLists.txt`, and commit with `rtk git commit -m "test: prove XC2 VCI connection flow"`.

### Task 5: Add the modeless native KTM connection workspace

**Files:**
- Add: `src/ktm/ui/KtmConnectionPage.h`
- Add: `src/ktm/ui/KtmConnectionPage.cpp`
- Add: `src/ktm/ui/KtmServiceWorkspace.h`
- Add: `src/ktm/ui/KtmServiceWorkspace.cpp`
- Add: `tests/ktm/test_KtmConnectionPage.cpp`
- Add: `tests/ktm/test_KtmServiceWorkspace.cpp`
- Modify: `src/mainwindow.h`
- Modify: `src/mainwindow.cpp`
- Modify: `CMakeLists.txt`

`KtmServiceWorkspace.h/.cpp` also declare and implement the testable `KtmServiceWorkspaceHost`; do not add a separate host file or modify `KtmSessionController.*` for Task 5.

Use these fail-fast offscreen commands for every page/workspace RED and GREEN cycle, respectively: `rtk proxy powershell -NoProfile -Command '$env:QT_QPA_PLATFORM="offscreen"; $env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_KtmConnectionPage --parallel 3; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; .\build-ktm\test_KtmConnectionPage.exe; exit $LASTEXITCODE'` and `rtk proxy powershell -NoProfile -Command '$env:QT_QPA_PLATFORM="offscreen"; $env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_KtmServiceWorkspace --parallel 3; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; .\build-ktm\test_KtmServiceWorkspace.exe; exit $LASTEXITCODE'`.

Freeze these 15 object names in the UI tests and production widgets: action `actionKtmService`; workspace `ktmServiceWorkspace`; page `ktmConnectionPage`; install-root edit `ktmInstallRootEdit`; browse button `ktmBrowseInstallRootButton`; start button `ktmStartSessionButton`; stop button `ktmStopSessionButton`; lookup button `ktmLookupVciButton`; device combo `ktmVciDeviceCombo`; connect button `ktmConnectVciButton`; disconnect button `ktmDisconnectVciButton`; state label `ktmSessionStateLabel`; progress bar `ktmVciProgressBar`; error label `ktmSessionErrorLabel`; and voltage label `ktmVciVoltageLabel`.

- [ ] **Step 1: Scaffold testing-only CMake targets before UI RED.** Add the two test source files and register `test_KtmConnectionPage` and `test_KtmServiceWorkspace` inside the existing `if(BUILD_TESTING AND RX14_KTM_XC2)` block, without yet changing the production `rx14` graph. Reconfigure `build-ktm`, prove both target names exist, and record the expected missing-UI compile failure rather than an `unknown target` failure.
- [ ] **Step 2: RED/GREEN the offscreen connection page.** Assert install-root display/browse/save through `Xc2Settings`, start/stop, explicit lookup, approved-device selection, connect/disconnect enablement, state/error projection, and voltage rendered to one decimal plus ` V` while preserving valid `0.0`. Progress is deliberately an indeterminate busy projection derived only from `VciLookup`/the active operation; Task 5 does not add a controller progress signal or state/operation getters. Use all 15 frozen object names and prove non-provider rows cannot apply. Run the page target first to the expected behavioral/compile failure, add its UI source to the test target, implement only the passive projection page, then rerun GREEN. It never constructs URLs, endpoint names, device JSON, sidecar commands, or retry policy.
- [ ] **Step 3: RED/GREEN workspace lifecycle and action hosting.** In `test_KtmServiceWorkspace.cpp`, use a bare `QMainWindow` and `QMenu`; never instantiate the real `MainWindow`. First prove failure for modeless close-hides-with-the-same-controller-and-sidecar, application-shutdown-only teardown, lazy action creating exactly one workspace, repeat trigger reuse, and action reattachment exactly once after menu clear/retranslation. `KtmServiceWorkspace` creates its controller and immediately creates the page, fixes their initial projection at `Stopped`/`None`, owns both, and exposes no transport seam. `KtmServiceWorkspaceHost`, declared and implemented in the same `KtmServiceWorkspace.*` pair, owns the stable action and lazy workspace. Close only hides; it never stops or destroys the session. Only the host's idempotent application `shutdown()` calls `stop()`. Add the UI sources to the test target and rerun the workspace test GREEN.
- [ ] **Step 4: Keep operator controls safe.** Add RED assertions that start, lookup, connect, and disconnect remain disabled while any controller operation is active, while Stop remains enabled as the operator's explicit abort path. Failed-state recovery and every new operation require explicit operator input; there is no automatic retry or restart. Implement only this enablement projection and rerun the page test GREEN.
- [ ] **Step 5: Integrate the guarded MainWindow action.** Under `RX14_KTM_XC2`, let `MainWindow` own only the tested `KtmServiceWorkspaceHost` lifetime. Attach its stable `KTM Service` action to the Miscellaneous menu at a fixed position before Preferences and the language submenus, and reattach that same action exactly once whenever `retranslateUi()` clears/rebuilds the menu. Create the action connection only once, outside `retranslateUi()`, and keep `MainWindow` ignorant of REST/STOMP/device details.
- [ ] **Step 6: RED/GREEN the production CMake ON boundary.** Before editing the production `rx14` graph, query the ON codemodel and record that `rx14` lacks the UI sources/link/definition. Then, only inside `if(RX14_KTM_XC2)`, use `target_sources(rx14 PRIVATE ...)` for `KtmConnectionPage.*` and `KtmServiceWorkspace.*`, link `rx14_ktm_xc2` to `rx14`, and define `RX14_KTM_XC2=1`. Keep `rx14_ktm_xc2` headless and free of any Qt Widgets dependency. Keep all includes, declarations, members, and calls in `mainwindow.*` macro-guarded; rebuild `rx14` and rerun the codemodel assertion GREEN.
- [ ] **Step 7: Run GREEN ON tests fail-fast.** Register the UI tests as `ktm_connection_page` and `ktm_service_workspace`, then run `rtk proxy powershell -NoProfile -Command '$env:QT_QPA_PLATFORM="offscreen"; $env:PATH="G:\KTM\.tools\Qt\6.8.3\mingw_64\bin;G:\KTM\.tools\Qt\Tools\mingw1310_64\bin;G:\KTM\.tools\py\Scripts;"+$env:PATH; cmake --build build-ktm --target test_KtmConnectionPage test_KtmServiceWorkspace rx14 --parallel 3; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; .\build-ktm\test_KtmConnectionPage.exe; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; .\build-ktm\test_KtmServiceWorkspace.exe; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ctest --test-dir build-ktm -R "ktm_(connection_page|service_workspace|session_controller)" --output-on-failure; exit $LASTEXITCODE'`; expect the UI/controller suite and application link to pass.
- [ ] **Step 8: Prove the OFF graph.** Configure a fresh OFF build with CMake file-api queries, then assert `RX14_KTM_XC2:BOOL=OFF`, no `Qt6Test_DIR`/`Qt6WebSockets_DIR`, no target matching `(ktm|xc2)`, and no `src/ktm` entry in the `rx14` codemodel source list. Run a full OFF build; do not rely only on target-name matching.
- [ ] **Step 9: Verify scope and commit.** Run `rtk proxy powershell -NoProfile -Command 'git diff --check; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; git status --short'`, stage only Task 5 files, and commit with `rtk git commit -m "feat: add KTM VCI connection workspace"`.

### Task 6: Run final verification and record evidence without overstating HIL

**Files:**
- Modify: `docs/superpowers/plans/2026-07-18-ktm-xc2-vci-connection-plan.md`
- Modify: `docs/superpowers/specs/2026-07-16-ktm-xc2-integration-design.md`
- Modify only if required by verified behavior: `.github/workflows/ktm-xc2-foundation.yml`
- Modify ignored ledger: `.superpowers/sdd/progress.md`

- [ ] **Step 1: Run the complete Windows KTM suite from a clean configure.** Configure `RX14_KTM_XC2=ON` and `BUILD_TESTING=ON`, build all targets, and run unfiltered CTest. Record the exact test count, failures, duration, compiler, Qt version, and commit SHA; no real XC2/JAR/VCI process is part of this suite.
- [ ] **Step 2: Run the complete Windows OFF proof.** Configure a separate fresh build with `RX14_KTM_XC2=OFF`, issue file-api codemodel/cache queries, verify no Qt Test/WebSockets discovery, no KTM/XC2 target, and no `src/ktm` source in `rx14`, then build all targets.
- [ ] **Step 3: Perform hygiene and provenance checks.** Run `rtk proxy powershell -NoProfile -Command 'git diff --check; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; git status --short; git ls-files | rg -i "(xc2_backend|\.jar$|\.dll$|firmware|credential|secret)"'`; adjudicate every match and confirm no vendor binary, firmware, credential, or misleading live-capture claim entered the branch.
- [ ] **Step 4: Commit the reviewed implementation evidence.** Update completed checkboxes and the ignored SDD ledger only after task reviews are clean. Stage the tracked plan/design evidence, commit with `rtk git commit -m "docs: record XC2 VCI connection verification"`, and push the feature branch to `fork` so the workflow run is tied to an exact commit SHA.
- [ ] **Step 5: Require the exact three-job GitHub Actions result.** Query the push run whose `headSha` exactly equals `rtk git rev-parse HEAD`. Require exactly `Windows / KTM ON`, `Linux / KTM OFF`, and `macOS / KTM OFF`, all completed/success. Linux keeps `cmake --build build --parallel`; macOS uses a 45-minute timeout and `cmake --build build --parallel 3`. Record run/job URLs and decisive test/OFF-proof lines; never substitute a nearby SHA.
- [ ] **Step 6: Preserve the eight HIL unknowns as open evidence items.** Record without claiming resolution: actual per-route 200/204/body behavior; device DTO nullability/stability; actual provider enumeration string; progress ordering/replay/loss; raw voltage unit/update rate; apply-to-connected/valid-voltage timing; close-to-disconnected/selected-clear timing; and real HTTP/D-PDU/device-disconnect error behavior.
- [ ] **Step 7: Run a fresh whole-slice review.** Review the range from the Phase 1 evidence commit through the exact final SHA for contract fidelity, same-authority/cookie isolation, state/epoch/generation safety, UI lifecycle, OFF graph isolation, tests, and evidence accuracy. Fix every Critical/Important finding with a failing regression test and re-review before marking this plan complete.

## Completion Gate

- [ ] Typed REST/STOMP tests reproduce the frozen static XC2 contract without exposing arbitrary transport access.
- [ ] The controller reaches `VciReady` only from apply success plus same-ID selected confirmation plus current-generation/current-epoch `connected:true`, in either arrival order.
- [ ] Progress-before-accept and XC2-compatible `CANCELED` lookup behavior are tested end to end.
- [ ] Wrong-provider/forged selection produces zero network writes; visibility loss revokes readiness and causes zero hidden retry.
- [ ] The modeless workspace supports install-root configuration, explicit discover/connect/disconnect, and correct voltage display.
- [ ] Windows KTM ON and the fresh OFF graph proof pass locally.
- [ ] The exact final SHA has the required successful Windows/Linux/macOS CI jobs.
- [ ] The eight VCI2K/ECU HIL unknowns remain explicitly open until tested on hardware.
