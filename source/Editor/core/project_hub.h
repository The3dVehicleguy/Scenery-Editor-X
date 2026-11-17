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
 * project_hub.h
 * -------------------------------------------------------
 * Created: 17/11/2026
 * -------------------------------------------------------
 */
#pragma once
#include <string>
#include <SceneryEditorX/settings/user_settings.h>

// -------------------------------------------------------

namespace SceneryEditorX
{
	struct EditorContext;
}

namespace SceneryEditorX::ProjectHub
{
	/**
	 * The Project Hub: the modal the editor starts on (File > Project Hub..., Ctrl+Shift+O) for picking what to work on.
	 *
	 *  - Recent Projects: the projects opened lately, searchable, opened with a double-click (one whose file is gone can
	 *    be taken off the list); Open Project... browses for any .edX, .sex or earth.wed.xml.
	 *  - New Project: the New Project form, set to a new airport (scenery and library projects too).
	 *  - Clone from Gateway: searches the X-Plane Scenery Gateway's airports, lists an airport's submissions (the
	 *    recommended one picked), and clones one into a project folder of its own (Documents/Scenery Editor X/Projects
	 *    by default, outside X-Plane's Custom Scenery so the simulator doesn't load the working copy): the submission's
	 *    files plus a project holding its airport and DSF placements as editable items (ProjectFile::CreateGatewayCloneProject),
	 *    which then opens.
	 *
	 * It shows at startup while the "Show at startup" box is ticked (the user preference show_welcome_screen), after the
	 * first-launch keymap choice and only when no project was asked for; runs started with other command-line flags skip
	 * it. Opening a project from it asks about the open project's unsaved changes first. Main thread only.
	 *
	 * Command line: --project-hub [recent|new|gateway] shows it at startup (screenshots); --gateway-clone
	 * "<scenery id>[;folder]" clones a Gateway submission without the hub (into the default folder unless one is given)
	 * and opens it, for verification runs.
	 */

	/* @brief The hub's pages. */
	enum class Page
	{
		Recent,
		NewProject,
		Gateway
	};

	/* @brief Reads the command line and the preferences (recent projects, show at startup). Call once, after EditorProjects::Init. */
	void Init(const Ref<UserPreferences> &preferences);

	/* @brief Cancels a Gateway download in flight. */
	void Shutdown();

	/* @brief Opens the hub on a page. */
	void Open(Page page = Page::Recent);

	/* @brief Opens the Gateway page on an airport, with one of its submissions picked (0: the recommended one). */
	void OpenGateway(const std::string &airportCode, int sceneryId = 0);

	/* @brief Once per frame: a clone's download finishing (also --gateway-clone without the hub). */
	void Tick(EditorContext *context);

	/* @brief Draws the hub when it's open, and opens it at startup. Every frame, with the other modals. */
	void Draw(EditorContext *context);

	/* @brief True while the hub is open (or about to open). */
	bool IsOpen();
}

// -------------------------------------------------------
