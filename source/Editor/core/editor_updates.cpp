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
 * editor_updates.cpp
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#include "editor_updates.h"
#include "editor_project.h"
#include "editor_theme.h"
#include "editor_ui.h"
#include "editor_welcome.h"
#include "project_hub.h"
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <Editor/settings/editor_preferences.h>
#include <SceneryEditorX/core/application/application.h>
#include <SceneryEditorX/filesystem/file_system.h>
#include <SceneryEditorX/i18n/tr.h>
#include <SceneryEditorX/renderer/font/font_awesome.h>
#include <SceneryEditorX/renderer/ui/i18n_imgui.h>
#include <SceneryEditorX/renderer/ui/resources/fonts.h>
#include <SceneryEditorX/renderer/ui/source/imgui/imgui_internal.h>
#include <SceneryEditorX/update/update_client.h>

// -------------------------------------------------------

namespace SceneryEditorX::EditorUpdates
{
	namespace
	{
		constexpr const char *LOG_TAG = "Update";
		constexpr const char *WINDOW = "Software Update###SoftwareUpdate";
		constexpr const char *PRODUCT_NAME = "Scenery Editor X";		// A name: never translated
		constexpr double STARTUP_DELAY_SECONDS = 3.0;		// Lets the editor finish starting before the check runs
		constexpr float WIDTH = 540.0f;					// The window's content width (unscaled)

		using Update::Offer;
		using Update::UpdateClient;
		using UpdateState = Update::State;

		/* @brief The parts of a release the window shows every frame, worked out once per release. */
		struct ReleaseView
		{
			std::string tag;				// The release these were made for
			std::string notes;				// Plain text
			bool hasInstaller = false;		// It has a Windows setup (FindWindowsInstaller)
			std::uint64_t installerSize = 0;
		};

		struct State
		{
			bool initialized = false;
			bool automatic = false;			// Check at startup (preferences or --check-updates)
			bool forced = false;			// --check-updates: a check like Help > Check for Updates, window included
			bool autoDownload = false;		// --update-download: download the update found, as Download and Install would
			bool started = false;			// The startup check ran
			bool retryCheck = false;		// A check the user asked for met the startup check's brief busy guard
			double startAt = -1.0;			// ImGui time it runs at (set on the first Tick)

			bool windowOpen = false;
			bool focusWindow = false;
			bool openWhenFree = false;		// A startup check found an update: open the window once no modal is up
			std::string announced;			// The update the window opened for by itself (once per run)
			Update::Status status;			// The client's status: copied in Tick when it changes, read by the drawing
			std::uint64_t seenGeneration = 0;

			ReleaseView view;				// What the window shows of the release found, made once per release
			bool confirmInstall = false;	// Install was clicked with unsaved changes: asking before closing
			std::string launchError;		// Why the setup didn't start
		};

		State &S()
		{
			static State state;
			return state;
		}

		ImVec4 Color(const ImVec4 &srgb)
		{
			return EditorTheme::ToStyle(srgb);
		}

		float Dpi()
		{
			return ImGui::GetStyle().FontScaleDpi;
		}

		std::string ReleasesPage()
		{
			return std::string("https://github.com/") + Update::REPOSITORY + "/releases";
		}

		/* @brief The release's own page when it is one of the repository's, else the list of releases. */
		std::string ReleasePage(const Update::Status &status)
		{
			const std::string releases = ReleasesPage();
			if (status.release && status.release->pageUrl.size() > releases.size())
			{
				const std::string &page = status.release->pageUrl;
				bool same = true;
				for (size_t i = 0; i < releases.size() && same; ++i)
					same = std::tolower(static_cast<unsigned char>(page[i])) == std::tolower(static_cast<unsigned char>(releases[i]));
				if (same && page[releases.size()] == '/')
					return page;
			}
			return releases;
		}

		std::string Megabytes(const std::uint64_t bytes)
		{
			char text[32];
			snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
			return text;
		}

		const ReleaseView &View(const Update::Release &release)
		{
			ReleaseView &view = S().view;
			if (view.tag != release.tag)
			{
				view.tag = release.tag;
				view.notes = Update::PlainTextNotes(release.notes);
				const Update::ReleaseAsset *installer = Update::FindWindowsInstaller(release);
				view.hasInstaller = installer != nullptr;
				view.installerSize = installer ? installer->size : 0;
			}
			return view;
		}

		void Skip(const std::string &version)
		{
			EditorPreferences &preferences = EditorPreferences::Get();
			preferences.skippedUpdate = version;
			EditorPreferences::Save();
			UpdateClient::SetSkippedVersion(version);
		}

		void OpenPage(const Update::Status &status)
		{
			IO::FileSystem::OpenUrl(ReleasePage(status));
		}

		void Install()
		{
			State &s = S();
			s.launchError.clear();
			s.confirmInstall = false;
			std::string error;
			if (!UpdateClient::LaunchInstaller(error))
			{
				s.launchError = error;
				EDITOR_WARN_TAG(LOG_TAG, "The update's setup didn't start: {}", error);
				return;
			}
			// The setup waits for the editor to close before it replaces its files
			Application::Get().Stop();
		}

		void DrawHeader(const Update::Status &status)
		{
			UI::Fonts::PushFont("Bold");
			ImGui::TextUnformatted(PRODUCT_NAME);
			UI::Fonts::PopFont();
			// TRANSLATORS: {} is the editor's version, e.g. 0.2.128.629
			ImGui::TextDisabled("%s", TrF("Installed: {}", SEDX_VERSION_STRING).c_str());
			if (status.checkedAt > 0)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
				// TRANSLATORS: {} is a date and time, e.g. 2026-11-27 14:05 UTC
				ImGui::TextDisabled("%s", TrF("Last checked {}", Update::FormatUtcTime(status.checkedAt)).c_str());
			}
			ImGui::Spacing();
		}

		void DrawAvailable(const Update::Status &status)
		{
			State &s = S();
			const float dpi = Dpi();
			const Update::Release &release = *status.release;
			const std::string version = release.version.ToString();
			const ReleaseView &view = View(release);
			const bool canInstall = UpdateClient::CanInstall() && view.hasInstaller;

			ImGui::PushStyleColor(ImGuiCol_Text, Color(EditorTheme::Palette::AccentHovered));
			// TRANSLATORS: {0} is the new version, {1} the installed one
			ImGui::TextWrapped("%s", TrF("Scenery Editor X {0} is available. You have {1}.", version, SEDX_VERSION_STRING).c_str());
			ImGui::PopStyleColor();
			if (const std::string date = release.PublishedDate(); !date.empty())
				ImGui::TextDisabled("%s", TrF("Released {}", date).c_str());
			if (status.offer == Offer::Skipped)
				ImGui::TextDisabled("%s", Tr("You chose to skip this version."));

			ImGui::SeparatorText(Tr("What's new"));
			const std::string &notes = view.notes;
			if (ImGui::BeginChild("##ReleaseNotes", ImVec2(WIDTH * dpi, 220.0f * dpi), ImGuiChildFlags_Borders))
			{
				if (notes.empty())
					ImGui::TextDisabled("%s", Tr("The release has no notes."));
				else
					ImGui::TextWrapped("%s", notes.c_str());
			}
			ImGui::EndChild();
			ImGui::Spacing();

			const ImVec2 button(0.0f, 0.0f);
			if (status.state != UpdateState::Downloaded)
				s.confirmInstall = false;
			switch (status.state)
			{
				case UpdateState::Downloading:
				{
					const float fraction = status.total > 0 ? static_cast<float>(static_cast<double>(status.downloaded) / static_cast<double>(status.total)) : -1.0f * static_cast<float>(ImGui::GetTime());
					// TRANSLATORS: Download progress in megabytes, e.g. "12.5 of 40.2 MB" (the second form: size unknown)
					const std::string progress = status.total > 0 ? TrF("{0} of {1} MB", Megabytes(status.downloaded), Megabytes(status.total))
						: TrF("{} MB", Megabytes(status.downloaded));
					ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), progress.c_str());
					if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_TIMES, "Cancel").c_str(), button))
						UpdateClient::Cancel();
					break;
				}

				case UpdateState::Downloaded:
				{
					ImGui::TextColored(Color(EditorTheme::Palette::Text), "%s %s", SEDX_ICON_CHECK_CIRCLE, Tr("Downloaded, and its SHA-256 matches the release."));
					ImGui::TextWrapped("%s", Tr("Setup updates Scenery Editor X once the editor has closed. It shows what it will do before it starts."));
					const bool unsaved = EditorProjects::IsDirty();
					if (unsaved)
					{
						ImGui::PushStyleColor(ImGuiCol_Text, Color(EditorTheme::Palette::Warning));
						ImGui::TextWrapped("%s", Tr("You have unsaved changes. Save them first: the editor closes when Setup starts."));
						ImGui::PopStyleColor();
					}
					if (s.confirmInstall && unsaved)
					{
						// Closing skips the usual question about unsaved changes: ask here instead
						ImGui::TextWrapped("%s", Tr("Close without saving? The unsaved project changes are kept as recovery data and offered when the project opens again."));
						if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_DOWNLOAD, "Install Without Saving").c_str(), button))
							Install();
						ImGui::SameLine();
						if (ImGui::Button(TrL("Cancel##InstallConfirm"), button))
							s.confirmInstall = false;
					}
					else
					{
						if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_DOWNLOAD, "Install and Close Editor").c_str(), button))
						{
							if (unsaved)
								s.confirmInstall = true;
							else
								Install();
						}
						ImGui::SameLine();
						if (ImGui::Button(TrL("Later##InstallLater"), button))
							s.windowOpen = false;
					}
					if (!s.launchError.empty())
					{
						ImGui::PushStyleColor(ImGuiCol_Text, Color(EditorTheme::Palette::Error));
						ImGui::TextWrapped("%s", TrF("Setup didn't start: {}", s.launchError).c_str());
						ImGui::PopStyleColor();
					}
					break;
				}

				default:
				{
					if (status.state == UpdateState::DownloadFailed)
					{
						ImGui::PushStyleColor(ImGuiCol_Text, Color(EditorTheme::Palette::Error));
						ImGui::TextWrapped("%s", TrF("The download failed: {}", status.error).c_str());
						ImGui::PopStyleColor();
					}
					if (canInstall)
					{
						// TRANSLATORS: {} is the size of the setup in megabytes
						const std::string label = EditorUI::IconLabel(SEDX_ICON_DOWNLOAD, TrF("Download and Install ({} MB)", Megabytes(view.installerSize))) + "###DownloadUpdate";
						if (ImGui::Button(label.c_str(), button))
							UpdateClient::Download();
						EditorUI::ItemTooltip("Downloads the Windows setup from the release, checks it against the SHA-256 the release publishes, then offers to install it");
						ImGui::SameLine();
					}
					if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_EXTERNAL_LINK, "Release Page").c_str(), button))
						OpenPage(status);
					ImGui::SameLine();
					if (status.offer != Offer::Skipped && ImGui::Button(TrL("Skip This Version"), button))
					{
						Skip(version);
						s.windowOpen = false;
					}
					ImGui::SameLine();
					if (ImGui::Button(TrL("Remind Me Later"), button))
						s.windowOpen = false;
					if (!canInstall && !UpdateClient::CanInstall())
						ImGui::TextDisabled("%s", Tr("Download the new version from the release page."));
					else if (!canInstall)
						ImGui::TextDisabled("%s", Tr("This release has no Windows setup: download it from the release page."));
					break;
				}
			}
		}

		void DrawWindow()
		{
			State &s = S();
			const float dpi = Dpi();
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			if (s.focusWindow)
			{
				ImGui::SetNextWindowFocus();
				s.focusWindow = false;
			}
			if (!ImGui::Begin(TrL(WINDOW), &s.windowOpen, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::End();
				return;
			}
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + WIDTH * dpi);
			ImGui::Dummy(ImVec2(WIDTH * dpi, 0.0f));	// Holds the auto-sized window at its width when the text is short

			const Update::Status &status = S().status;
			DrawHeader(status);

			switch (status.state)
			{
				case UpdateState::Idle:
					ImGui::TextWrapped("%s", Tr("Updates haven't been checked yet."));
					if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_REFRESH, "Check Now").c_str()))
						CheckNow();
					break;

				case UpdateState::Checking:
					ImGui::TextWrapped("%s", Tr("Asking GitHub for the newest release..."));
					break;

				case UpdateState::UpToDate:
					ImGui::TextColored(Color(EditorTheme::Palette::Text), "%s", SEDX_ICON_CHECK_CIRCLE);
					ImGui::SameLine();
					ImGui::TextWrapped("%s", Tr("You have the newest version of Scenery Editor X."));
					if (status.release && status.release->version < UpdateClient::RunningVersion())
						ImGui::TextDisabled("%s", TrF("The newest release is {}: this build is newer.", status.release->version.ToString()).c_str());
					if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_REFRESH, "Check Again").c_str()))
						CheckNow();
					ImGui::SameLine();
					if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_EXTERNAL_LINK, "All Releases").c_str()))
						IO::FileSystem::OpenUrl(ReleasesPage());
					break;

				case UpdateState::CheckFailed:
					ImGui::PushStyleColor(ImGuiCol_Text, Color(EditorTheme::Palette::Error));
					ImGui::TextWrapped("%s", TrF("The update check failed: {}", status.error).c_str());
					ImGui::PopStyleColor();
					if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_REFRESH, "Try Again").c_str()))
						CheckNow();
					ImGui::SameLine();
					if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_EXTERNAL_LINK, "All Releases").c_str()))
						IO::FileSystem::OpenUrl(ReleasesPage());
					break;

				case UpdateState::Available:
				case UpdateState::Downloading:
				case UpdateState::Downloaded:
				case UpdateState::DownloadFailed:
					if (status.release)
						DrawAvailable(status);
					break;
			}

			ImGui::Spacing();
			ImGui::Separator();
			EditorPreferences &preferences = EditorPreferences::Get();
			if (ImGui::Checkbox(TrL("Check for updates at startup"), &preferences.checkForUpdates))
				EditorPreferences::Save();
			EditorUI::ItemTooltip("At most once a day, a few seconds after the editor starts. The request tells GitHub this editor's version, nothing else");

			ImGui::PopTextWrapPos();
			ImGui::End();
		}
	}

	// -------------------------------------------------------

	void Init()
	{
		State &s = S();
		if (s.initialized)
			return;

		Update::ClientOptions options;
		bool scripted = false, disabled = false;
		if (const PlatformContext *platform = Application::Get().GetPlatformContext())
		{
			const std::vector<std::string> &args = platform->GetCommandLineArgs();
			for (size_t i = 0; i < args.size(); ++i)
			{
				const std::string &arg = args[i];
				if (arg == "--no-update-check")
					disabled = true;
				else if (arg == "--check-updates")
					s.forced = true;
				else if (arg == "--update-download")
					s.autoDownload = true;
				else if (arg == "--update-url" && i + 1 < args.size())
				{
					options.latestReleaseUrl = args[++i];
					options.testServer = true;
				}
				else if (arg == "--project")
					++i;	// Explorer opening a project file: an ordinary start
				else if (arg.starts_with("--"))
					scripted = true;
			}
		}

		const EditorPreferences &preferences = EditorPreferences::Get();
		options.skippedVersion = preferences.skippedUpdate;
		UpdateClient::Init(options);
		s.automatic = !disabled && (s.forced || (!scripted && preferences.checkForUpdates));
		s.initialized = true;
	}

	void Shutdown()
	{
		State &s = S();
		if (!s.initialized)
			return;
		UpdateClient::Shutdown();
		s.initialized = false;
	}

	void Tick()
	{
		State &s = S();
		if (!s.initialized)
			return;

		if (s.retryCheck)
			s.retryCheck = !UpdateClient::Check(true);

		if (s.automatic && !s.started)
		{
			const double now = ImGui::GetTime();
			if (s.startAt < 0.0)
				s.startAt = now + STARTUP_DELAY_SECONDS;
			else if (now >= s.startAt)
			{
				s.started = true;
				if (s.forced)
					CheckNow();
				else
					UpdateClient::CheckIfDue();
			}
		}

		// The status is copied only when it changed (it holds the release notes); a download's progress every frame
		bool changed = false;
		if (UpdateClient::GetGeneration() != s.seenGeneration)
		{
			s.status = UpdateClient::GetStatus();
			s.seenGeneration = s.status.generation;
			changed = true;
		}
		if (s.status.state == UpdateState::Downloading)
			UpdateClient::GetProgress(s.status.downloaded, s.status.total);

		const Update::Status &status = s.status;
		if (changed)
		{
			if (s.autoDownload && status.state == UpdateState::Available && status.offer == Offer::Newer && UpdateClient::CanInstall())
			{
				s.autoDownload = false;
				UpdateClient::Download();
			}
			if (!status.manual && status.state == UpdateState::Available && status.offer == Offer::Newer && status.release &&
				s.announced != status.release->tag)
			{
				s.announced = status.release->tag;
				s.openWhenFree = true;
				EDITOR_INFO_TAG(LOG_TAG, "Scenery Editor X {} is available (running {})", status.release->version.ToString(), SEDX_VERSION_STRING);
			}
		}

		// Not over the first-launch keymap question, the Project Hub or another modal: after them
		if (s.openWhenFree && !ImGui::GetTopMostPopupModal() && !EditorWelcome::IsShowing() && !ProjectHub::IsOpen())
		{
			s.openWhenFree = false;
			OpenWindow();
		}
	}

	void Draw()
	{
		if (S().initialized && S().windowOpen)
			DrawWindow();
	}

	void DrawMenuItems()
	{
		const Update::Status &status = S().status;
		if (status.release && status.offer == Offer::Newer && status.state != UpdateState::Checking)
		{
			// TRANSLATORS: Help menu item; {} is the new version
			const std::string label = TrF("Update {} Available...", status.release->version.ToString()) + "###UpdateAvailable";
			if (ImGui::MenuItem(label.c_str()))
				OpenWindow();
		}
		if (ImGui::MenuItem(TrL("Check for Updates..."), nullptr, false, status.state != UpdateState::Checking && status.state != UpdateState::Downloading))
			CheckNow();
	}

	void DrawStatusBarItem(const float spacing)
	{
		if (!S().initialized)
			return;
		const Update::Status &status = S().status;
		std::string text;
		switch (status.state)
		{
			case UpdateState::Available:
			case UpdateState::DownloadFailed:
				if (status.release && status.offer == Offer::Newer)
					text = TrF("Update {} available", status.release->version.ToString());
				break;
			case UpdateState::Downloading:
				if (status.total > 0)
					text = TrF("Downloading update: {}%", static_cast<int>(100.0 * static_cast<double>(status.downloaded) / static_cast<double>(status.total)));
				else
					text = Tr("Downloading update...");
				break;
			case UpdateState::Downloaded:
				text = Tr("Update ready to install");
				break;
			default:
				break;
		}
		if (text.empty())
			return;

		ImGui::SameLine(0.0f, spacing);
		ImGui::PushStyleColor(ImGuiCol_Text, Color(EditorTheme::Palette::AccentHovered));
		ImGui::Text("%s %s", SEDX_ICON_ARROW_CIRCLE_O_UP, text.c_str());
		ImGui::PopStyleColor();
		if (ImGui::IsItemClicked())
			OpenWindow();
		EditorUI::ItemTooltip("Click to see what's new and update");
	}

	void DrawPreferencesPage()
	{
		EditorPreferences &preferences = EditorPreferences::Get();
		const Update::Status &status = S().status;
		ImGui::Spacing();
		if (ImGui::Checkbox(TrL("Check for updates at startup"), &preferences.checkForUpdates))
			EditorPreferences::Save();
		ImGui::TextWrapped("%s", Tr("At most once a day, a few seconds after the editor starts, Scenery Editor X asks GitHub whether a newer "
			"release is out. The request tells GitHub this editor's version, nothing else. Help > Check for Updates asks at any time."));
		ImGui::Spacing();
		if (status.checkedAt > 0)
			ImGui::TextDisabled("%s", TrF("Last checked {}", Update::FormatUtcTime(status.checkedAt)).c_str());
		if (!preferences.skippedUpdate.empty())
		{
			ImGui::TextDisabled("%s", TrF("Skipped version: {}", preferences.skippedUpdate).c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(TrL("Offer It Again")))
				Skip(std::string());
		}
		ImGui::Spacing();
		if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_REFRESH, "Check Now").c_str()))
			CheckNow();
	}

	void CheckNow()
	{
		if (!S().initialized)
			return;
		State &s = S();
		s.launchError.clear();
		// Refused while something runs: a download keeps going (the window shows it); anything else is only the startup
		// check reading its cache for a moment, so ask again next frame instead of silently doing nothing
		if (!UpdateClient::Check(true))
		{
			const UpdateState state = UpdateClient::GetStatus().state;
			s.retryCheck = state != UpdateState::Downloading && state != UpdateState::Checking;
		}
		OpenWindow();
	}

	void OpenWindow()
	{
		State &s = S();
		s.windowOpen = true;
		s.focusWindow = true;
		s.confirmInstall = false;
	}
}

// -------------------------------------------------------
