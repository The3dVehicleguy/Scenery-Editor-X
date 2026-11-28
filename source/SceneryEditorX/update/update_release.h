/**
 * -------------------------------------------------------
 * Scenery Editor X
 * -------------------------------------------------------
 * Copyright (c) 2026 Thomas Ray
 * Copyright (c) 2026 Coalition of Freeware Developers
 * -------------------------------------------------------
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * -------------------------------------------------------
 * update_release.h
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#pragma once
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------

/**
 * The update check's decisions, standard library only (plus the json submodule) so ProjectFormatTests-style suites
 * can test them without the editor: release versions, GitHub's release JSON, which file is the installer, checksum
 * listings, and when a check is due. The network side is update_client.h.
 *
 * A release is published by .github/workflows/release.yml for a tag "v<SEDX_VERSION_STRING>" on the `release`
 * branch. Its files are SceneryEditorX-<version>-<channel>-Setup.exe, the same name with ".sha256" added (one
 * sha256sum line), and SHA256SUMS.txt (every file).
 */
namespace SceneryEditorX::Update
{
	/* @brief A Scenery Editor X version, major.minor.patch.build as in SEDX_VERSION_STRING. */
	struct Version
	{
		std::uint32_t major = 0;
		std::uint32_t minor = 0;
		std::uint32_t patch = 0;
		std::uint32_t build = 0;

		auto operator<=>(const Version &) const = default;

		/* @brief "0.2.128.629". */
		[[nodiscard]] std::string ToString() const;
	};

	/**
	 * @brief "0.2.128.629" or a tag "v0.2.128.629" (three parts give build 0). Nothing else is a version: no
	 * suffixes ("1.2.3-beta"), signs, spaces or empty parts, and no part over 999999999.
	 */
	std::optional<Version> ParseVersion(std::string_view text);

	/* @brief A file attached to a release. */
	struct ReleaseAsset
	{
		std::string name;
		std::string downloadUrl;	// browser_download_url
		std::string contentType;
		std::string sha256;			// Lowercase hex from GitHub's "digest" ("sha256:..."); empty when it gave none
		std::uint64_t size = 0;
	};

	/* @brief A GitHub release, as /repos/{owner}/{repo}/releases/latest returns it. */
	struct Release
	{
		std::string tag;			// tag_name
		Version version;			// From the tag
		std::string title;			// name (the tag when empty)
		std::string notes;			// body (Markdown)
		std::string pageUrl;		// html_url
		std::string publishedAt;	// published_at (ISO 8601, UTC)
		bool draft = false;
		bool prerelease = false;
		std::vector<ReleaseAsset> assets;

		/* @brief The date part of publishedAt ("2026-11-27"), empty when there is none. */
		[[nodiscard]] std::string PublishedDate() const;
	};

	/* @brief Reads a release object. False, with error set, when it isn't JSON or its tag isn't a version. */
	bool ParseRelease(std::string_view json, Release &release, std::string &error);

	/* @brief An attached file by name (case-insensitive). nullptr when there is none. */
	const ReleaseAsset *FindAsset(const Release &release, std::string_view name);

	/**
	 * @brief The Windows setup of a release: SceneryEditorX-<version>-<channel>-Setup.exe, case-insensitive. The
	 * version in the name must equal the tag's as a version (0.2.63 and 0.2.63.0 match), the channel is letters and
	 * digits (Alpha, Beta, RC1). nullptr when the release has none.
	 */
	const ReleaseAsset *FindWindowsInstaller(const Release &release);

	/**
	 * @brief The hash a sha256sum listing gives a file: lines "<64 hex>  name" or "<64 hex> *name" (binary mode),
	 * blank lines and "#" comments skipped, the name compared case-insensitively. Lowercase hex, empty when the file
	 * isn't listed.
	 */
	std::string FindChecksum(std::string_view listing, std::string_view fileName);

	/**
	 * @brief Whether a URL is a file of the repository's releases: https://github.com/<repository>/releases/download/...
	 * (repository "owner/name", compared case-insensitively, as GitHub does). Downloads from anywhere else are refused.
	 */
	bool IsReleaseDownloadUrl(std::string_view url, std::string_view repository);

	/**
	 * @brief Whether the automatic check is due: never checked (lastCheck 0), the clock went back past the last
	 * check, or interval seconds have passed since it.
	 */
	bool IsCheckDue(std::int64_t lastCheck, std::int64_t now, std::int64_t interval);

	/* @brief What the newest release means for the running editor. */
	enum class Offer : std::uint8_t
	{
		None,		// Not newer than the running version (or a draft)
		Newer,		// Newer: offer it
		Skipped		// Newer, but the user chose to skip this version (offered again by a check from the Help menu)
	};

	/* @brief Compares the newest release with the running version and the version the user skipped ("" for none). */
	Offer Evaluate(const Version &running, const Release &latest, std::string_view skippedVersion);

	/**
	 * @brief Release notes (GitHub Markdown) as plain text the editor can wrap: no emphasis markers, code ticks,
	 * <kbd> tags or link targets ("[text](url)" keeps the text); headings and list items stay lines of their own.
	 */
	std::string PlainTextNotes(std::string_view markdown);

	/**
	 * @brief A name to save a downloaded file under: the asset's name with anything but letters, digits, '.', '-' and
	 * '_' replaced by '_', leading dots dropped. Never empty, never a path.
	 */
	std::string SafeFileName(std::string_view name);
}

// -------------------------------------------------------
