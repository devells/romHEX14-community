# KTM XC2 Diagnostics and Flashing Integration Design

**Status:** Approved  
**Date:** 2026-07-16  
**Target:** romHEX 14 Community Edition on Windows

## 1. Summary

romHEX14 will add a native Qt KTM service workspace that provides the diagnostic
and firmware flashing capabilities exposed by the locally installed KTM XC2
backend. The XC2 Electron executable and legacy React UI will not run.

The implementation will retain the existing 32-bit Java backend as a local
sidecar process. That process continues to own the generated KTM ECU
definitions, DIFLOW workflows, security access logic, BaseCom stack, and AVL
D-PDU provider. The 64-bit romHEX14 process communicates with it through the
existing localhost REST and STOMP/WebSocket contracts.

This process boundary is mandatory. The AVL provider
`PDUAPI_AVLDitest.dll` is 32-bit and cannot be loaded into a 64-bit romHEX14
process.

## 2. Goals

The completed integration must provide, from a native Qt UI:

- XC2 sidecar discovery, launch, health monitoring, logging, and shutdown.
- AVL DiTEST VCI2K discovery and connection through D-PDU API.
- Automatic vehicle detection and manual vehicle selection fallback.
- Vehicle information and ECU autoscan.
- ECU identification, DTC display, and confirmed DTC clear. Freeze-frame is
  deferred until a callable route and complete DTO are evidenced.
- Live measurement selection, streaming, plotting, and clean stop.
- ECU functions, actuators, learning, coding, and guided diagnostic workflows.
- Automatic mapping selection and automatic firmware flashing.
- Custom/file firmware selection and flashing.
- Fail-closed compatibility checks, operation journaling, and interruption
  handling.
- Contract tests against the XC2 mock backend and staged VCI2K bench tests.

## 3. Non-goals

The first complete release will not:

- Start or embed `XC2.exe`.
- Embed the XC2 React application through Qt WebEngine.
- Reimplement D-PDU, UDS, KWP2000, CCP, security algorithms, or DIFLOW in C++.
- Support J2534 or non-AVL interfaces.
- Support macOS or Linux for the KTM service module.
- Integrate DealerNet, technical documentation, PowerParts, VR, or the XC2
  application update service.
- Promise full ECU Flash ROM readback. The examined XC2 contract supports
  diagnostic reads and firmware programming, but no complete ROM dump flow was
  identified.
- Claim generic resume or recovery after an interrupted erase or transfer.

## 4. Confirmed Constraints

The examined XC2 package establishes these implementation constraints:

- The original frontend is a 64-bit Electron application.
- The backend runs on a bundled 32-bit Java 8 runtime.
- The backend exposes REST under `/xc2/1.0` and STOMP/WebSocket under
  `/xc2-websocket`.
- The backend loads the 32-bit AVL D-PDU provider through JNA and BaseCom.
- D-PDU provider discovery uses the 32-bit registry root at
  `HKLM\\SOFTWARE\\Wow6432Node\\D-PDU API\\Root File`.
- The provider connects to AVL VCI2K and has confirmed CAN/ISO-TP support.
- KTM ECU definitions and DIFLOW workflows are generated Java resources in the
  backend, not ODX/PDX or a database that romHEX14 should reinterpret.
- ECU-specific programming flows differ. A single hard-coded UDS flashing
  sequence cannot replace them.

The known local compatibility profile is:

- `xc2_backend_patched.jar` SHA-256:
  `B64A38C47F74D02145F462CDDA16EEA421A2170146155602575A1BD8B7E62840`
- `PDUAPI_AVLDitest.dll` SHA-256:
  `3F790B47D3F968FE2E757F309BFFDC0448C2FCE8A488DFA7C00F1FEDCB2A3075`

These artifacts remain external installation dependencies and are not added to
the GPL repository. Additional backend versions require an explicit contract
profile and test run before state-changing operations are enabled.

## 5. Architecture

```text
romHEX14 x64
  KtmServiceWorkspace
    KtmSessionController
      Xc2RestClient -----------+
      Xc2StompClient ----------+--> XC2 Java sidecar x86
      KtmJobRegistry ----------+      KTM definitions / DIFLOW
      KtmFlashCoordinator -----+      BaseCom / D-PDU
      KtmOperationJournal -----+      PDUAPI_AVLDitest.dll x86
                                             |
                                        AVL VCI2K
                                             |
                                         KTM ECUs
```

No XC2 native library crosses the process boundary. JSON, STOMP frames, log
files, and explicitly selected firmware paths are the only shared artifacts.

## 6. Components

### 6.1 `Xc2BackendManager`

Responsibilities:

- Store a configurable XC2 installation root in `AppConfig`.
- Accept only an installation-root string through `startProduction()`, then run
  a fresh production inspection internally immediately before launch. Public
  prerequisite reports, layouts, mutable launch specs, programs, argv callbacks,
  candidate allocators, and lock paths never have production spawn authority.
- Locate the bundled x86 Java executable, approved production sidecar JAR,
  configuration, and D-PDU metadata; verify file existence, PE bitness,
  SHA-256 compatibility, registry values, provider paths, and working paths.
- Reserve a free loopback port and start the sidecar with `QProcess`.
- Pass `server.address=127.0.0.1`, the selected port, logging configuration,
  production main class, dynamic callback/GRIPS URLs, and local profile as unique
  JVM properties before `-jar`. Start the absolute x86 Java path directly, use
  the XC2 application root as working directory, and remove inherited Java/Spring
  option-injection environment variables.
- Set the XC2 application directory as the process working directory so its
  relative resources resolve consistently.
- Poll `/xc2/1.0/serviceStatus/status` until the backend reports `alive` or a
  bounded startup deadline expires. Construct the canonical fully encoded
  loopback REST base internally and pass encoded bytes, never a caller-owned
  mutable URL, into the REST client.
- Require a non-blank strict current-user identity before Ready, retain its exact
  permission list, and treat an empty list as Ready but unauthorized for every
  later operation. Ready does not claim DealerNet authentication.
- Capture bounded independently fragmented stdout/stderr, exit code, sanitized
  command metadata, and the backend log path.
- Hold a stable Windows process HANDLE and exact IPv4 listener PID proof. Before
  a shutdown POST, connect a dedicated no-proxy socket and, before writing any
  byte, prove the server side of that same established four-tuple is owned by the
  still-live captured process. Obtain Cookie bytes only through the REST
  client's exact shutdown-target export; its WebSocket-only export never
  authorizes shutdown. Otherwise write zero bytes and terminate/kill only the
  owned process.

The manager will not reuse or select port 8082. That service may be the mock
backend or an incompatible XC2 instance. A port reservation cannot be handed to
Java atomically, so every attempt has a distinct identity and the manager retries
at most three candidates only after proving a wrong-PID collision and fully
reaping the failed child. It never terminates an external process.

A fixed absolute per-user `QLockFile` under `AppLocalDataLocation`, configured
with stale-lock time zero, prevents two romHEX14 instances from claiming VCI2K.
It is acquired before candidate selection and retained until the actual child is
NotRunning and its stable HANDLE is signaled, including collision, stop, manager
destruction, and asynchronous reaping. Dead-PID stale locks are recoverable;
live owners are never removed because of lock age. A non-cooperating external
XC2 process may still hold D-PDU; that condition is reported as `VciError`, not
resolved by killing the process.

Each run and each collision attempt have immutable identities. Canceling startup
revokes only the ability to become Ready and network authorization; it never
invalidates the callbacks required to drain output, observe `finished`, reap the
child, and release the lock. Stop mutates those gates and removes pending request
IDs before aborting an asynchronous REST reply, so synchronous abort completion
cannot revive a stale run. Failed cleanup always closes through `Stopped`; a new
start is rejected until then. Every accepted start clears the previous run's
sanitized output rings and pending partials. `recentOutput()` then exposes only
the active run and retains that just-finished run while `Stopped`, until the next
accepted start. An accepted run that never starts a child reports its sole
terminal result as `stopped(-1, QProcess::CrashExit)` after cleanup and lock
release.

Manager destruction transfers the complete process/HANDLE/job/lock context to an
application-lifetime asynchronous reaper and never waits, calls `processEvents`,
or enters a nested event loop on the UI thread. For coordinated normal Quit, one
application event filter consumes the first and repeated `QEvent::Quit` events
idempotently and starts the bounded asynchronous terminate-then-kill sequence.
The helper process and `QLockFile` stay live until `QProcess` is NotRunning, the
stable child HANDLE is signaled, and output is finalized; only then does the
reaper release the lock and repost exactly one Quit event.

The same fully qualified `Xc2BackendManagerTestAccess` friend may be defined
independently in `test_Xc2BackendManager.cpp` and
`xc2_manager_exit_helper.cpp`, which are separate test executables. The helper
uses that private seam to launch the real manager with the fake sidecar,
publishes its PID and lock path, destroys the manager to transfer the live
context into the real reaper, and then requests quit. It does not copy launch,
Job, lock, or reaping logic.

Every owned child also belongs to a Windows Job Object with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. A forced `QCoreApplication::exit`, owner
crash, or OS termination promises only Job Object child-tree containment when
no further Qt events can run. There is no strict lock-release ordering after the
owner is dead; crash leftovers are recoverable dead-owner stale locks. Preserving
such ordering would require an external guardian process and is outside scope.

### 6.2 `Xc2RestClient`

`Xc2RestClient` deliberately does not use `QNetworkAccessManager`. Qt 6.8's
private HTTP channel can reconnect up to twice and a live close-before-status
fake proved that it transparently resends a complete GET or POST; Qt exposes no
public no-retry switch. Each XC2 operation therefore creates one fresh
`QTcpSocket`, sets `QNetworkProxy::NoProxy`, connects to one canonical numeric
loopback `QHostAddress`, writes one HTTP/1.1 request, and never reconnects,
retries, redirects, reuses a connection, or resends request bytes.

The base configuration boundary accepts the original visible-ASCII URL bytes,
not a caller-constructed `QUrl`. This is required because `QUrl` irreversibly
canonicalizes `127.1` and integer/octal IPv4 aliases to `127.0.0.1` and decodes
unreserved escapes such as `%78` and `%2e`. The client strictly parses the raw
scheme, authority, canonical explicit port, and exact `/xc2/1.0[/]` path before
constructing internal canonical REST and WebSocket URLs. A rebase is rejected
while any socket is pending and preserves the current authority and cookie jar;
a successful cross-authority rebase clears that jar. Two narrow cookie exports
accept encoded bytes only when they equal their internally derived target's
fully encoded bytes exactly: `cookieHeaderFor()` is WebSocket-only, while
`shutdownCookieHeaderFor()` is bound to the shutdown REST endpoint. There is no
generic URL cookie-export surface.

Requests carry an explicit-port `Host` header with IPv6 brackets when needed,
`Connection: close`, `Accept-Encoding: identity`, and manually selected REST
cookies. The client owns a dedicated `QNetworkCookieJar` and manually installs
every separate `Set-Cookie` only after a syntactically complete response, then
uses the same jar for later REST Cookie headers and the two target-bound exports.
For shutdown, default-path cookies set by service-status responses, cookies with
`Path=/xc2/1.0`, and root cookies match; cookies scoped only to
`/xc2-websocket` do not. A protocol-failed response installs no cookie visible
through either export.

An incremental bounded HTTP/1.1 parser handles fragmented headers,
`Content-Length`, chunked bodies plus trailers, 204/no-body, and
close-delimited bodies. It rejects ambiguous `Transfer-Encoding` plus
`Content-Length`, malformed or truncated chunks, invalid framing, headers or
trailers above 64 KiB, and decoded bodies above 8 MiB. Independent total and
inactivity timers, caller abort, socket events, and parser failures converge on
one socket-pending `completeOnce` gate, so every request emits exactly one typed
asynchronous result.

The public surface is grouped by domain rather than exposing arbitrary request
URLs:

- Service/session: health, current user, settings, shutdown.
- VCI: lookup, available devices, selected device, selection.
- Vehicle: detect, manufacturer/series/model selection, vehicle information.
- ECU: domains, open, close, scan, identifiers, measurements.
- Jobs: autoscan, DTC clear, flow execution, mapping download, flashing.
- Flow: submit the user's response to the currently displayed flow page.

The stale served frontend/profile mentions `GET ecu/getFunctions/{ecuId}`, but
the pinned patched backend JAR exposes no matching JAX endpoint. That dead route
is not part of the callable REST profile. `EcuDomain` supplies function and
actuator metadata, and execution uses the DIFLOW flow routes.

Every response is validated before conversion to a C++ model. A successful HTTP
status with missing required fields is a contract error. State-changing calls
have an idempotency classification and are never automatically repeated; the
Task 5 foundation transport itself automatically repeats no request of any
class.

### 6.3 `Xc2StompClient`

The STOMP client uses Qt WebSockets and implements the subset required by XC2:
CONNECT, CONNECTED, SUBSCRIBE, MESSAGE, RECEIPT, ERROR, heartbeat, UNSUBSCRIBE,
and DISCONNECT. It subscribes after the REST session exists. The WebSocket URL
and Cookie header are derived together from the validated REST session; the
transport cannot be given an unrelated authority or caller-built Cookie header.
The client offers and requires WebSocket subprotocol `v12.stomp`, then requires
STOMP `version:1.2` before treating the session as connected.

Required topics include:

- `/topic/vci/status`
- `/topic/vehicleinfo`
- `/topic/ecu`
- `/topic/progress`
- `/topic/measurementValues`
- `/topic/flowGUI`
- `/topic/flowProgress`
- `/topic/login`

Unknown messages are logged with their topic and schema version but are not
silently accepted. `Xc2StompClient` never reconnects itself. It emits a generic
visibility-loss event only when an established session is lost unexpectedly.
The session controller may initiate a fresh connection generation only when no
destructive job is active. During a flash, it propagates that event to the job
registry as degraded visibility and does not restart the backend, reconnect the
transport, or resubmit the job. An intentional disconnect sends a generation-
unique receipt request, waits for the matching RECEIPT, and force-aborts after a
bounded graceful-disconnect deadline. Its owner can also force-abort the current
generation immediately to enforce a wider REST-plus-WebSocket operation
deadline; graceful success, graceful timeout, and caller cancellation remain
distinguishable outcomes.

### 6.4 `Xc2JobRegistry`

REST operations that start work return a `jobID`. `Xc2JobRegistry` creates one
record per exact job ID and correlates subsequent `/topic/progress` messages
without guessing from the number or order of active jobs. Each accepted event
retains its Task 7 connection generation, STOMP `message-id`, strict decoded
progress model, and raw JSON. The pair `(generation, message-id)` is the delivery
identity: an identical replay is idempotent, while reuse of that identity with a
different payload is a contract error. Equal progress payloads delivered under
different identities remain distinct ordered events.

The authoritative progress wire object is
`{jobId:string,status:status,ticks:int32,totalTicks:int32,message:{id:int64,text:string}|null}`.
`message` is required even when null. Non-null localized text requires an exact
signed 64-bit integer ID and string text; legacy string messages, numeric
overflow, aliases, wrong casing, and wrong types are contract failures whose raw
payload is retained. The only wire statuses are `IN_PROGRESS`, `FINISHED`,
`CANCELED`, `ERROR`, and `NOT_AUTHORIZED`; `Created` remains registry-internal.

The normalized backend states are:

- `Created`
- `InProgress`
- `Finished`
- `Canceled`
- `Error`
- `NotAuthorized`

`VisibilityLost` is not a backend state and never replaces one of those values.
It is an orthogonal record flag set when an established STOMP generation is
lost. Repeating the same loss is idempotent. Only a valid authoritative event
from a later generation clears the flag for that event's exact job ID; it does
not clear other jobs or synthesize progress. Terminal backend states are
immutable regardless of later transport activity.

Registry mutations are validate-then-commit and emit value snapshots exactly
once. A new REST acceptance creates `Created`; an existing acceptance is an
idempotent replay and cannot reset progress. A valid first progress event may
create an unknown job without losing that event. Progress may move `Created` to
`InProgress` or directly terminal, and `InProgress` to another non-regressive
`InProgress` event or terminal. Clearing a terminal record leaves a session-
lifetime tombstone so a late acceptance or progress message cannot recreate it.
Active records and visibility-change signals use stable first-observed order.

Only one diagnostic or programming job may own the VCI at a time. Measurement
streaming also counts as an active VCI operation until explicitly stopped. That
ownership gate belongs to `KtmSessionController`; the registry records facts and
does not infer operation ownership from job order.

### 6.5 `KtmSessionController`

The controller is the single owner of UI-visible workflow state:

```text
Unavailable
  -> BackendStarting
  -> BackendReady
  -> SessionReady
  -> VciReady
  -> VehicleSelected
  -> EcuAvailable
  -> JobRunning
```

It guards every command, closes open ECU sessions when leaving a vehicle, stops
measurements before disconnecting, and prevents concurrent workflows.

The sidecar must report a current local user with the permissions required by
the requested operation. The Qt client does not implement an authentication
bypass. If the configured patched sidecar cannot establish its approved local
session, state-changing features remain disabled and the failure is shown.

### 6.6 `KtmServiceWorkspace`

romHEX14 adds a `KTM Service` action and a native Qt workspace with these pages:

1. Connection and prerequisites.
2. Vehicle detection/selection and vehicle information.
3. ECU autoscan and ECU overview.
4. ECU identification and DTC data; freeze-frame remains excluded until its
   callable route and complete DTO are evidenced.
5. Live measurements and plots.
6. ECU functions and guided workflows.
7. Automatic and file flashing.
8. Operation history and diagnostic logs.

The workspace shares the application's theme and translation infrastructure but
does not couple diagnostic state to `MainWindow` internals. `MainWindow` owns
only the action and workspace lifetime.

### 6.7 `KtmFlowRenderer`

`KtmFlowRenderer` translates `/topic/flowGUI` descriptions into native Qt
controls. Supported control families are text/instructions, progress,
single-choice, multi-choice, numeric/text input, confirmation, and navigation
buttons. User responses are submitted through `flow/updateGui` with the active
job and page identity.

The renderer does not infer a default response for an unknown required control.
An unsupported schema pauses the flow, preserves the raw payload in the log,
and disables submission. This prevents an accidental actuator, coding, or
programming choice.

### 6.8 `KtmFlashCoordinator`

The coordinator performs UI-level validation and delegates all ECU-specific
programming to DIFLOW. It never sends UDS/KWP/CCP services directly.

For automatic flashing it requires a selected vehicle and identified ECU, then
requests mapping resolution/download and displays the backend-provided current
and target mapping information before confirmation.

For file flashing it creates a `FlashArtifactSnapshot` containing:

- Absolute immutable file path.
- SHA-256, byte length, and detected container type.
- Source romHEX14 project/version when applicable.
- Preserved address segments for HEX/S19 exports.
- Sidecar-returned `FlashInfo` and compatibility result.

Opaque formats such as KFW, PRM/EDT, encrypted S-records, and encrypted HEX are
passed unchanged only when the known XC2 backend explicitly accepts them for
the selected ECU. Otherwise they fail closed.

`Flash current project` is enabled only when the project retains a verified
source address map and its exporter can round-trip the required container.
Sparse HEX/S19 content must not be flattened to a zero-based BIN. When that
requirement cannot be proven, the user must export and select an approved
supplier artifact instead.

### 6.9 `KtmOperationJournal`

Before any state-changing operation starts, an atomic journal is created with
`QSaveFile`. A flash journal records:

- Timestamp and romHEX14/backend versions.
- Vehicle VIN/article number and selected ECU domain.
- ECU identifiers, current mapping, and target mapping.
- VCI identity and D-PDU provider identity.
- Firmware path, hash, size, and compatibility response.
- Preflight confirmations.
- XC2 `jobID` and every ordered progress/flow event.
- Final state or the last observed state before interruption.

The journal is diagnostic evidence, not a generic resume token.

## 7. Diagnostic Data Flow

### 7.1 Startup

1. Open the KTM workspace.
2. Validate sidecar and D-PDU prerequisites.
3. Start the approved sidecar on a private loopback port.
4. Wait for health readiness.
5. Establish REST session and verify current-user permissions.
6. Connect STOMP and initially subscribe only to `/topic/vci/status` and
   `/topic/login`; later controllers add other approved topics when their
   workflow becomes active.
7. Discover and select VCI2K.
8. Enable vehicle operations only after `VciReady`.

### 7.2 Vehicle and ECU

Vehicle detection and autoscan are asynchronous jobs. The REST response supplies
the `jobID`; progress and resulting vehicle/ECU changes arrive over STOMP. A
manual manufacturer/series/model selection is available when detection fails.

The ECU overview groups domains such as engine, ABS, dashboard, immobilizer,
lighting, suspension, tire pressure, and battery management. Only domains
reported by the sidecar for the selected vehicle are displayed.

### 7.3 ECU Diagnostics

Opening an ECU is explicit. Identification and DTC reads update an immutable
snapshot used by the UI. DTC clear requires a second confirmation that names the
vehicle and ECU; it is never included in a read/refresh operation.

The currently evidenced freeze-frame DTO cannot be consumed safely by this
strict profile. Freeze-frame UI remains unavailable until a callable pinned-JAR
route and its complete DTO are evidenced; the stale served frontend alone does
not authorize a guessed parser or request.

Measurement configuration is loaded from the sidecar for the chosen ECU. Start
and stop are explicit operations. Values are correlated by signal identity and
timestamp, then forwarded to the existing plotting concepts where practical.
Closing the page stops measurement streaming before closing the ECU.

ECU functions, actuators, coding, learning, and guided diagnostics all use the
same DIFLOW job/renderer path. The Qt layer does not copy service bytes, scaling
rules, seeds, keys, or ECU-specific conditions from generated definitions.

## 8. Flashing Flow

### 8.1 Preflight

Programming is enabled only after all of these are true:

- Production sidecar contract and artifact hashes are approved.
- Local session has the required flash permission.
- VCI2K is connected through USB and D-PDU reports it ready.
- Vehicle and target ECU are identified.
- Article number, ECU domain, hardware/software identifiers, current mapping,
  and target mapping satisfy the sidecar compatibility check.
- No other VCI job or measurement stream is active.
- The sidecar's voltage condition passes.
- The user confirms charger, VCI power, ignition, and electrical-load
  prerequisites presented by the workflow.

The voltage threshold is owned by the ECU/backend workflow. romHEX14 will not
invent one when the backend does not provide it; absence of a required voltage
result blocks programming.

### 8.2 Execution

The UI presents normalized observational stages:

```text
Validating
  -> AwaitingConfirmation
  -> Preparing
  -> SecurityAccess
  -> Erasing
  -> Downloading
  -> Verifying
  -> Resetting
  -> PostScan
  -> Completed
```

These labels do not drive protocol actions. The ECU-specific DIFLOW remains
authoritative and may use UDS, KWP2000, CCP, different security levels, or
additional steps.

### 8.3 Cancellation and Interruption

- A cancel control is shown only when the backend flow explicitly offers it.
- Cancel never means terminating the Java process.
- romHEX14 exit and system sleep are inhibited while a flash is active.
- Closing the workspace cannot destroy its active controller or sidecar.
- REST state-changing requests are never automatically resubmitted.
- Sidecar or VCI loss after programming starts produces `FlashCriticalError`.
- The backend is not automatically restarted during a flash.
- The UI reports `State unknown` when completion cannot be proven.
- Recovery requires an ECU-specific, explicitly available recovery flow. There
  is no generic automatic retry after erase.

After a successful reset, a new ECU scan verifies communication and mapping.
Post-scan failure is reported separately from transfer success.

## 9. Error Model

Errors retain the backend message, HTTP status, XC2 code, job ID, ECU context,
and nested D-PDU information where supplied. UI-facing categories are:

- `PrerequisiteError`
- `BackendError`
- `ContractError`
- `SessionError`
- `VciError`
- `VehicleError`
- `JobError`
- `FlashCriticalError`

Health checks and read-only GET operations may use bounded retry with backoff.
Vehicle selection, DTC clear, flow submission, actuator/coding operations, and
all flash operations have zero automatic retries.

## 10. Build and Configuration

The feature is compiled on Windows behind `RX14_KTM_XC2`, which defaults to ON
on Windows and OFF elsewhere. Qt WebSockets is a required component when the
option is enabled; configuration fails with a direct prerequisite message when
it is missing. Non-Windows builds retain the rest of romHEX14 and omit the KTM
service action.

Configuration stores only paths and user preferences. Firmware, XC2 JARs,
vendor DLLs, credentials, and dealer data are not copied into the romHEX14
repository or project format.

## 11. Testing

### 11.1 Unit Tests

- REST request/response models and schema rejection.
- STOMP framing, heartbeat, topic routing, and job correlation.
- Backend port selection, startup timeout, process ownership, crash, and clean
  shutdown using a stub process.
- Session-controller guards and cleanup ordering.
- Idempotency rules proving state-changing calls are not retried.
- Flow-renderer controls, unknown-schema blocking, and response serialization.
- Firmware hash/snapshot behavior and sparse HEX/S19 address preservation.
- Compatibility, voltage, permission, and concurrent-job gates.
- Atomic journal updates and interrupted journal recovery display.

### 11.2 Contract Tests

The original XC2 mock JAR is used as an external test fixture to validate:

- REST routes and JSON models.
- Login/current-user permission behavior.
- Vehicle, ECU, job, flow, and measurement event schemas.
- STOMP topic names and terminal job statuses.

Sanitized golden JSON/STOMP fixtures are committed for deterministic CI. The
external JAR is not committed. Fixture provenance is per artifact: the
CONNECTED frame is an observed read-only handshake, while progress JSON/MESSAGE
and the ERROR frame are synthetic contract/protocol fixtures. Progress schema
authority is the served app plus pinned patched backend JAR, `liveCapture` is
false, and no manifest-wide date may imply that a progress MESSAGE was captured.

An optional live foundation probe never starts Java or a JAR and never uses the
backend manager. It accepts an explicit loopback REST base, performs only
`GET serviceStatus/status`, `GET auth/currentUser`, an authority-bound
`v12.stomp` connection using the REST cookie, subscriptions to
`/topic/vci/status` and `/topic/login`, and a receipt-confirmed DISCONNECT. It
uses one wall-clock deadline across REST and WebSocket work and requires the
case-sensitive `EcuDiagnosticRead` permission. Its success means only that this
read-only foundation surface is compatible; it does not approve artifact
provenance or enable device, vehicle, ECU, flow, or flash operations.

Probe exits are deterministic: 0 for that complete read-only sequence, 2 for
CLI/URL/REST transport or backend availability, 3 for REST schema/contract, 4
for WebSocket/STOMP/receipt failure, and 5 for the exact unauthenticated or
missing-permission cases. Task 5 still classifies an empty current-user HTTP 204
as `Contract`; only the probe's endpoint-aware environment mapping converts that
exact response to 5. Malformed authentication errors remain contract failures.

CI invokes the real probe executable as a child against one loopback fake
authority serving both REST and WebSocket. The fake asserts the exact request,
topic, Cookie, subprotocol, and frame allowlists, zero state-changing or
unexpected calls, bounded cleanup, sanitized output, and every documented exit
code. CI never starts or downloads the external XC2 JAR.

### 11.3 UI Tests

A fake `IXc2Client` drives every page without hardware. Tests verify navigation,
guards, confirmations, job ownership, measurement cleanup, unknown flow controls,
and prevention of application exit during a flash.

### 11.4 Hardware-in-the-Loop Tests

Hardware validation advances in this order:

1. VCI2K discovery with no vehicle.
2. Bench ECU identification, DTC read, and measurements.
3. Non-destructive DIFLOW functions.
4. Mismatched firmware and low-voltage rejection with proof that no destructive
   request begins.
5. Automatic and file flashing on a recoverable bench ECU.
6. Injected USB disconnect, timeout, negative response, transfer failure, and
   verification failure at controlled stages.
7. Post-reset ECU rescan and mapping verification.

Real flashing is controlled by a compatibility registry keyed by backend
profile, ECU family/domain, and mapping family. A registry entry is enabled only
after a recorded successful bench run and the applicable failure-injection
evidence. Unknown combinations retain diagnostics but fail closed for flashing.

## 12. Implementation Phases

1. **Contract foundation:** freeze the approved sidecar profile, capture mock
   fixtures, and implement core models.
2. **Process and transport:** backend manager, REST client, STOMP client, job
   registry, and connection page.
3. **Read diagnostics:** vehicle detection, autoscan, ECU identifiers, DTC, and
   measurements.
4. **Dynamic workflows:** flow renderer, functions, actuators, learning, coding,
   and guided diagnostics.
5. **Flash preparation:** mapping resolution, artifact snapshots, compatibility
   gates, journal, and dry-run UI.
6. **Bench programming:** automatic/file flash integration and VCI2K fault
   injection.
7. **Release hardening:** regression, translations, packaging checks, and
   per-ECU-family compatibility records.

Each phase leaves the application in a testable state. Destructive controls
remain disabled until Phase 6 evidence is complete.

## 13. Acceptance Criteria

The design is complete when all of these are demonstrated:

- romHEX14 launches and owns the approved sidecar without starting `XC2.exe`.
- Communication is loopback-only and no x86 vendor DLL loads into romHEX14.
- VCI2K discovery and status are visible in the Qt workspace.
- A KTM vehicle can be detected or selected and autoscan populates its ECUs.
- ECU identification, DTC, live values, and dynamic DIFLOW functions work from
  the native UI.
- Automatic and compatible file flashing complete on a supported bench ECU.
- Mismatched ECU/firmware, missing permissions, missing voltage proof, and
  concurrent jobs are blocked before destructive work.
- An interrupted flash is journaled and reported as unknown without automatic
  retry or false success.
- `XC2.exe`, QtWebEngine, J2534, and full ROM readback are not required or
  advertised.
