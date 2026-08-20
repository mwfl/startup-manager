# Changelog

## 0.1.1 - 2026-08-20

- Reduce refresh overhead by reusing elevation and executable metadata results.
- Improve filtering allocation behavior and executable-command parsing coverage.
- Keep executable resource metadata consistent with the application version.

## 0.1.0 - 2026-08-19

- Add daily GitHub Release update checks with configurable reminders.
- Publish versioned Portable ZIP releases with SHA-256 checksums.
- Discover current-user and all-users Startup folders.
- Discover current-user and local-machine Run entries in both registry views.
- Discover logon-triggered scheduled tasks and use their native enabled state.
- Add, disable, re-enable, and delete current-user startup entries.
- Allow machine-wide mutations after an explicit administrator restart.
- Filterable native MWFL interface with full command and location details.
- Portable Windows x64 packaging and generated application icon.
