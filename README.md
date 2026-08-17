# MWFL Startup Manager

MWFL Startup Manager is a safety-focused native Windows utility for inspecting
and managing applications that start automatically.

The repository is currently in the design/bootstrap phase. See
[docs/DESIGN.md](docs/DESIGN.md) for the approved MVP behavior and architecture.

## MVP

- Discover startup entries from the current-user and all-users Startup folders.
- Discover 32-bit and 64-bit `Run` registry values for the current user and local machine.
- Discover interactive-logon Task Scheduler entries.
- Enable or disable an entry without losing the information needed to restore it.
- Add a Startup-folder shortcut or `Run` value after validating its target and arguments.
- Delete an entry only after confirmation, with a local recovery record.
- Show source, scope, publisher/signature, command, target status, and impact hints.

## Safety principles

- Read-only discovery does not require elevation.
- Machine-wide mutations request elevation only for that operation.
- Disable is reversible; delete is explicit and backed up.
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

