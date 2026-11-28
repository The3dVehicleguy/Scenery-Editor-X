#!/usr/bin/env bash
# The release workflow's gate (.github/workflows/release.yml): decides whether a pushed tag may publish a release, and
# writes the release notes. Run from anywhere in the repository; the checkout must be the commit being released.
#
#   bash scripts/ci/release_gate.sh --tag v0.2.128.629 --release-ref origin/release [--published tags.txt] [--notes notes.md]
#   bash scripts/ci/release_gate.sh [--notes notes.md]                 (dry run: checks the version only, never publishes)
#
# A tag publishes only when all of these hold:
#   - it is named v<major>.<minor>.<patch>.<build>, and it is the commit checked out;
#   - source/resource.h at that commit says the same version (SEDX_VERSION_STRING, the SEDX_MAJOR / MINOR / PATCH /
#     BUILD parts and the SEDX_VERSION hex agree with each other and with the tag);
#   - the commit is on the release branch (--release-ref: an ancestor of it, or its tip);
#   - the version is newer than every release already published (--published: one tag per line), so the editor's update
#     check, which reads the newest release, only ever moves forward;
#   - CHANGELOG.md has a section for the version ("## 0.2.128.629", "## [0.2.128.629] - 2026-12-01", ...) with text in
#     it: that text is the release notes.
# A dry run (no --tag: pull requests, manual runs) checks resource.h and writes notes from the version's section, else
# from "## Unreleased".
#
# Prints every check; exits 1 when one fails. Under GitHub Actions it also writes version, tag, publish (true / false)
# and notes to $GITHUB_OUTPUT and a summary to $GITHUB_STEP_SUMMARY.
set -euo pipefail

TAG=""
RELEASE_REF=""
PUBLISHED=""
NOTES=""
while [ $# -gt 0 ]; do
	case "$1" in
		--tag) TAG=${2:?--tag needs a value}; shift 2 ;;
		--release-ref) RELEASE_REF=${2:?--release-ref needs a value}; shift 2 ;;
		--published) PUBLISHED=${2:?--published needs a file}; shift 2 ;;
		--notes) NOTES=${2:?--notes needs a file}; shift 2 ;;
		-h|--help) awk 'NR > 1 && /^#/ { print; next } NR > 1 { exit }' "$0"; exit 0 ;;
		*) echo "unknown argument $1" >&2; exit 2 ;;
	esac
done

cd "$(git rev-parse --show-toplevel)"

failures=0
pass() { echo "  ok    $*"; }
fail() {
	echo "  FAIL  $*" >&2
	if [ "${GITHUB_ACTIONS:-}" = true ]; then echo "::error title=Release gate::$*"; fi
	failures=$((failures + 1))
}

# Version helpers ---------------------------------------------

VERSION_RE='^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'

# 0 when $1 > $2 (both a.b.c.d)
version_gt()
{
	local -a a b
	IFS=. read -r -a a <<< "$1"
	IFS=. read -r -a b <<< "$2"
	local i
	for i in 0 1 2 3; do
		if ((10#${a[i]} > 10#${b[i]})); then return 0; fi
		if ((10#${a[i]} < 10#${b[i]})); then return 1; fi
	done
	return 1
}

define_value()
{
	sed -n "s/^#define[[:space:]]\{1,\}$1[[:space:]]\{1,\}\(.*\)$/\1/p" source/resource.h | head -n 1 | tr -d '[:space:]"'
}

# resource.h --------------------------------------------------

echo "Release gate"
echo "  commit $(git rev-parse HEAD)"
VERSION=$(define_value SEDX_VERSION_STRING)
MAJOR=$(define_value SEDX_MAJOR)
MINOR=$(define_value SEDX_MINOR)
PATCH=$(define_value SEDX_PATCH)
BUILD=$(define_value SEDX_BUILD)
HEX=$(define_value SEDX_VERSION)

if [[ "$VERSION" =~ $VERSION_RE ]]; then
	pass "source/resource.h: SEDX_VERSION_STRING $VERSION"
else
	fail "source/resource.h: SEDX_VERSION_STRING \"$VERSION\" isn't major.minor.patch.build"
fi
if [ "$MAJOR.$MINOR.$PATCH.$BUILD" = "$VERSION" ]; then
	pass "SEDX_MAJOR / MINOR / PATCH / BUILD match it (the executable's file version)"
else
	fail "SEDX_MAJOR / MINOR / PATCH / BUILD are $MAJOR.$MINOR.$PATCH.$BUILD, not $VERSION"
fi
if [[ "$MAJOR$MINOR$PATCH$BUILD" =~ ^[0-9]+$ ]]; then
	expected_hex=$(printf '0x%010X' $(((10#$MAJOR << 32) | (10#$MINOR << 24) | (10#$PATCH << 16) | 10#$BUILD)))
	if [ "$(echo "$HEX" | tr '[:lower:]' '[:upper:]' | sed 's/^0X/0x/')" = "$expected_hex" ]; then
		pass "SEDX_VERSION $HEX matches"
	else
		fail "SEDX_VERSION is $HEX; for $VERSION it should be $expected_hex (major<<32 | minor<<24 | patch<<16 | build)"
	fi
fi

# The tag -----------------------------------------------------

PUBLISH=false
if [ -n "$TAG" ]; then
	if [[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
		pass "tag $TAG is v<major>.<minor>.<patch>.<build>"
	else
		fail "tag $TAG isn't a release tag: release tags are v<SEDX_VERSION_STRING>, like v$VERSION"
	fi
	if [ "$TAG" = "v$VERSION" ]; then
		pass "tag matches source/resource.h"
	else
		fail "tag $TAG doesn't match source/resource.h ($VERSION): tag the commit that bumps the version to ${TAG#v}, or bump it first"
	fi

	if tag_commit=$(git rev-parse --verify --quiet "refs/tags/$TAG^{commit}"); then
		if [ "$tag_commit" = "$(git rev-parse HEAD)" ]; then
			pass "tag $TAG is the commit checked out"
		else
			fail "tag $TAG is $tag_commit, but $(git rev-parse HEAD) is checked out"
		fi
	else
		fail "tag $TAG isn't in this checkout (fetch the tags)"
	fi

	if [ -z "$RELEASE_REF" ]; then
		fail "no --release-ref: a tag publishes only from the release branch"
	elif ! git rev-parse --verify --quiet "$RELEASE_REF^{commit}" > /dev/null; then
		fail "the release branch ($RELEASE_REF) doesn't exist: create it, merge into it, then tag on it"
	elif git merge-base --is-ancestor HEAD "$RELEASE_REF"; then
		pass "the commit is on the release branch ($RELEASE_REF)"
	else
		fail "the commit isn't on the release branch ($RELEASE_REF): merge it into the release branch and tag the merge"
	fi

	if [ -n "$PUBLISHED" ]; then
		newest=""
		while IFS= read -r published || [ -n "$published" ]; do
			published=${published%$'\r'}
			[ -n "$published" ] || continue
			if [ "$published" = "$TAG" ]; then
				fail "$TAG has been released already"
			fi
			candidate=${published#v}
			if [[ "$candidate" =~ $VERSION_RE ]] && { [ -z "$newest" ] || version_gt "$candidate" "$newest"; }; then
				newest=$candidate
			fi
		done < "$PUBLISHED"
		if [ -z "$newest" ]; then
			pass "no release has been published yet"
		elif [[ "$VERSION" =~ $VERSION_RE ]] && version_gt "$VERSION" "$newest"; then
			pass "$VERSION is newer than the newest release ($newest)"
		else
			fail "$VERSION isn't newer than the newest release ($newest): the update check would never offer it"
		fi
	else
		fail "no --published list: can't check that the version is newer than the releases"
	fi
fi

# Release notes (CHANGELOG.md) ----------------------------------

# The body of the section whose heading names $1: "## 1.2.3.4", "## [1.2.3.4]", "## 1.2.3.4 - 2026-12-01", "## Unreleased"
changelog_section()
{
	awk -v name="$1" '
		function heading_matches(line, rest) {
			rest = substr(line, 4)
			sub(/^\[/, "", rest)
			if (index(rest, name) != 1) return 0
			rest = substr(rest, length(name) + 1)
			return rest == "" || rest ~ /^(\])?([ \t]|$)/
		}
		{ sub(/\r$/, "") }
		/^## / { if (found) exit; if (heading_matches($0)) { found = 1; next } }
		found { print }
	' CHANGELOG.md | sed -e '/./,$!d' | sed -e ':a' -e '/^\n*$/{$d;N;ba' -e '}'
}

notes=""
notes_from=""
if [ ! -f CHANGELOG.md ]; then
	fail "CHANGELOG.md is missing"
else
	notes=$(changelog_section "$VERSION")
	notes_from="## $VERSION"
	if [ -z "$notes" ] && [ -z "$TAG" ]; then
		notes=$(changelog_section "Unreleased")
		notes_from="## Unreleased"
	fi
	if [ -n "$notes" ]; then
		pass "release notes from CHANGELOG.md ($notes_from, $(printf '%s\n' "$notes" | wc -l | tr -d ' ') lines)"
	elif [ -n "$TAG" ]; then
		fail "CHANGELOG.md has no \"## $VERSION\" section with text: rename \"## Unreleased\" to \"## $VERSION - $(date -u +%Y-%m-%d)\" (and start a new Unreleased) in the release commit"
	else
		echo "  note  CHANGELOG.md has no \"## $VERSION\" or \"## Unreleased\" section: the dry run's notes are empty"
	fi
fi
if [ -n "$NOTES" ]; then
	mkdir -p "$(dirname "$NOTES")"
	printf '%s\n' "$notes" > "$NOTES"
fi

# Result ------------------------------------------------------

if [ -n "$TAG" ] && [ "$failures" -eq 0 ]; then
	PUBLISH=true
fi
if [ -n "${GITHUB_OUTPUT:-}" ]; then
	{
		echo "version=$VERSION"
		echo "tag=${TAG:-v$VERSION}"
		echo "publish=$PUBLISH"
		echo "notes=$NOTES"
	} >> "$GITHUB_OUTPUT"
fi
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
	{
		echo "### Release gate"
		echo
		if [ -n "$TAG" ]; then
			echo "Tag \`$TAG\`, version \`$VERSION\`: $([ "$failures" -eq 0 ] && echo "**publishes**" || echo "**$failures check(s) failed, nothing is published**")"
		else
			echo "Dry run for version \`$VERSION\` (no tag): builds the installer, publishes nothing."
		fi
	} >> "$GITHUB_STEP_SUMMARY"
fi

if [ "$failures" -gt 0 ]; then
	echo "Release gate: $failures check(s) failed" >&2
	exit 1
fi
if [ "$PUBLISH" = true ]; then
	echo "Release gate: $TAG publishes"
else
	echo "Release gate: dry run passed (version $VERSION; nothing is published without a release tag)"
fi
