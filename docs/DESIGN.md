# Startup Manager design

Status: design baseline and implementation roadmap, 2026-08-17

## Product boundary

The application manages user-facing auto-start mechanisms. The MVP includes:

| Source | Inspect | Disable/enable | Delete | Add |
|---|---:|---:|---:|---:|
| `HKCU/HKLM ...\\CurrentVersion\\Run` | yes | yes | yes | yes |
| 32-bit and 64-bit registry views | yes | yes | yes | yes |
| Per-user/all-users Startup folders | yes | yes | yes | shortcut |
| Task Scheduler logon tasks | planned | planned | planned | later |

The MVP excludes services, drivers, browser extensions, AppX background tasks,
Winlogon shell values, Group Policy, and undocumented startup locations. Those
areas have materially different security and recovery risks.

## User experience

The main window is a single table with search and source/scope/status filters.
Columns are Name, Publisher, Status, Impact, Source, Scope, and Command. Selecting
an entry opens a details pane containing the exact location, parsed executable,
arguments, signature state, last modification time when available, and recovery
information.

Primary actions:

1. **Disable** stores enough source-specific metadata to restore the entry and
   then performs the least-destructive native operation.
2. **Enable** restores only an entry previously disabled by this application,
   unless the source exposes a native enabled flag such as Task Scheduler.
3. **Delete** shows the exact object being removed, creates a recovery record,
   then asks for confirmation.
4. **Add** accepts a display name, executable, arguments, working directory,
   scope, and mechanism. The default mechanism is the current-user Startup folder.

Refresh occurs after every mutation and reports partial discovery failures in a
non-modal banner. The UI must never report success until a read-back confirms
the resulting state.

## Disable semantics

There is no universal Windows startup disable API, so each provider owns its
reversible behavior:

- **Registry Run:** move the value into the app-owned recovery store, recording
  hive, view, key, value name, registry type, and raw bytes, then remove the
  original value. Enable restores it only if the destination name is free.
- **Startup folder:** move the shortcut/file into an app-owned disabled folder
  on the same volume where possible; record the original absolute path and file
  identity. Enable refuses to overwrite a new file at the original path.
- **Task Scheduler:** use the registered task's native enabled flag. Preserve
  the previous flag in the operation log.

The preview's reversible store is source-native: disabled registry values move
to `Software\\MWFL\\StartupManager\\DisabledRun`, while startup-folder files move
under `%LOCALAPPDATA%\\MWFL\\StartupManager\\DisabledStartup`. A versioned JSON
operation journal remains planned before machine-wide mutation is enabled.

## Architecture

```text
Win32/MWFL UI
    |
Application service (refresh, validate, mutate, read-back)
    |
IStartupProvider
    +-- RegistryRunProvider (HKCU/HKLM, WOW64 views)
    +-- StartupFolderProvider (known folders, shortcuts)
    +-- ScheduledTaskProvider (Task Scheduler COM API)
    |
RecoveryStore + OperationLog + SignatureInspector
```

`StartupEntry` is an immutable discovery snapshot with a stable provider-owned
identity. Mutations accept the identity plus an expected fingerprint so stale UI
state cannot modify an entry that changed after discovery.

Providers expose `Discover`, `Disable`, `Enable`, `Delete`, and supported `Add`
operations. Each mutation returns a structured result containing the native
error, recovery-record ID, and read-back state. The application service owns
validation, elevation routing, user confirmation policy, and refresh.

## Privilege model

The main process runs as the interactive user. User-scope changes happen in
process. Machine-scope changes are sent as a single, narrowly scoped request to
an elevated helper. The request names the provider, operation, identity,
fingerprint, and recovery record; it does not accept an arbitrary command line.
The helper validates every path and registry location against an allowlist and
exits after the operation.

The helper protocol should use a versioned, length-prefixed message over a named
pipe restricted to the launching user and Administrators. The elevated process
must verify the caller and executable installation path before accepting work.

## Command and trust handling

Command lines are preserved verbatim for display and recovery. Executable
resolution follows source-specific Windows behavior and never rewrites arguments
silently. The UI flags missing targets, unquoted executable paths containing
spaces, relative paths, network paths, writable-by-standard-user locations, and
invalid signatures. These are warnings, not automatic malware judgments.

Publisher information comes from Authenticode verification using WinVerifyTrust.
Impact is initially `Unknown`; a later release may calculate measured impact from
Windows startup telemetry. The MVP must not invent impact ratings.

## Delete and add rules

Delete always creates a recovery record before mutation. A delete failure leaves
the recovery record marked `prepared`; successful read-back marks it `committed`.
The confirmation names the entry, scope, mechanism, and exact location.

Add validates that the executable is an absolute local path and exists. Arguments
remain a separate field. Machine scope requires elevation. Duplicate identities
are rejected; duplicate targets generate a warning. Startup-folder shortcuts are
created with IShellLink and IPersistFile. Registry strings default to `REG_SZ`.

## Failure and concurrency behavior

- Discovery is best-effort per provider and returns entries plus diagnostics.
- All mutations compare the discovery fingerprint immediately before writing.
- A provider lock prevents overlapping mutations within this app; native access
  controls remain authoritative across processes.
- Recovery data is durable before destructive writes.
- Post-write read-back determines success; mismatches are surfaced with recovery
  guidance and never hidden.
- The operation log contains no environment dumps or secrets and redacts user
  profile prefixes in exported diagnostics.

## Testing strategy

- Unit tests for identity, command parsing, fingerprints, recovery serialization,
  allowlists, validation, and state transitions.
- Provider integration tests use disposable registry keys, temporary folders,
  and uniquely named scheduled tasks.
- Mutation tests cover access denied, destination collision, stale fingerprints,
  interrupted recovery writes, WOW64 views, Unicode, long paths, and read-back
  mismatch.
- A Windows Sandbox manual matrix covers standard user/admin accounts, UAC,
  portable paths, unsigned binaries, network targets, and light/dark UI.

## Delivery slices

1. Domain model, recovery store, provider contracts, and fixtures.
2. Registry provider with add/disable/enable/delete and integration tests.
3. Startup-folder provider and shortcut inspection/creation.
4. Task Scheduler discovery and enable/disable/delete.
5. MWFL table/details UI, confirmations, filtering, and diagnostics.
6. Elevated helper, packaging, signing, CI, and sandbox verification.

## Decisions still open

- Product/repository name (`startup-manager` is the working name).
- Whether deletion recovery should be restorable from the UI or only from an
  exported recovery bundle in v1.
- Retention policy for committed recovery records.
- Whether Task Scheduler task creation belongs in v1.0 or v1.1.
