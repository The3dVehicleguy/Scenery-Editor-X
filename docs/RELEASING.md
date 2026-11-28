# Releasing Scenery Editor X

A release is a GitHub release made by [`.github/workflows/release.yml`](../.github/workflows/release.yml) from a tag on
the `release` branch. Installed editors find it by themselves: a few seconds after startup, at most once a day, they ask
GitHub for the newest release, and on Windows they offer to download, check and install its setup
(**Help › Check for Updates…** asks at any time). Nothing else publishes a release, and the workflow refuses a tag that
isn't a correct release tag.

```
feature branches ──PR──▶ dev ──PR──▶ release ──tag v0.2.130.631──▶ Release workflow ──▶ GitHub release ──▶ update check
```

| Branch / tag | What it is |
|---|---|
| `dev` | Day-to-day development. Feature branches are cut from it and come back to it through pull requests. |
| `master` | The GitHub default branch: issue forms, issue workflows and `Closes #N` follow it. Bring it up to date from `dev` at a release, as before; tags on it don't publish. |
| `release` | What has been, or is about to be, published. It only moves forward, through pull requests from `dev`. |
| `v<version>` | A release tag: `v` and `SEDX_VERSION_STRING` from `source/resource.h`, like `v0.2.130.631`. |

## Making a release

1. **Prepare it on `dev`** (a pull request like any other):
   - Bump the version in `source/resource.h`: `SEDX_PATCH` and `SEDX_BUILD` (or the major / minor parts),
     `SEDX_VERSION_STRING`, and the `SEDX_VERSION` hex, which is `major << 32 | minor << 24 | patch << 16 | build`
     (0.2.130.631 is `0x0002820277`). It must be newer than every release so far.
   - In `CHANGELOG.md`, rename `## Unreleased` to `## 0.2.130.631 - 2026-12-01` (the version and the date) and start a
     new, empty `## Unreleased` above it. That section becomes the release notes, and the editor shows them in its
     **Software Update** window.
2. **Merge `dev` into `release`** with a pull request (base `release`). The unit tests run on it on all three platforms.
3. **Tag the merge on `release`** and push the tag:

   ```bash
   git fetch origin release
   git tag -a v0.2.130.631 origin/release -m "Scenery Editor X 0.2.130.631"     # -s instead of -a to sign it
   git push origin v0.2.130.631
   ```

4. **Watch the workflow** (`gh run watch`, or the repository's Actions tab, workflow *Release*). It takes about an hour
   when the vcpkg ports have to be built and much less once they're cached:
   - **Release gate** checks the tag (below) and writes the release notes.
   - **Unit tests** run the Catch2 suites on Linux (g++), macOS (Apple clang) and Windows (MSYS2 g++).
   - **Windows installer** builds the Release editor and its NSIS setup, starts the staged editor with
     `--check-vulkan` to prove its files load, and writes the setup's SHA-256.
   - **Publish the release** runs only when all of the above passed: it checks the files against their SHA-256 and
     creates the release, marked latest, with `SceneryEditorX-<version>-Alpha-Setup.exe`, its `.sha256` and
     `SHA256SUMS.txt`.
5. **Check the release page.** Editors that check at startup offer the release within a day.

### When it fails

Nothing is published unless every job passed. Fix the problem, then move the tag to the fixed commit on `release`:

```bash
git push origin :refs/tags/v0.2.130.631       # delete the remote tag
git tag -d v0.2.130.631
# fix, merge into release, then tag and push again (step 3)
```

Never delete or move the tag of a release that **was** published, and never publish the same version twice: editors
remember the newest release they saw. Make a new version instead.

## What the gate checks

[`scripts/ci/release_gate.sh`](../scripts/ci/release_gate.sh) refuses the tag, with the reason, unless:

| Check | Why |
|---|---|
| The tag is `v<major>.<minor>.<patch>.<build>` and is the commit the workflow checked out | Only release tags publish; other `v*` tags (`v1-test`) stop at the gate |
| `source/resource.h` at that commit has the tag's version, and its four parts and hex agree | The editor compares this version with the release's; the setup and the exe's file version come from it |
| The commit is on the `release` branch | Releases only come from `release`, never from `dev` or a feature branch |
| The version is newer than every published release, and not published already | The update check only moves forward |
| `CHANGELOG.md` has a `## <version>` section with text in it | It becomes the release notes |

Try it locally: `bash scripts/ci/release_gate.sh` (a dry run: the version and the notes), or
`bash scripts/ci/release_gate.sh --tag v0.2.130.631 --release-ref origin/release --published <(gh release list --json tagName --jq '.[].tagName')`.

## Dry runs

Pull requests into `dev` or `release` that change the release pipeline (`release.yml`, `scripts/ci/release_gate.sh`,
`scripts/ci/build_release_windows.ps1`, `installer/**`) run the whole workflow except the publish job, and keep the
installer as a workflow artifact for two weeks. So does **Actions › Release › Run workflow** on any branch, once the
workflow is on the default branch. Test a pipeline change that way before tagging.

The Windows job runs [`scripts/ci/build_release_windows.ps1`](../scripts/ci/build_release_windows.ps1), which works the
same on a developer machine (Visual Studio, CMake, NSIS, the Vulkan SDK with `VULKAN_SDK` set):
`pwsh scripts\ci\build_release_windows.ps1` builds `build\` and leaves the setup in `build\installer\`.

## What the editor relies on

The update check ([`source/SceneryEditorX/update/`](../source/SceneryEditorX/update), window in
[`source/Editor/core/editor_updates.cpp`](../source/Editor/core/editor_updates.cpp)) reads
`https://api.github.com/repos/The3dVehicleguy/Scenery-Editor-X/releases/latest`, so:

- **Don't mark releases as pre-releases or drafts.** GitHub leaves those out of "latest", and editors wouldn't see them.
- **Keep the setup's name** `SceneryEditorX-<version>-<channel>-Setup.exe`, with the tag's version: that's how the
  editor finds it. It downloads only files of this repository's releases (`https://github.com/<repo>/releases/download/…`)
  and checks them against GitHub's own SHA-256 of the file, else against `<setup>.sha256` or `SHA256SUMS.txt`; a
  release without either can't be installed from the editor.
- A fork builds with `-DSEDX_UPDATE_REPOSITORY="owner/name"` (a compile definition) to follow its own releases.

To try the update window without publishing anything, serve a JSON file shaped like GitHub's answer and start the
editor with `--check-updates --update-url http://127.0.0.1:8000/latest.json` (with that option plain `http` and any
download address are accepted). [`source/Tests/update_tests/UpdateTests.cpp`](../source/Tests/update_tests/UpdateTests.cpp)
has an example of the JSON.

## Repository settings

Set once by a repository administrator (the workflow can't change them):

- **The `release` branch ruleset.** The *version release* ruleset covers `release` (and `alpha`, `beta`, `stable`) with
  creation, update, deletion and force-push rules and signed commits required, and lists no bypass actors, so as it
  stands nobody can create or update `release`. Before the first release, add a bypass for the maintainers (Repository
  admin role; "pull requests only" keeps every change going through a pull request), then create `release` from `dev`.
  With signed commits required, merge pull requests with a merge or squash commit made on GitHub (GitHub signs those).
- **Who can push release tags (recommended).** A tag ruleset on `refs/tags/v*` that restricts creation, update and
  deletion to maintainers keeps anyone else with write access from starting the workflow. The gate checks every tag
  anyway, so a stray tag can't publish, but it would still run the build.
- **Approval before publishing (optional).** Create an environment named `release` with required reviewers and add
  `environment: release` to the *publish* job: the workflow then waits for an approval before it creates the release.
- **Actions.** The repository allows only GitHub's own actions and a few listed ones; the release workflow uses only
  `actions/*`. It needs no secrets: the publish job uses the workflow's own token with `contents: write`.
