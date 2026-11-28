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
 * update_client.h
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#pragma once
#include "update_release.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// -------------------------------------------------------

/* The GitHub repository ("owner/name") whose releases the editor follows. A fork's build can set its own. */
#ifndef SEDX_UPDATE_REPOSITORY
#define SEDX_UPDATE_REPOSITORY "The3dVehicleguy/Scenery-Editor-X"
#endif

namespace SceneryEditorX::Update
{
	inline constexpr const char *REPOSITORY = SEDX_UPDATE_REPOSITORY;

	/* @brief How often the automatic check asks GitHub (the result is cached in between). */
	inline constexpr std::int64_t CHECK_INTERVAL_SECONDS = 24 * 60 * 60;

	enum class State : std::uint8_t
	{
		Idle,			// Nothing asked yet
		Checking,		// Asking GitHub for the newest release
		UpToDate,		// The newest release isn't newer (or there is none yet)
		Available,		// A newer release (Status::offer says whether the user skipped it)
		CheckFailed,	// Status::error says why
		Downloading,	// The Windows setup of the newer release
		Downloaded,		// Downloaded and its SHA-256 checked: Status::installer is ready to run
		DownloadFailed	// Status::error says why; the release is still known (Available's buttons apply)
	};

	/* @brief A copy of the client's state, safe to keep. */
	struct Status
	{
		State state = State::Idle;
		bool manual = false;				// Help > Check for Updates asked: show the result even when nothing is new
		bool fromCache = false;				// The release came from the last check's cache, not the network
		Offer offer = Offer::None;
		std::optional<Release> release;		// The newest release, once a check found one
		std::string error;
		std::filesystem::path installer;	// Downloaded: the checked setup
		std::uint64_t downloaded = 0;		// Downloading: bytes so far, of total (0 when unknown)
		std::uint64_t total = 0;
		std::int64_t checkedAt = 0;			// Unix seconds of the last check that reached GitHub, 0 for never
		std::uint64_t generation = 0;		// Goes up with every change of the fields above
	};

	struct ClientOptions
	{
		/* @brief Where the newest release is read. Empty: https://api.github.com/repos/<REPOSITORY>/releases/latest. */
		std::string latestReleaseUrl;
		/* @brief Accept http and any download host (a local test server, --update-url); off for real releases. */
		bool testServer = false;
		/* @brief The version the user chose to skip ("" for none); automatic checks don't offer it. */
		std::string skippedVersion;
	};

	/**
	 * @class UpdateClient
	 * @brief Checks GitHub for a newer Scenery Editor X release and, on Windows, downloads its setup and checks it
	 * against the SHA-256 the release publishes. Work runs on a thread of its own, one request at a time; the main
	 * thread reads GetStatus() each frame. The last answer is kept in cache:/updates so a start within a day of the
	 * last check offers what it found without asking GitHub again.
	 */
	class UpdateClient
	{
	public:
		static void Init(const ClientOptions &options);

		/* @brief Stops a check or download in progress and waits for its thread. */
		static void Shutdown();

		/* @brief The running editor's version (SEDX_VERSION_STRING). */
		static Version RunningVersion();

		/**
		 * @brief Asks GitHub for the newest release. manual: the user asked (Help > Check for Updates), so a skipped
		 * version is offered again and "up to date" is worth showing. False while a check or download is running.
		 */
		static bool Check(bool manual);

		/**
		 * @brief For a start: offers what the last check found (from the cache) and asks GitHub only when that check
		 * is a day old or there is none. False when nothing was started (busy).
		 */
		static bool CheckIfDue();

		/* @brief Downloads the newer release's Windows setup. False when there is none or something is running. */
		static bool Download();

		/* @brief Stops a check or download in progress. */
		static void Cancel();

		static Status GetStatus();
		static std::uint64_t GetGeneration();

		/* @brief A download's bytes so far and of total (0 when unknown), without copying the status. */
		static void GetProgress(std::uint64_t &downloaded, std::uint64_t &total);

		/* @brief The version automatic checks don't offer ("" for none); the caller saves it with the preferences. */
		static void SetSkippedVersion(const std::string &version);

		/* @brief Whether this build can install a release itself (Windows: it runs the downloaded setup). */
		static bool CanInstall();

		/**
		 * @brief Starts the downloaded setup (Windows). The editor must close right after: the setup replaces its
		 * files, and waits for the editor to close before it does. False, with error set, when it couldn't start.
		 */
		static bool LaunchInstaller(std::string &error);

		/* @brief Where downloads and the last check's answer are kept (cache:/updates). */
		static std::filesystem::path CacheFolder();
	};
}

// -------------------------------------------------------
