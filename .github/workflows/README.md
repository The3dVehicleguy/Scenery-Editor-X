# Workflows

What runs where, and when. Releases are described in [docs/RELEASING.md](../../docs/RELEASING.md).

## Build and test

| Workflow | Runs on | What it does |
|---|---|---|
| [unit-tests.yml](unit-tests.yml) | Pull requests into `dev` and `release`; pushes to `dev`, `master`, `release`; called by `release.yml` | The Catch2 suites that need no GPU ([scripts/ci/build_unit_tests.sh](../../scripts/ci/build_unit_tests.sh)) on Linux (g++), macOS (Apple clang) and Windows (MSYS2 g++), with JUnit reports |
| [localization.yml](localization.yml) | Pull requests into `dev` and pushes to `dev` that touch the source or the catalogs | Extracts the translatable strings, updates the catalogs, lints the Dear ImGui calls, and commits the refreshed files to the pull request |
| [validate-github-forms.yml](validate-github-forms.yml) | Pull requests and pushes to `dev` / `master` that touch `.github/` | Issue forms and the triage map, and `actionlint` + `shellcheck` over every workflow |

## Release

| Workflow | Runs on | What it does |
|---|---|---|
| [release.yml](release.yml) | A pushed `v*` tag (publishes, if the gate passes); pull requests that change the release pipeline and manual runs (dry runs, nothing published) | **Release gate** ([scripts/ci/release_gate.sh](../../scripts/ci/release_gate.sh): the tag is `v<version>`, matches `source/resource.h`, is on `release`, is newer than every release, has a CHANGELOG section) → **Unit tests** (`unit-tests.yml`) and **Windows installer** ([scripts/ci/build_release_windows.ps1](../../scripts/ci/build_release_windows.ps1): Release build, NSIS setup, a start of the staged editor, SHA-256) → **Publish the release** (the setup, its `.sha256` and `SHA256SUMS.txt`, notes from the CHANGELOG, marked latest). The editor's update check reads that release. |
| [vcpkg-packages.yml](vcpkg-packages.yml) | Pushes to `dev` / `release` / `master` that change `vcpkg.json` or the vcpkg submodule; Mondays; manual; dependency pull requests (check only) | Builds the vcpkg ports once and keeps them as the artifact `vcpkg-x64-windows` ([scripts/ci/vcpkg_prebuilt.ps1](../../scripts/ci/vcpkg_prebuilt.ps1)); `release.yml` restores the newest set from a trusted run instead of building the ports again. [Spec](../../spec/spec-process-cicd-vcpkg-packages.md) |

## Issues and reviews

| Workflow | Runs on | What it does |
|---|---|---|
| [issue-triage.yml](issue-triage.yml) | New and edited issues, comments | Labels from the issue forms' answers, missing-information checks |
| [add-to-project.yml](add-to-project.yml) | Opened, reopened and transferred issues; manual | Assigns them and adds them to the organization's project (needs the `PROJECT_TOKEN` secret) |
| [claude-issue-triage.yml](claude-issue-triage.yml) | Issues labelled `CLAUDE TRIAGE` | Claude triages the issue |
| [claude.yml](claude.yml) | `@claude` in issues and pull request comments, the `CLAUDE` label | Claude works on the request against `dev` |
| [claude-pr.yml](claude-pr.yml) | Pull requests (opened, updated) | Claude reviews the change |
| [sync-labels.yml](sync-labels.yml) | Pushes to `dev` / `master` that change `.github/labels.json`; manual | Creates and updates the labels the forms use |

Issue and comment workflows, issue forms and manual runs use the files on the default branch (`master`): changes to
them take effect once they reach it.

## Conventions

- Only GitHub's own actions (`actions/*`) and the few the repository allows (`anthropics/claude-code-action`,
  `google/clusterfuzzlite`) can run; any other action stops the whole workflow before a job starts.
- Give a workflow the least it needs: `permissions: contents: read` at the top, and a job that writes asks for more
  itself (the release's publish job: `contents: write`).
- The submodules use SSH URLs: workflows fetch them with
  `git -c 'url.https://github.com/.insteadOf=git@github.com:' submodule update --init --depth 1 <paths>`.
- The work is in scripts under `scripts/ci/` that run the same way on a developer machine, so a failing job can be
  repeated locally with the same command.
