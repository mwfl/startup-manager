# MWFL Startup Manager

MWFL Startup Manager is a safety-focused native Windows utility for inspecting
and managing applications that start automatically.

The first public preview is implemented. It is intentionally conservative:
current-user entries can be changed directly, while machine-wide entries remain
visible but read-only. See [docs/DESIGN.md](docs/DESIGN.md) for the design and roadmap.

## MVP

- Discover startup entries from the current-user and all-users Startup folders.
- Discover 32-bit and 64-bit `Run` registry values for the current user and local machine.
- Enable or disable an entry without losing the information needed to restore it.
- Add a current-user `Run` value after validating the selected executable.
- Delete an entry only after confirmation.
- Show source, scope, command, and target status.

## Safety principles

- Read-only discovery does not require elevation.
- Machine-wide entries are inspection-only in the preview.
- Disable is reversible; delete is explicit and confirmed.
- Commands are displayed exactly and parsed conservatively before mutation.
- Microsoft and security-related entries receive an additional warning, not a
  blanket allow or deny rule.
- The application never disables Windows services or drivers.

## Proposed build

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-release
ctest --preset vs2026-x64-release
```

Visual Studio 2026 with the MSVC C++20 toolchain is the target environment.

## Download and usage

GitHub Releases contain a portable ZIP. Extract it and run
`startup-manager.exe`; no installer or background service is added. The app
starts without elevation. Machine-wide entries are shown for inspection only.

Before changing an unfamiliar entry, inspect its exact command and vendor
documentation. Disabling is safer than deleting.

## Roadmap

- Narrowly scoped elevated helper for machine-wide mutations.
- Task Scheduler logon-task discovery and native enable/disable.
- Authenticode publisher details and durable operation history.
- Startup-folder shortcut creation with separate arguments and working directory.
