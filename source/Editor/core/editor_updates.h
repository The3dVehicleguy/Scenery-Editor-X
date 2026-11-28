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
 * editor_updates.h
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#pragma once

// -------------------------------------------------------

namespace SceneryEditorX::EditorUpdates
{
	/**
	 * The editor's update check (SceneryEditorX/update/update_client.h) and its window, Help > Check for Updates.
	 *
	 * A few seconds after startup the editor asks GitHub, at most once a day, whether a newer release is out (Edit >
	 * Preferences > Updates turns that off). When one is, the Software Update window opens once the startup windows
	 * are closed, with the release notes and, on Windows, Download and Install: the setup is downloaded, checked
	 * against the SHA-256 the release publishes, and run once the editor closes. Elsewhere it links to the release
	 * page. The status bar shows an available update until it's installed or skipped.
	 *
	 * Command line: --no-update-check (no check at startup), --check-updates (check now, even in a scripted run, and
	 * show the window), --update-url <url> (read the newest release from a test server instead of GitHub),
	 * --update-download (download the update a check finds, as Download and Install does; for testing).
	 * Runs started with other flags (self-tests, captures, scripted exports) don't check, apart from --project
	 * (opening a project file from Explorer).
	 */

	/* @brief Reads the command line and the preferences and starts the client. Call once at startup. */
	void Init();

	/* @brief Stops a check or download and waits for it. Call at shutdown. */
	void Shutdown();

	/* @brief Starts the startup check when it's time and opens the window for what it found. Every frame, outside any window. */
	void Tick();

	/* @brief The Software Update window, when it's open. Every frame. */
	void Draw();

	/* @brief Help menu: Check for Updates (and the update found, when there is one). */
	void DrawMenuItems();

	/* @brief A status bar notice for an update found or being downloaded, after spacing; draws nothing otherwise. */
	void DrawStatusBarItem(float spacing);

	/* @brief The Updates page of the Preferences window. */
	void DrawPreferencesPage();

	/* @brief Asks GitHub now, the update found before a skipped one included, and shows the window. */
	void CheckNow();

	/* @brief Opens the Software Update window. */
	void OpenWindow();
}

// -------------------------------------------------------
