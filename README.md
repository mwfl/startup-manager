# MWFL Startup Manager

MWFL Startup Manager is a safety-focused native Windows utility for inspecting
and managing applications that start automatically.

The first public preview is implemented. It starts with ordinary user rights;
machine-wide entries and scheduled tasks become writable only after an explicit
**Restart as administrator** action. See [docs/DESIGN.md](docs/DESIGN.md).

## MVP

- Discover startup entries from the current-user and all-users Startup folders.
- Discover 32-bit and 64-bit `Run` registry values for the current user and local machine.
- Discover logon-triggered Task Scheduler entries recursively.
- Enable or disable an entry without losing the information needed to restore it.
- Add a current-user `Run` value after validating the selected executable.
- Delete an entry only after confirmation.
- Show source, scope, command, and target status.

## Safety principles

- Read-only discovery does not require elevation.
- Machine-wide changes require an explicit administrator restart.
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
Standalone builds fetch the pinned MWFL `v0.1.0` release; the default local
development preset uses the neighboring mwfl checkout.

## Download and usage

GitHub Releases contain a portable ZIP. Extract it and run
`startup-manager.exe`; no installer or background service is added. The app
starts without elevation. Use **Restart as administrator** only when a selected
machine-wide entry must be changed.

Before changing an unfamiliar entry, inspect its exact command and vendor
documentation. Disabling is safer than deleting.

## Roadmap

- Narrowly scoped one-operation elevated helper to replace the elevated session mode.
- Authenticode publisher details and durable operation history.
- Startup-folder shortcut creation with separate arguments and working directory.

## Updates and Portable releases

The app checks the latest stable GitHub Release at most once per day. Use **Settings > Automatically Check for Updates** to disable or re-enable checks, or **Check for Updates** to run one manually. An available update can open the official Portable release, be deferred for three days or one week, or dismissed until the next day. Tag releases publish a versioned `windows-x64-portable.zip` plus a SHA-256 checksum; replacement is always an explicit download-and-extract action.
