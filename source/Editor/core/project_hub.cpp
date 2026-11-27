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
 * project_hub.cpp
 * -------------------------------------------------------
 * Created: 17/11/2026
 * -------------------------------------------------------
 */
#include "project_hub.h"
#include "editor_project.h"
#include "editor_theme.h"
#include "editor_ui.h"
#include "editor_welcome.h"
#include "project_dialogs.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <future>
#include <map>
#include <optional>
#include <portable-file-dialogs.h>
#include <SceneryEditorX/core/application/application.h>
#include <SceneryEditorX/core/platform/platform_context.h>
#include <SceneryEditorX/filesystem/file_system.h>
#include <SceneryEditorX/filesystem/vfs.h>
#include <SceneryEditorX/i18n/tr.h>
#include <SceneryEditorX/logging/logging.hpp>
#include <SceneryEditorX/project/gateway_clone.h>
#include <SceneryEditorX/renderer/font/font_awesome.h>
#include <SceneryEditorX/renderer/ui/i18n_imgui.h>
#include <SceneryEditorX/renderer/ui/resources/fonts.h>
#include <SceneryEditorX/renderer/ui/source/imgui/misc/cpp/imgui_stdlib.h>
#include <SceneryEditorX/xplane/xp_gateway.h>
#include <SceneryEditorX/xplane/xp_library_manager.h>

// -------------------------------------------------------

namespace SceneryEditorX::ProjectHub
{
	namespace fs = std::filesystem;
	using namespace XPlane;

	namespace
	{
		constexpr const char *LOG_TAG = "Project";
		constexpr const char *HUB_ID = "###ProjectHub";						// The modal's ID (its title is translated)
		constexpr const char *CLONE_LOCATION_FILE = "user:/gateway_clone_location.txt";	// The folder the last clone went into
		constexpr int STARTUP_FRAMES = 30;									// The main window is up and drawn by then
		constexpr size_t MAX_LOGGED_WARNINGS = 25;
		constexpr const char *PRODUCT_NAME = "Scenery Editor X";						// A name: not translated

		/* @brief What the clone job hands back to the UI thread. */
		struct CloneResult
		{
			bool unpacked = false;
			bool created = false;
			std::string error;
			ProjectFile::GatewayCloneReport report;
		};

		/* @brief A clone the user asked for: the download in flight, then the job making it, and where it goes. */
		struct PendingClone
		{
			std::shared_ptr<GatewayRequest> request;
			ProjectFile::GatewayCloneSpec spec;
			std::string label;					// "KSEA #123456"
			bool fromCommandLine = false;
			std::future<CloneResult> job;		// Valid once downloaded: unpacking, importing and writing it off the UI thread
			std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);	// Set: the job stops and takes back what it wrote
		};

		struct State
		{
			Ref<UserPreferences> preferences;
			bool openRequested = false;
			bool isOpen = false;
			bool closeRequested = false;
			Page page = Page::Recent;
			std::string opening;								// The project the hub waits for (it opens once the library is indexed)
			std::string openError;

			// Startup
			bool startupPending = false;
			Page startupPage = Page::Recent;
			std::string startupAirport;							// --project-hub gateway <code>
			int startupFrames = 0;

			// Recent Projects
			ImGuiTextFilter recentFilter;
			std::string selectedRecent;							// File path
			std::map<std::string, bool> recentExists;			// File path -> found, checked when the hub opens

			// Clone from Gateway: the airport list and its search
			ImGuiTextFilter gatewayFilter;
			bool onlyWithScenery = true;
			bool only3D = false;
			std::vector<uint32_t> results;
			std::string resultsKey;
			std::shared_ptr<const std::vector<GatewayAirport>> resultsList;
			std::shared_ptr<GatewayRequest> listRequest;
			bool listAsked = false;								// The list was fetched once for this session's first visit
			std::string listError;

			// ... the airport picked, its submissions and the one picked
			std::string selectedCode;
			std::shared_ptr<GatewayRequest> detailsRequest;
			std::shared_ptr<const GatewayAirportDetails> details;
			std::string detailsError;
			int selectedScenery = 0;
			int wantedScenery = 0;								// OpenGateway's pick, applied once the submissions arrive

			// ... where it goes
			std::string cloneLocation;
			std::string cloneName;								// Empty: "<ICAO> - <Name>"
			std::optional<PendingClone> clone;
			std::string cloneError;
			int cliSceneryId = 0;								// --gateway-clone
			std::string cliFolder;
		};

		State &S()
		{
			static State state;
			return state;
		}

		ImVec4 Color(const ImVec4 &palette)
		{
			return EditorTheme::ToStyle(palette);
		}

		std::string FormatLocalTime(const std::time_t time)
		{
			if (time <= 0)
				return {};
			std::tm local{};
#ifdef _WIN32
			localtime_s(&local, &time);
#else
			localtime_r(&time, &local);
#endif
			char buffer[32];
			std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
			return buffer;
		}

		std::string FormatBytes(const uint64_t bytes)
		{
			char buffer[32];
			if (bytes >= 1024u * 1024u)
				snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
			else
				snprintf(buffer, sizeof(buffer), "%.0f KB", static_cast<double>(bytes) / 1024.0);
			return buffer;
		}

		/* @brief Spinner, state and progress of a Gateway request. */
		void DrawBusy(const std::string &what)
		{
			static const char *spinner = "|/-\\";
			ImGui::TextColored(Color(EditorTheme::Palette::Warning), "%c %s", spinner[static_cast<int>(ImGui::GetTime() * 8.0) & 3], what.c_str());
		}

		void DrawRequestStatus(const GatewayRequest &request, const std::string &what)
		{
			DrawBusy(what);
			ImGui::SameLine();
			const float progress = request.GetProgress();
			if (progress >= 0.0f)
				ImGui::ProgressBar(progress, ImVec2(160.0f * ImGui::GetStyle().FontScaleDpi, 0.0f));
			else
				ImGui::TextDisabled("%s", FormatBytes(request.GetBytesReceived()).c_str());
		}

		fs::path CustomSceneryFolder()
		{
			const fs::path root = LibraryManager::GetInstallPath();
			return root.empty() ? fs::path() : VFS::GetCustomSceneryFolder(root);
		}

		/* @brief Where clones go unless another folder was used last: Documents/Scenery Editor X/Projects. */
		fs::path DefaultCloneFolder()
		{
			fs::path documents = VFS::GetFolder(VFS::Folder::Documents);
			if (documents.empty())
				documents = VFS::GetFolder(VFS::Folder::Home);
			return documents.empty() ? fs::path() : documents / "Scenery Editor X" / "Projects";
		}

		std::string CloneLocation()
		{
			std::string saved;
			std::error_code ec;
			if (VFS::Get().Read(CLONE_LOCATION_FILE, saved))
			{
				while (!saved.empty() && (saved.back() == '\n' || saved.back() == '\r' || saved.back() == ' '))
					saved.pop_back();
				if (!saved.empty() && fs::is_directory(VFS::FromUtf8(saved), ec))
					return saved;
			}
			return VFS::ToUtf8(DefaultCloneFolder());
		}

		void Close()
		{
			S().closeRequested = true;
		}

		/* @brief The hub opened: what it shows is read again. */
		void OnOpened()
		{
			State &s = S();
			s.isOpen = true;
			s.closeRequested = false;
			s.recentExists.clear();
			if (s.preferences)
			{
				std::error_code ec;
				s.selectedRecent.clear();
				for (const auto &[time, recent] : s.preferences->GetRecentProjects())
				{
					const bool exists = fs::is_regular_file(VFS::FromUtf8(recent.filePath), ec);
					s.recentExists[recent.filePath] = exists;
					if (exists && s.selectedRecent.empty())
						s.selectedRecent = recent.filePath;	// The newest one that's still there: Enter opens it
				}
			}
			if (s.cloneLocation.empty())
				s.cloneLocation = CloneLocation();
			s.opening.clear();
			s.openError.clear();
			ProjectDialogs::PrepareNewProjectForm(true);
		}

		/* @brief The hub closed: a clone still downloading is dropped (one being written finishes, then opens). */
		void OnClosed()
		{
			State &s = S();
			s.isOpen = false;
			if (s.clone && !s.clone->fromCommandLine && !s.clone->job.valid())
			{
				if (s.clone->request && !s.clone->request->IsFinished())
				{
					s.clone->request->Cancel();
					EDITOR_INFO_TAG(LOG_TAG, "Gateway clone of {} cancelled", s.clone->label);
				}
				s.clone.reset();
			}
		}

		void SelectPage(const Page page)
		{
			S().page = page;
		}

		/**
		 * @brief Opens a project and closes the hub; while the project waits for the X-Plane library index (just after
		 * startup) the hub stays up and says so.
		 */
		void OpenProject(const fs::path &path, EditorContext *context)
		{
			State &s = S();
			s.openError.clear();
			if (!EditorProjects::RequestOpen(path, context))
			{
				s.openError = TrF("{} couldn't be opened: the CONSOLE panel says why", VFS::ToUtf8(path.filename()));
				return;
			}
			if (!EditorProjects::GetWaitingProject().empty())
				s.opening = VFS::ToUtf8(path.filename());
			else
				Close();
		}

		// -------------------------------------------------------
		// Clone from Gateway

		void SelectAirport(const std::string &code)
		{
			State &s = S();
			if (code.empty())
				return;
			s.selectedCode = code;
			s.details.reset();
			s.detailsError.clear();
			s.selectedScenery = 0;
			s.cloneError.clear();
			s.detailsRequest = GatewayClient::FetchAirport(code);
		}

		/* @brief The submission to preselect: the one asked for, else the recommended one, else the newest approved, else the newest. */
		int PickScenery(const GatewayAirportDetails &details, const int wanted)
		{
			if (wanted != 0 && std::ranges::any_of(details.scenery, [wanted](const GatewayScenery &scenery) { return scenery.sceneryId == wanted; }))
				return wanted;
			if (const GatewayScenery *recommended = details.GetRecommended())
				return recommended->sceneryId;
			for (size_t n = details.scenery.size(); n-- > 0;)
			{
				if (details.scenery[n].IsApproved())
					return details.scenery[n].sceneryId;
			}
			return details.scenery.empty() ? 0 : details.scenery.back().sceneryId;
		}

		void RefreshResults()
		{
			State &s = S();
			const std::shared_ptr<const std::vector<GatewayAirport>> list = GatewayClient::GetAirports();
			const std::string key = std::string(s.gatewayFilter.InputBuf) + '|' + std::to_string(s.onlyWithScenery) + '|' + std::to_string(s.only3D);
			if (list == s.resultsList && key == s.resultsKey)
				return;
			s.resultsList = list;
			s.resultsKey = key;
			s.results.clear();
			if (!list)
				return;
			GatewaySearchOptions options;
			options.onlyWithScenery = s.onlyWithScenery;
			options.only3D = s.only3D;
			s.results = SearchGatewayAirports(*list, s.gatewayFilter.InputBuf, options);
		}

		/* @brief Starts downloading a submission to clone it; Tick() makes the project once it's here. */
		void StartClone(const int sceneryId, ProjectFile::GatewayCloneSpec spec, std::string label, const bool fromCommandLine)
		{
			State &s = S();
			s.cloneError.clear();
			PendingClone clone;
			clone.spec = std::move(spec);
			clone.label = std::move(label);
			clone.fromCommandLine = fromCommandLine;
			clone.request = GatewayClient::FetchScenery(sceneryId);
			EDITOR_INFO_TAG(LOG_TAG, "Downloading {} from the Scenery Gateway to clone it into {}", clone.label, VFS::ToUtf8(clone.spec.location));
			s.clone = std::move(clone);
		}

		/**
		 * @brief The slow part of a clone, run on a job thread: unzipping the submission, reading and importing its DSF tiles
		 * (a large airport's take a while) and writing the folder. It touches nothing but its arguments.
		 */
		CloneResult MakeClone(const std::shared_ptr<const GatewaySceneryPack> pack, const ProjectFile::GatewayCloneSpec spec,
			const std::shared_ptr<const std::atomic<bool>> cancel)
		{
			CloneResult result;
			try
			{
				ProjectFile::GatewaySubmission submission;
				submission.sceneryId = pack->info.sceneryId;
				submission.icao = pack->icao;
				submission.airportName = pack->airportName;
				submission.artist = pack->info.userName;
				submission.dateUploaded = pack->info.dateUploaded;
				submission.type = pack->info.type;
				if (!GatewayClient::UnpackScenery(*pack, submission.files, &result.error))
					return result;
				result.unpacked = true;
				result.created = ProjectFile::CreateGatewayCloneProject(submission, spec, result.report, &result.error, cancel.get());
			}
			catch (const std::exception &exception)
			{
				result.created = false;
				result.error = exception.what();
			}
			return result;
		}

		/* @brief A finished download: the clone's folder and project are made on a job thread, then the project opens. */
		void FinishClone(EditorContext *context)
		{
			State &s = S();
			if (!s.clone || !s.clone->request || !s.clone->request->IsFinished())
				return;

			// Downloaded: start the job
			if (!s.clone->job.valid())
			{
				const GatewayRequest &request = *s.clone->request;
				if (request.GetState() == GatewayRequestState::Cancelled)
				{
					s.clone.reset();
					return;
				}
				if (!request.Succeeded() || !request.scenery)
				{
					s.cloneError = request.GetError().empty() ? std::string(GetGatewayRequestStateName(request.GetState())) : request.GetError();
					EDITOR_ERROR_TAG(LOG_TAG, "Gateway download of {} failed: {}", s.clone->label, s.cloneError);
					s.clone.reset();
					return;
				}
				s.clone->job = std::async(std::launch::async, MakeClone, request.scenery, s.clone->spec, s.clone->cancel);
				return;
			}
			if (s.clone->job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				return;

			PendingClone clone = std::move(*s.clone);
			s.clone.reset();
			const CloneResult result = clone.job.get();
			const GatewaySceneryPack &pack = *clone.request->scenery;
			if (!result.unpacked)
			{
				s.cloneError = result.error;
				EDITOR_ERROR_TAG(LOG_TAG, "Gateway submission {} can't be unpacked: {}", pack.info.sceneryId, result.error);
				return;
			}
			if (!result.created)
			{
				s.cloneError = result.error;
				EDITOR_ERROR_TAG(LOG_TAG, "Could not clone {}: {}", clone.label, result.error);
				return;
			}
			const ProjectFile::GatewayCloneReport &report = result.report;
			EDITOR_INFO_TAG(LOG_TAG, "Cloned {} ({} by {}) into {}: {} files, {} DSF tiles, {} items", clone.label, pack.airportName, pack.info.userName,
				VFS::ToUtf8(report.folder), report.files, report.tiles, report.items);
			for (size_t i = 0; i < report.warnings.size() && i < MAX_LOGGED_WARNINGS; ++i)
				EDITOR_WARN_TAG(LOG_TAG, "Clone: {}", report.warnings[i]);
			if (report.warnings.size() > MAX_LOGGED_WARNINGS)
				EDITOR_WARN_TAG(LOG_TAG, "Clone: {} more warnings", report.warnings.size() - MAX_LOGGED_WARNINGS);

			VFS::Get().Write(CLONE_LOCATION_FILE, VFS::ToUtf8(clone.spec.location));
			s.cloneName.clear();
			if (clone.fromCommandLine || !s.isOpen)		// The hub was closed while the clone was written
				EditorProjects::RequestOpen(report.projectFile, context);
			else
				OpenProject(report.projectFile, context);
		}

		void DrawAirportList()
		{
			State &s = S();
			const float dpi = ImGui::GetStyle().FontScaleDpi;
			const std::shared_ptr<const std::vector<GatewayAirport>> list = GatewayClient::GetAirports();

			// The list: from the disk cache (a day old at most), else downloaded on the first visit
			if (!list && !s.listRequest && !s.listAsked)
			{
				s.listAsked = true;
				s.listRequest = GatewayClient::FetchAirports(false);
			}
			if (s.listRequest && s.listRequest->IsFinished())
			{
				s.listError = s.listRequest->Succeeded() ? std::string() : s.listRequest->GetError();
				if (!s.listError.empty())
					EDITOR_ERROR_TAG("Gateway", "Airport list: {}", s.listError);
				s.listRequest.reset();
			}

			if (s.listRequest)
				DrawRequestStatus(*s.listRequest, Tr("Downloading the Gateway's airport list"));
			else if (list)
				ImGui::TextDisabled("%s", TrF("{} airports, list from {}", list->size(), FormatLocalTime(static_cast<std::time_t>(GatewayClient::GetAirportsTimestamp()))).c_str());
			else if (!s.listError.empty())
				ImGui::TextColored(Color(EditorTheme::Palette::Error), "%s", TrF("No airport list: {}", s.listError).c_str());
			ImGui::SameLine();
			ImGui::BeginDisabled(s.listRequest != nullptr);
			if (ImGui::SmallButton(EditorUI::IconLabel(SEDX_ICON_REFRESH, list ? Text("Refresh") : Text("Download list")).c_str()))
				s.listRequest = GatewayClient::FetchAirports(true);
			ImGui::EndDisabled();
			EditorUI::ItemTooltip("The list of every Gateway airport (about 20 MB, kept for a day)");

			// Search: Enter looks up the code typed even without the list
			const float searchWidth = ImGui::GetContentRegionAvail().x;
			EditorUI::SearchFilter("##hubGatewaySearch", s.gatewayFilter, searchWidth);
			EditorUI::ItemTooltip("Airport code or name. Enter looks a code up directly");
			if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
			{
				std::string code(s.gatewayFilter.InputBuf);
				std::erase_if(code, [](const char c) { return c == ' ' || c == '\t'; });
				std::ranges::transform(code, code.begin(), [](const char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c); });
				if (!code.empty())
					SelectAirport(code);
			}
			ImGui::Checkbox(TrL("Has scenery##hubScenery"), &s.onlyWithScenery);
			EditorUI::ItemTooltip("Only airports with an accepted or approved submission");
			ImGui::SameLine();
			ImGui::Checkbox(TrL("3D##hub3D"), &s.only3D);
			EditorUI::ItemTooltip("Only airports with 3D submissions (buildings, objects), not just a 2D layout");
			RefreshResults();

			constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
			if (!ImGui::BeginTable("##hubGatewayAirports", 3, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
				return;
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(Tr("Code"), ImGuiTableColumnFlags_WidthFixed, 78.0f * dpi);
			ImGui::TableSetupColumn(Tr("Name"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
			ImGui::TableSetupColumn(Tr("Approved"), ImGuiTableColumnFlags_WidthFixed, 66.0f * dpi);
			ImGui::TableHeadersRow();
			if (list)
			{
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(s.results.size()));
				while (clipper.Step())
				{
					for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
					{
						const GatewayAirport &airport = (*list)[s.results[static_cast<size_t>(row)]];
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::PushID(row);
						if (ImGui::Selectable(airport.code.c_str(), airport.code == s.selectedCode, ImGuiSelectableFlags_SpanAllColumns))
							SelectAirport(airport.code);
						ImGui::PopID();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(airport.name.c_str());
						if (airport.deprecated)
						{
							ImGui::SameLine();
							ImGui::TextDisabled("%s", Tr("(deprecated)"));
						}
						ImGui::TableNextColumn();
						if (airport.approvedSceneryCount > 0)
							ImGui::Text("%d", airport.approvedSceneryCount);
						if (airport.recommendedSceneryId != 0)
						{
							ImGui::SameLine();
							ImGui::TextColored(Color(EditorTheme::Palette::Warning), "%s", SEDX_ICON_STAR);
						}
					}
				}
			}
			ImGui::EndTable();
		}

		/* @brief The picked airport's submissions, and the clone form. */
		void DrawAirportDetails()
		{
			State &s = S();
			const float dpi = ImGui::GetStyle().FontScaleDpi;
			if (s.detailsRequest && s.detailsRequest->IsFinished())
			{
				if (s.detailsRequest->Succeeded() && s.detailsRequest->airport)
				{
					s.details = s.detailsRequest->airport;
					s.selectedScenery = PickScenery(*s.details, s.wantedScenery);
				}
				else
					s.detailsError = s.detailsRequest->GetError();
				s.wantedScenery = 0;
				s.detailsRequest.reset();
			}

			if (s.selectedCode.empty())
			{
				EditorUI::EmptyState("Pick an airport on the left (or type its code and press Enter) to see its Gateway submissions.\n\n"
					"Cloning one makes a project of your own from it, in a folder outside X-Plane's Custom Scenery: "
					"the airport and its scenery become editable items, and X-Plane keeps flying the version it has.");
				return;
			}
			if (s.detailsRequest)
			{
				DrawRequestStatus(*s.detailsRequest, TrF("Looking up {}", s.selectedCode));
				return;
			}
			if (!s.detailsError.empty() || !s.details)
			{
				ImGui::TextColored(Color(EditorTheme::Palette::Error), "%s: %s", s.selectedCode.c_str(), s.detailsError.empty() ? Tr("not on the Gateway") : s.detailsError.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton(TrL("Retry##hubDetails")))
					SelectAirport(s.selectedCode);
				return;
			}

			const GatewayAirportDetails &details = *s.details;
			UI::Fonts::PushFont("Bold");
			ImGui::TextUnformatted((details.airport.code + "  " + details.airport.name).c_str());
			UI::Fonts::PopFont();
			ImGui::SameLine();
			if (ImGui::SmallButton(EditorUI::IconLabel(SEDX_ICON_EXTERNAL_LINK, "Website").c_str()))
				IO::FileSystem::OpenUrl(GetGatewayAirportPageUrl(details.airport.code));
			EditorUI::ItemTooltip("The airport's page on gateway.x-plane.com");
			ImGui::TextDisabled("%s", TrNF("{} submission", "{} submissions", static_cast<std::int64_t>(details.scenery.size()), details.scenery.size()).c_str());
			if (!details.airport.checkedOutBy.empty())
			{
				ImGui::SameLine();
				ImGui::TextColored(Color(EditorTheme::Palette::Warning), "%s", TrF("Checked out by {}", details.airport.checkedOutBy).c_str());
				EditorUI::ItemTooltip("Another artist is working on this airport for the Gateway: a clone is still yours to edit");
			}

			// Submissions, newest first
			const GatewayScenery *recommended = details.GetRecommended();
			const float tableHeight = std::max(120.0f * dpi, ImGui::GetContentRegionAvail().y - 250.0f * dpi);
			constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
			if (ImGui::BeginTable("##hubSubmissions", 5, flags, ImVec2(0.0f, tableHeight)))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn(Tr("Submission"), ImGuiTableColumnFlags_WidthFixed, 90.0f * dpi);
				ImGui::TableSetupColumn(Tr("Type"), ImGuiTableColumnFlags_WidthFixed, 36.0f * dpi);
				ImGui::TableSetupColumn(Tr("Artist"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
				ImGui::TableSetupColumn(Tr("Uploaded"), ImGuiTableColumnFlags_WidthFixed, 84.0f * dpi);
				ImGui::TableSetupColumn(Tr("Status"), ImGuiTableColumnFlags_WidthFixed, 80.0f * dpi);
				ImGui::TableHeadersRow();
				for (size_t n = details.scenery.size(); n-- > 0;)
				{
					const GatewayScenery &scenery = details.scenery[n];
					const bool isRecommended = recommended && recommended->sceneryId == scenery.sceneryId;
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::PushID(scenery.sceneryId);
					const std::string label = (isRecommended ? std::string(SEDX_ICON_STAR) + " " : std::string()) + std::to_string(scenery.sceneryId);
					if (ImGui::Selectable(label.c_str(), s.selectedScenery == scenery.sceneryId, ImGuiSelectableFlags_SpanAllColumns))
						s.selectedScenery = scenery.sceneryId;
					if (ImGui::IsItemHovered())
					{
						std::string comments = scenery.artistComments;
						if (!scenery.moderatorComments.empty())
							comments += (comments.empty() ? "" : "\n") + TrF("Moderator: {}", scenery.moderatorComments);
						if (isRecommended)
							comments = std::string(Tr("Recommended: the submission X-Plane ships")) + (comments.empty() ? "" : "\n\n") + comments;
						if (!comments.empty())
							ImGui::SetTooltip("%s", comments.c_str());
					}
					ImGui::PopID();
					ImGui::TableNextColumn();
					if (scenery.Is3D())
						ImGui::TextColored(Color(EditorTheme::Palette::Accent), "3D");
					else
						ImGui::TextUnformatted(scenery.type.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(scenery.userName.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(GatewayScenery::ShortDate(scenery.dateUploaded).c_str());
					ImGui::TableNextColumn();
					const ImVec4 statusColor = scenery.IsDeclined() ? EditorTheme::Palette::Error : scenery.IsApproved() ? EditorTheme::Palette::Accent : EditorTheme::Palette::Text;
					ImGui::TextColored(Color(statusColor), "%s", scenery.status.c_str());
				}
				ImGui::EndTable();
			}

			// Where the clone goes
			ImGui::SeparatorText(Tr("Clone into"));
			const auto submission = std::ranges::find(details.scenery, s.selectedScenery, &GatewayScenery::sceneryId);
			ProjectFile::GatewayCloneSpec spec;
			spec.location = VFS::FromUtf8(s.cloneLocation);
			spec.projectName = s.cloneName;
			const std::string defaultName = ProjectFile::GetGatewayCloneName({}, details.airport.code, details.airport.name);
			const fs::path defaultFolder = DefaultCloneFolder();
			std::vector<ProjectFile::FieldProblem> problems = ProjectFile::ValidateGatewayClone(spec, details.airport.code, details.airport.name);
			std::error_code ec;
			const bool createDefault = !defaultFolder.empty() && VFS::IsInside(spec.location, defaultFolder) && VFS::IsInside(defaultFolder, spec.location) &&
				!fs::exists(spec.location, ec);
			if (createDefault)	// Made when the clone starts
				std::erase_if(problems, [](const ProjectFile::FieldProblem &problem) { return problem.field == "location"; });

			const auto problemOf = [&problems](const char *field) -> const ProjectFile::FieldProblem *
			{
				const auto found = std::ranges::find(problems, std::string_view(field), &ProjectFile::FieldProblem::field);
				return found == problems.end() ? nullptr : &*found;
			};
			if (EditorUI::BeginPropertyGrid("##hubClone", 120.0f))
			{
				EditorUI::PropertyLabel("Folder");
				const float browseWidth = ImGui::CalcTextSize(Tr("Browse...")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
				ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
				ImGui::InputText("##hubCloneLocation", &s.cloneLocation);
				ImGui::SameLine();
				if (ImGui::Button(TrL("Browse...##hubCloneLocation")))
				{
					const std::string chosen = pfd::select_folder(Tr("Choose the folder the clone is made in"), s.cloneLocation).result();
					if (!chosen.empty())
						s.cloneLocation = chosen;
				}
				if (const ProjectFile::FieldProblem *problem = problemOf("location"))
				{
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextColored(Color(EditorTheme::Palette::Error), "%s", problem->message.c_str());
					ImGui::PopTextWrapPos();
				}

				EditorUI::PropertyLabel("Project name");
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputTextWithHint("##hubCloneName", defaultName.c_str(), &s.cloneName);
				EditorUI::ItemTooltip("The project folder and the project file are named after it (empty: \"ICAO - Name\")");
				if (const ProjectFile::FieldProblem *problem = problemOf("projectName"))
				{
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextColored(Color(EditorTheme::Palette::Error), "%s", problem->message.c_str());
					ImGui::PopTextWrapPos();
				}
				EditorUI::EndPropertyGrid();
			}

			const fs::path folder = ProjectFile::GetGatewayCloneFolder(spec, details.airport.code, details.airport.name);
			ImGui::TextColored(Color(EditorTheme::Palette::TextLabel), "%s  %s", SEDX_ICON_FOLDER_O, VFS::ToUtf8(folder).c_str());
			const fs::path customScenery = CustomSceneryFolder();
			const bool inCustomScenery = !customScenery.empty() && !spec.location.empty() && VFS::IsInside(spec.location, customScenery);
			if (inCustomScenery)
			{
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextColored(Color(EditorTheme::Palette::Warning), "%s  %s", SEDX_ICON_EXCLAMATION_TRIANGLE,
					Tr("This is inside X-Plane's Custom Scenery: X-Plane will load the copy next to the airport it already has. Pick a folder elsewhere to work on it on its own."));
				ImGui::PopTextWrapPos();
			}

			// Clone
			if (s.clone)
			{
				if (s.clone->request && !s.clone->request->IsFinished())
				{
					DrawRequestStatus(*s.clone->request, TrF("Downloading {}", s.clone->label));
					ImGui::SameLine();
					if (ImGui::SmallButton(TrL("Cancel##hubClone")))
					{
						s.clone->request->Cancel();
						s.clone.reset();
					}
				}
				else
					DrawBusy(TrF("Making the project of {}", s.clone->label));	// Unzipping, importing, writing: can't be stopped halfway
				return;
			}
			// The name the folder above shows, whatever name the downloaded submission gives its airport
			spec.projectName = ProjectFile::GetGatewayCloneName(spec, details.airport.code, details.airport.name);
			ImGui::BeginDisabled(submission == details.scenery.end() || !problems.empty());
			const std::string cloneLabel = submission == details.scenery.end() ? std::string(Tr("Clone and Open")) : TrF("Clone {} and Open", submission->sceneryId);
			if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_CLOUD_DOWNLOAD, cloneLabel).c_str(), ImVec2(0.0f, 0.0f)))
			{
				s.cloneError.clear();
				if (createDefault)
				{
					fs::create_directories(spec.location, ec);
					if (ec)
						s.cloneError = TrF("Could not create {}: {}", VFS::ToUtf8(spec.location), ec.message());
				}
				if (s.cloneError.empty())
					StartClone(submission->sceneryId, spec, details.airport.code + " #" + std::to_string(submission->sceneryId), false);
			}
			ImGui::EndDisabled();
			EditorUI::ItemTooltip("Download the submission, make its project folder and open the project");
			if (!s.cloneError.empty())
			{
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextColored(Color(EditorTheme::Palette::Error), "%s  %s", SEDX_ICON_EXCLAMATION_TRIANGLE, s.cloneError.c_str());
				ImGui::PopTextWrapPos();
			}
		}

		void DrawGatewayPage()
		{
			const float dpi = ImGui::GetStyle().FontScaleDpi;
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(Color(EditorTheme::Palette::TextLabel), "%s", Tr("Clone an airport from the X-Plane Scenery Gateway into a project folder of your own, outside Custom Scenery."));
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			const float listWidth = std::max(260.0f * dpi, ImGui::GetContentRegionAvail().x * 0.42f);
			if (ImGui::BeginChild("##hubGatewayList", ImVec2(listWidth, 0.0f)))
				DrawAirportList();
			ImGui::EndChild();
			ImGui::SameLine();
			if (ImGui::BeginChild("##hubGatewayDetails", ImVec2(0.0f, 0.0f)))
				DrawAirportDetails();
			ImGui::EndChild();
		}

		// -------------------------------------------------------
		// Recent Projects

		const char *FormatIcon(const std::string &path)
		{
			std::string lower = path;
			std::ranges::transform(lower, lower.begin(), [](const char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
			if (lower.ends_with(".wed.xml"))
				return SEDX_ICON_CODE;
			if (lower.ends_with(".sex"))
				return SEDX_ICON_FILE;
			return SEDX_ICON_FILE_TEXT_O;
		}

		void OpenRecent(const std::string &path, EditorContext *context)
		{
			OpenProject(VFS::FromUtf8(path), context);
		}

		void DrawRecentPage(EditorContext *context)
		{
			State &s = S();
			const float dpi = ImGui::GetStyle().FontScaleDpi;
			std::vector<RecentProject> recent;
			if (s.preferences)
			{
				for (const auto &[time, project] : s.preferences->GetRecentProjects())
					recent.push_back(project);
			}

			if (recent.empty())
			{
				EditorUI::EmptyState("No recent projects yet.\n\nStart a new airport project, clone an airport from the Scenery Gateway, or open a project file (.edX, .sex or a WorldEditor earth.wed.xml).");
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f * dpi);
				if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_PLUS, "New Airport Project").c_str()))
					SelectPage(Page::NewProject);
				ImGui::SameLine();
				if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_CLOUD_DOWNLOAD, "Clone from Gateway").c_str()))
					SelectPage(Page::Gateway);
				ImGui::SameLine();
				if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_FOLDER_OPEN, "Open Project...").c_str()))
				{
					Close();
					EditorProjects::BrowseOpen(context);
				}
				return;
			}

			EditorUI::SearchFilter("##hubRecentSearch", s.recentFilter, ImGui::GetContentRegionAvail().x * 0.5f);
			EditorUI::ItemTooltip("Name or folder");
			size_t missing = 0;
			for (const RecentProject &project : recent)
				missing += s.recentExists.contains(project.filePath) && !s.recentExists[project.filePath] ? 1 : 0;
			if (missing > 0)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton(TrNF("Remove {} missing", "Remove {} missing", static_cast<std::int64_t>(missing), missing).c_str()))
				{
					for (const RecentProject &project : recent)
					{
						if (s.recentExists.contains(project.filePath) && !s.recentExists[project.filePath])
							s.preferences->RemoveRecentProject(project.filePath);
					}
					s.preferences->SavePreferences();
					return;
				}
				EditorUI::ItemTooltip("Take the projects whose file is gone off the list (nothing is deleted)");
			}

			const ProjectFile::ProjectDocument *document = EditorProjects::GetDocument();
			const std::string openPath = document && document->HasFile() ? VFS::ToUtf8(document->GetPath()) : std::string();
			const float buttonsHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
			constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
			std::string remove;
			if (ImGui::BeginTable("##hubRecent", 3, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y - buttonsHeight)))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn(Tr("Project"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
				ImGui::TableSetupColumn(Tr("Folder"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
				ImGui::TableSetupColumn(Tr("Opened"), ImGuiTableColumnFlags_WidthFixed, 120.0f * dpi);
				ImGui::TableHeadersRow();
				for (const RecentProject &project : recent)
				{
					const fs::path path = VFS::FromUtf8(project.filePath);
					const std::string folder = VFS::ToUtf8(path.parent_path());
					const std::string name = project.name.empty() ? VFS::ToUtf8(path.stem()) : project.name;
					if (!s.recentFilter.PassFilter(name.c_str()) && !s.recentFilter.PassFilter(project.filePath.c_str()))
						continue;
					const bool exists = !s.recentExists.contains(project.filePath) || s.recentExists[project.filePath];
					const bool isOpen = !openPath.empty() && VFS::IsInside(path, VFS::FromUtf8(openPath)) && VFS::IsInside(VFS::FromUtf8(openPath), path);

					ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 1.4f);
					ImGui::TableNextColumn();
					ImGui::PushID(project.filePath.c_str());
					if (!exists)
						ImGui::PushStyleColor(ImGuiCol_Text, Color(EditorTheme::Palette::TextDisabled));
					const std::string label = std::string(FormatIcon(project.filePath)) + "  " + name;
					if (ImGui::Selectable(label.c_str(), s.selectedRecent == project.filePath, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
						ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 1.2f)))
					{
						s.selectedRecent = project.filePath;
						if (exists && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
							OpenRecent(project.filePath, context);
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", exists ? project.filePath.c_str() : TrF("{}\nThe file isn't there any more", project.filePath).c_str());
					if (ImGui::BeginPopupContextItem("##recentMenu"))
					{
						s.selectedRecent = project.filePath;
						if (ImGui::MenuItem(EditorUI::IconLabel(SEDX_ICON_FOLDER_OPEN, "Open").c_str(), nullptr, false, exists))
							OpenRecent(project.filePath, context);
						if (ImGui::MenuItem(EditorUI::IconLabel(SEDX_ICON_FOLDER_O, "Show in Folder").c_str(), nullptr, false, exists))
							IO::FileSystem::OpenDirectoryInExplorer(path.parent_path());
						ImGui::Separator();
						if (ImGui::MenuItem(EditorUI::IconLabel(SEDX_ICON_TIMES, "Remove from List").c_str()))
							remove = project.filePath;
						ImGui::EndPopup();
					}
					if (!exists)
						ImGui::PopStyleColor();
					if (isOpen)
					{
						ImGui::SameLine();
						ImGui::TextColored(Color(EditorTheme::Palette::Accent), "%s", Tr("open"));
					}
					else if (!exists)
					{
						ImGui::SameLine();
						ImGui::TextColored(Color(EditorTheme::Palette::Warning), "%s", Tr("missing"));
					}
					ImGui::PopID();
					ImGui::TableNextColumn();
					ImGui::TextColored(Color(EditorTheme::Palette::TextLabel), "%s", folder.c_str());
					ImGui::TableNextColumn();
					ImGui::TextColored(Color(EditorTheme::Palette::TextLabel), "%s", FormatLocalTime(project.lastOpened).c_str());
				}
				ImGui::EndTable();
			}
			if (!remove.empty())
			{
				s.preferences->RemoveRecentProject(remove);
				s.preferences->SavePreferences();
				if (s.selectedRecent == remove)
					s.selectedRecent.clear();
			}

			// The selected project
			const auto selected = std::ranges::find(recent, s.selectedRecent, &RecentProject::filePath);
			const bool selectedExists = selected != recent.end() && (!s.recentExists.contains(selected->filePath) || s.recentExists[selected->filePath]);
			ImGui::BeginDisabled(!selectedExists);
			if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_FOLDER_OPEN, "Open").c_str(), ImVec2(120.0f * dpi, 0.0f)) ||
				(selectedExists && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)))
				OpenRecent(selected->filePath, context);
			ImGui::SameLine();
			if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_FOLDER_O, "Show in Folder").c_str()))
				IO::FileSystem::OpenDirectoryInExplorer(VFS::FromUtf8(selected->filePath).parent_path());
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(selected == recent.end());
			if (ImGui::Button(EditorUI::IconLabel(SEDX_ICON_TIMES, "Remove from List").c_str()))
			{
				s.preferences->RemoveRecentProject(selected->filePath);
				s.preferences->SavePreferences();
				s.selectedRecent.clear();
			}
			ImGui::EndDisabled();
			EditorUI::ItemTooltip("Take the project off this list (its files stay where they are)");
		}

		// -------------------------------------------------------

		void DrawNewPage(EditorContext *context)
		{
			if (ProjectDialogs::DrawNewProjectForm(context) != ProjectDialogs::FormResult::Created)
				return;
			State &s = S();
			if (const fs::path waiting = EditorProjects::GetWaitingProject(); !waiting.empty())
				s.opening = VFS::ToUtf8(waiting.filename());
			else
				Close();
		}

		/* @brief One entry of the left column. */
		bool NavItem(const char *icon, const Text label, const bool selected)
		{
			const float dpi = ImGui::GetStyle().FontScaleDpi;
			const std::string text = EditorUI::IconLabel(icon, label);
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
			if (selected)
				ImGui::PushStyleColor(ImGuiCol_Header, Color(EditorTheme::Palette::FrameActive));
			const bool clicked = ImGui::Selectable(text.c_str(), selected, ImGuiSelectableFlags_None, ImVec2(0.0f, 30.0f * dpi));
			if (selected)
			{
				const ImVec2 min = ImGui::GetItemRectMin();
				const ImVec2 max = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + 3.0f * dpi, max.y), EditorTheme::ToU32(EditorTheme::Palette::Accent));
				ImGui::PopStyleColor();
			}
			ImGui::PopStyleVar();
			return clicked;
		}

		void DrawNavigation(EditorContext *context)
		{
			State &s = S();
			const float dpi = ImGui::GetStyle().FontScaleDpi;
			ImGui::Dummy(ImVec2(0.0f, 4.0f * dpi));
			UI::Fonts::PushFont("Bold");
			ImGui::TextUnformatted(PRODUCT_NAME);
			UI::Fonts::PopFont();
			ImGui::TextDisabled("%s", SEDX_VERSION_STRING);
			ImGui::Dummy(ImVec2(0.0f, 8.0f * dpi));

			if (NavItem(SEDX_ICON_HISTORY, "Recent Projects", s.page == Page::Recent))
				SelectPage(Page::Recent);
			if (NavItem(SEDX_ICON_PLUS, "New Project", s.page == Page::NewProject))
				SelectPage(Page::NewProject);
			if (NavItem(SEDX_ICON_CLOUD_DOWNLOAD, "Clone from Gateway", s.page == Page::Gateway))
				SelectPage(Page::Gateway);
			ImGui::Dummy(ImVec2(0.0f, 6.0f * dpi));
			ImGui::Separator();
			ImGui::Dummy(ImVec2(0.0f, 6.0f * dpi));
			if (NavItem(SEDX_ICON_FOLDER_OPEN, "Open Project...", false))
			{
				Close();
				EditorProjects::BrowseOpen(context);
			}
			EditorUI::ItemTooltip("Open a project file: .edX, .sex or a WorldEditor earth.wed.xml");
		}
	}

	// -------------------------------------------------------

	void Init(const Ref<UserPreferences> &preferences)
	{
		State &s = S();
		s.preferences = preferences;

		// Scripted runs (self-tests, captures, --project...) pass flags and must not stop at a modal; --project-hub shows it anyway
		bool flags = false, force = false;
		if (const PlatformContext *platform = Application::Get().GetPlatformContext())
		{
			const std::vector<std::string> &args = platform->GetCommandLineArgs();
			for (size_t i = 0; i < args.size(); ++i)
			{
				const std::string &arg = args[i];
				flags |= arg.starts_with("--");
				if (arg == "--project-hub")
				{
					force = true;
					const std::string page = i + 1 < args.size() && !args[i + 1].starts_with("--") ? args[++i] : std::string();
					s.startupPage = page == "new" ? Page::NewProject : page == "gateway" ? Page::Gateway : Page::Recent;
					if (s.startupPage == Page::Gateway && i + 1 < args.size() && !args[i + 1].starts_with("--"))
						s.startupAirport = args[++i];
				}
				else if (arg == "--gateway-clone" && i + 1 < args.size())
				{
					// "<scenery id>[;folder]"
					const std::string &value = args[++i];
					const size_t semicolon = value.find(';');
					s.cliSceneryId = std::atoi(value.substr(0, semicolon).c_str());
					if (semicolon != std::string::npos)
						s.cliFolder = value.substr(semicolon + 1);
					if (s.cliSceneryId <= 0)
						EDITOR_ERROR_TAG(LOG_TAG, "--gateway-clone wants \"<scenery id>[;folder]\", not '{}'", value);
				}
			}
		}
		s.startupPending = force || (!flags && preferences && preferences->GetShowWelcomeScreen());
	}

	void Shutdown()
	{
		State &s = S();
		if (s.clone && s.clone->request)
			s.clone->request->Cancel();
		if (s.clone)
			s.clone->cancel->store(true, std::memory_order_relaxed);	// A clone being written stops at its next tile or file and takes back what it wrote
		s.clone.reset();		// The job's future waits for that
		s.listRequest.reset();
		s.detailsRequest.reset();
		s.details.reset();
		s.resultsList.reset();
		s.preferences = nullptr;
	}

	void Open(const Page page)
	{
		State &s = S();
		s.page = page;
		s.openRequested = true;
	}

	void OpenGateway(const std::string &airportCode, const int sceneryId)
	{
		State &s = S();
		Open(Page::Gateway);
		if (airportCode.empty())
			return;
		snprintf(s.gatewayFilter.InputBuf, sizeof(s.gatewayFilter.InputBuf), "%s", airportCode.c_str());
		s.gatewayFilter.Build();
		if (s.selectedCode == airportCode && s.details)
		{
			if (sceneryId != 0)
				s.selectedScenery = PickScenery(*s.details, sceneryId);
			return;
		}
		SelectAirport(airportCode);
		s.wantedScenery = sceneryId;
	}

	void Tick(EditorContext *context)
	{
		State &s = S();

		// --gateway-clone: once (the Gateway client is up by now)
		if (s.cliSceneryId > 0 && !s.clone)
		{
			ProjectFile::GatewayCloneSpec spec;
			spec.location = s.cliFolder.empty() ? VFS::FromUtf8(CloneLocation()) : VFS::FromUtf8(s.cliFolder);
			std::error_code ec;
			fs::create_directories(spec.location, ec);
			StartClone(s.cliSceneryId, spec, "#" + std::to_string(s.cliSceneryId), true);
			s.cliSceneryId = 0;
		}
		FinishClone(context);
	}

	void Draw(EditorContext *context)
	{
		State &s = S();

		// At startup: once the main window is up, after the keymap welcome, and when nothing else is asked for
		if (s.startupPending && ++s.startupFrames > STARTUP_FRAMES && !EditorWelcome::IsShowing() &&
			!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
		{
			s.startupPending = false;
			if (!s.startupAirport.empty())
				OpenGateway(s.startupAirport);
			else if (!EditorProjects::GetDocument() && EditorProjects::GetWaitingProject().empty() && !s.clone)
				Open(s.startupPage);
		}

		if (s.openRequested)
		{
			s.openRequested = false;
			ImGui::OpenPopup(HUB_ID);
			OnOpened();
		}

		const float dpi = ImGui::GetStyle().FontScaleDpi;
		const ImGuiViewport *viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(std::min(1100.0f * dpi, viewport->WorkSize.x * 0.92f), std::min(700.0f * dpi, viewport->WorkSize.y * 0.9f)), ImGuiCond_Appearing);
		ImGui::SetNextWindowSizeConstraints(ImVec2(760.0f * dpi, 460.0f * dpi), ImVec2(FLT_MAX, FLT_MAX));
		bool open = true;
		// TRANSLATORS: The window the editor starts on: recent projects, a new project, a Gateway airport to clone
		if (!ImGui::BeginPopupModal(TrL("Project Hub###ProjectHub"), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse))
		{
			if (s.isOpen)
				OnClosed();		// Closed from outside (another modal, a layout reset)
			return;
		}

		const ImGuiStyle &style = ImGui::GetStyle();
		const float footer = ImGui::GetFrameHeight() + style.ItemSpacing.y * 3.0f + style.WindowPadding.y * 0.5f;	// Separator, spacing, the footer's row
		const float navWidth = 220.0f * dpi;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(EditorTheme::Palette::PanelHeader));
		if (ImGui::BeginChild("##hubNavigation", ImVec2(navWidth, -footer), ImGuiChildFlags_AlwaysUseWindowPadding))
			DrawNavigation(context);
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::BeginChild("##hubPage", ImVec2(0.0f, -footer), ImGuiChildFlags_AlwaysUseWindowPadding))
		{
			// A project with unsaved changes is asked about before another one opens; say so up front
			if (EditorProjects::IsDirty())
			{
				ImGui::TextColored(Color(EditorTheme::Palette::Warning), "%s  %s", SEDX_ICON_EXCLAMATION_TRIANGLE, Tr("The open project has unsaved changes: you'll be asked about them before another project opens."));
				ImGui::Spacing();
			}
			switch (s.page)
			{
				case Page::Recent:
					UI::Fonts::PushFont("Bold");
					ImGui::TextUnformatted(Tr("Recent Projects"));
					UI::Fonts::PopFont();
					ImGui::Spacing();
					DrawRecentPage(context);
					break;
				case Page::NewProject:
					UI::Fonts::PushFont("Bold");
					ImGui::TextUnformatted(Tr("New Project"));
					UI::Fonts::PopFont();
					DrawNewPage(context);
					break;
				case Page::Gateway:
					UI::Fonts::PushFont("Bold");
					ImGui::TextUnformatted(Tr("Clone from the Scenery Gateway"));
					UI::Fonts::PopFont();
					DrawGatewayPage();
					break;
			}
		}
		ImGui::EndChild();

		// A project waiting for the library index: the hub closes once it's open
		if (!s.opening.empty() && EditorProjects::GetWaitingProject().empty())
		{
			if (EditorProjects::GetDocument())
				s.closeRequested = true;
			else
				s.openError = TrF("{} couldn't be opened: the CONSOLE panel says why", s.opening);
			s.opening.clear();
		}

		// Footer: show at startup, and the way out
		ImGui::Separator();
		ImGui::Spacing();
		if (s.preferences)
		{
			bool showAtStartup = s.preferences->GetShowWelcomeScreen();
			if (ImGui::Checkbox(TrL("Show at startup##hubStartup"), &showAtStartup))
			{
				s.preferences->SetShowWelcomeScreen(showAtStartup);
				s.preferences->SavePreferences();
			}
			EditorUI::ItemTooltip("Open the Project Hub when the editor starts (File > Project Hub... opens it any time)");
		}
		if (!s.opening.empty())
		{
			static const char *spinner = "|/-\\";
			ImGui::SameLine(0.0f, 24.0f * dpi);
			ImGui::TextColored(Color(EditorTheme::Palette::Warning), "%c %s", spinner[static_cast<int>(ImGui::GetTime() * 8.0) & 3],
				TrF("Opening {} once the X-Plane library is indexed", s.opening).c_str());
		}
		else if (!s.openError.empty())
		{
			ImGui::SameLine(0.0f, 24.0f * dpi);
			ImGui::TextColored(Color(EditorTheme::Palette::Error), "%s  %s", SEDX_ICON_EXCLAMATION_TRIANGLE, s.openError.c_str());
		}
		const Text closeLabel = EditorProjects::GetDocument() ? Text("Close") : Text("Continue Without a Project");
		const float closeWidth = ImGui::CalcTextSize(closeLabel.Get()).x + style.FramePadding.x * 2.0f + 24.0f * dpi;
		ImGui::SameLine(ImGui::GetContentRegionMax().x - closeWidth);
		const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (ImGui::Button(TrL(closeLabel), ImVec2(closeWidth, 0.0f)) || escape)
			s.closeRequested = true;

		if (!open || s.closeRequested)
		{
			s.closeRequested = false;
			ImGui::CloseCurrentPopup();
			OnClosed();
		}
		ImGui::EndPopup();
	}

	bool IsOpen()
	{
		const State &s = S();
		return s.isOpen || s.openRequested;
	}
}

// -------------------------------------------------------
