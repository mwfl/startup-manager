# Security policy

Please report security issues privately through GitHub Security Advisories for
this repository. Do not include secrets, personal startup commands, or complete
environment dumps in a public issue.

The application does not classify software as safe or malicious. It displays
Windows startup configuration and performs only the explicit operation selected
by the user. Prefer disabling an entry before deleting it.

The process starts unelevated. Machine-wide mutations and scheduled-task changes
require the user to choose **Restart as administrator** and accept Windows UAC.
A narrower one-operation elevated helper is planned for a future release.
