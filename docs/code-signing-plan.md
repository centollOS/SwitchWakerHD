# Signing the Windows installer (issue #61)

Branch `feature/code-signing`. The signing step stays off until SignPath Foundation accepts the project and its
settings exist in the repository; until then the workflow builds and publishes exactly as before.

## The problem

Microsoft Defender flags `SwitchWakerHD-Installer.exe` (v0.4.7) as `Trojan:Win32/Wacatac.B!ml` (#61). `!ml` is
its machine-learning heuristic, not a signature: a small unsigned .NET program that asks for Administrator,
downloads files and starts another `.exe` (the devkitPro updater) is what that model dislikes. SmartScreen also
warns on every release, since an unsigned file only gains reputation by its hash and every release has a new one.

Signing does not guarantee the heuristic stops, but it gives Windows a stable publisher: reputation then builds
on the certificate across releases instead of starting from zero with each hash. Every release must be signed
with the same certificate and a timestamp.

## The options

| Option | Cost | For us | CI |
|---|---|---|---|
| SignPath Foundation | free | needs acceptance (OSI license, "verifiable reputation") | official action, key in their HSM, manual approval per release |
| Certum Open Source (SimplySign) | ~50-70 EUR/year | personal certificate, "Open Source Developer" | session opened with a phone code: a self-hosted runner or signing by hand |
| Azure Artifact Signing | 9.99 USD/month | individuals only in the USA and Canada; EU organizations need 3 years of history | - |
| Commercial OV | 100-300 USD/year | yes | key on a token/HSM since 2023, same problem as Certum |

Choice: apply to SignPath Foundation; if refused, Certum Open Source signed by hand on Windows before uploading.
Windows then shows "SignPath Foundation" as the publisher, not centollOS.

## SignPath's conditions and where we stand

- OSI license for all components, no proprietary code: MPL-2.0 (LICENSE); vendored code is MPL/zlib/MIT
  (THIRD_PARTY.md). Nothing of the game ships: the user brings the dump.
- Sign our own code only: the installer (`installer/windows`, `installer/shared`) is this fork's own code.
- Already released in the form to be signed: the `.exe` is in every release since v0.4.1.
- Built from source on GitHub-hosted runners: `.github/workflows/release-installer.yml`.
- Product name and version enforced by the artifact configuration: `Product` = `SwitchWakerHD`, version =
  the release tag. `IncludeSourceRevisionInInformationalVersion` is off so the product version has no `+<commit>`.
- MFA for every team member on GitHub and SignPath: the centollOS organization requires 2FA (since 2026-10-21).
- Roles: authors and reviewers = the organization's members; approver = Pulpparty. Every signing request is
  approved by hand in SignPath.
- System changes announced: the installer says it installs Python, devkitPro (and, for the icon, Git, Pillow,
  MSYS2's gcc) before doing so; the build folder is `%USERPROFILE%\SwitchWakerHD-Build`.
- Privacy: the installer only talks to GitHub (the release zip, the latest release's tag, the devkitPro
  updater, the minimap mod) and to the package servers of what it installs, all to do the install the user
  asked for. Nothing is sent about the user.
- A "Code signing policy" section on the home page and the download page: in README and INSTALL.md since
  main 716f4afd, saying the installer stays unsigned until SignPath accepts the project.

## The change

- `installer/windows/SwitchWakerHD.Installer.csproj`: copyright said GPLv3, the project is MPL-2.0; no source
  revision in the product version.
- `.signpath/artifact-configuration.xml`: the artifact configuration to paste into SignPath (the action uploads
  the `.exe` zipped by `actions/upload-artifact`, so the root is `<zip-file>`).
- `.github/workflows/release-installer.yml`: when the repository variable `SIGNPATH_ORGANIZATION_ID` is set, upload
  the unsigned `.exe`, submit it with `signpath/github-action-submit-signing-request@v3`, wait for the approval
  (up to an hour) and replace the `.exe` with the signed one before the SHA256 and the upload. A release signs
  with the `release-signing` policy, a run by hand with `test-signing`.

Repository settings once accepted (Settings > Secrets and variables > Actions):

- secret `SIGNPATH_API_TOKEN`: a SignPath CI user token with submitter rights;
- variables `SIGNPATH_ORGANIZATION_ID`, `SIGNPATH_PROJECT_SLUG` (e.g. `SwitchWakerHD`),
  `SIGNPATH_ARTIFACT_CONFIGURATION_SLUG` (e.g. `installer`).

## On acceptance

1. Create the secret and variables above, and paste `.signpath/artifact-configuration.xml` into the SignPath project.
2. Drop "(Applied for in October 2026: ... still unsigned.)" from INSTALL.md's Code signing policy and
   "(applied for in October 2026; unsigned until accepted)" from README's.
3. Merge this branch, then test as below.

## Testing

1. Before acceptance: a run by hand of the workflow on this branch builds and uploads the unsigned `.exe` as
   today (signing skipped); on Windows, *Properties > Details* shows product version `0.0.0` without `+<commit>`.
2. After acceptance: a run by hand signs with `test-signing`; then the first release with `release-signing`,
   checked with `Get-AuthenticodeSignature SwitchWakerHD-Installer.exe` (Status `Valid`, signer SignPath Foundation)
   and with VirusTotal / a Defender scan.

## Status

- 2026-10-21: plan, csproj and workflow on this branch (signing off until the variables exist); org 2FA required.
- 2026-10-21: code signing policy on main (716f4afd); application sent to SignPath Foundation (no official review time:
  1-2 weeks to ~2 months by other projects' reports). Check mid-November; if refused, Certum Open Source.
