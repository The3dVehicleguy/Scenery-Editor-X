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
 * gateway_clone.cpp
 * -------------------------------------------------------
 * Created: 17/11/2026
 * -------------------------------------------------------
 */
#include "gateway_clone.h"
#include "project_dsf.h"
#include "project_io.h"
#include <SceneryEditorX/filesystem/vfs.h>
#include <SceneryEditorX/xplane/xp_apt.h>
#include <SceneryEditorX/xplane/xp_dsf.h>
#include <algorithm>
#include <set>
#include <system_error>

// -------------------------------------------------------

namespace SceneryEditorX::ProjectFile
{
	namespace fs = std::filesystem;

	namespace
	{
		constexpr const char *CLONE = "clone";		// The clone's folder, mounted while it's written
		constexpr double FEET_TO_METERS = 0.3048;

		bool Fail(std::string *error, std::string message)
		{
			if (error)
				*error = std::move(message);
			return false;
		}

		std::string ToLowerAscii(std::string_view text)
		{
			std::string out(text);
			for (char &c : out)
			{
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			}
			return out;
		}

		std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
				text.remove_prefix(1);
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
				text.remove_suffix(1);
			return text;
		}

		/* @brief "Earth nav data/..." in any spelling, with forward slashes. */
		std::string NormalizedName(const std::string &name)
		{
			std::string lower = ToLowerAscii(name);
			std::ranges::replace(lower, '\\', '/');
			return lower;
		}

		bool IsAptDat(const XPlane::ZipEntry &entry)
		{
			return NormalizedName(entry.name) == "earth nav data/apt.dat";
		}

		bool IsDsfTile(const XPlane::ZipEntry &entry)
		{
			const std::string name = NormalizedName(entry.name);
			return name.starts_with("earth nav data/") && name.ends_with(".dsf");
		}
	}

	// -------------------------------------------------------

	std::string GetGatewayCloneName(const GatewayCloneSpec &spec, const std::string_view icao, const std::string_view airportName)
	{
		const std::string_view name = Trim(spec.projectName);
		if (!name.empty())
			return std::string(name);
		return MakeDefaultProjectName(Trim(icao), Trim(airportName));
	}

	fs::path GetGatewayCloneFolder(const GatewayCloneSpec &spec, const std::string_view icao, const std::string_view airportName)
	{
		return spec.location / VFS::FromUtf8(GetGatewayCloneName(spec, icao, airportName));
	}

	std::vector<FieldProblem> ValidateGatewayClone(const GatewayCloneSpec &spec, const std::string_view icao, const std::string_view airportName)
	{
		std::vector<FieldProblem> problems;
		const auto problem = [&problems](const char *field, std::string message) { problems.push_back({ field, std::move(message) }); };

		const std::string name = GetGatewayCloneName(spec, icao, airportName);
		bool nameValid = false;
		if (name.empty())
			problem("projectName", "Enter the project's name");
		else if (const std::string why = VFS::CheckPortableName(name); !why.empty())
			problem("projectName", why);
		else
			nameValid = true;

		std::error_code ec;
		if (spec.location.empty())
			problem("location", "Choose the folder the project goes in");
		else if (!fs::is_directory(spec.location, ec))
			problem("location", VFS::ToUtf8(spec.location) + " doesn't exist");
		else if (nameValid)
		{
			const fs::path folder = GetGatewayCloneFolder(spec, icao, airportName);
			if (fs::exists(folder, ec) && (!fs::is_directory(folder, ec) || !fs::is_empty(folder, ec)))
				problem("projectName", VFS::ToUtf8(folder.filename()) + " already exists there: choose another name or location");
		}
		return problems;
	}

	bool MakeGatewayCloneProject(const GatewaySubmission &submission, const GatewayCloneSpec &spec, SceneryProject &out, GatewayCloneReport &report, std::string *error)
	{
		report = {};

		// The airport
		const auto aptEntry = std::ranges::find_if(submission.files, IsAptDat);
		if (aptEntry == submission.files.end())
			return Fail(error, "The submission holds no Earth nav data/apt.dat");
		XPlane::AptFile apt;
		std::string reason;
		if (!XPlane::ParseAptDat(std::string_view(reinterpret_cast<const char *>(aptEntry->data.data()), aptEntry->data.size()), apt, &reason))
			return Fail(error, "Earth nav data/apt.dat: " + reason);
		if (apt.airports.empty())
			return Fail(error, "Earth nav data/apt.dat holds no airport");
		const std::string wanted = ToLowerAscii(Trim(submission.icao));
		const auto named = std::ranges::find_if(apt.airports, [&wanted](const XPlane::AptAirport &airport) { return ToLowerAscii(airport.id) == wanted; });
		const XPlane::AptAirport &airport = named != apt.airports.end() ? *named : apt.airports.front();
		if (named == apt.airports.end() && !wanted.empty())
		{
			// One airport under another code (the Gateway's code changed since): that's it. Several, none of them the
			// submission's: no way to tell which, so nothing is guessed
			if (apt.airports.size() > 1)
			{
				std::string ids;
				for (const XPlane::AptAirport &other : apt.airports)
					ids += (ids.empty() ? "" : ", ") + other.id;
				return Fail(error, "Earth nav data/apt.dat holds " + std::to_string(apt.airports.size()) + " airports and none is " +
					std::string(Trim(submission.icao)) + " (" + ids + ")");
			}
			report.warnings.push_back("apt.dat has no airport " + std::string(Trim(submission.icao)) + ": " + airport.id + " was taken");
		}
		if (apt.airports.size() > 1)
			report.warnings.push_back("apt.dat holds " + std::to_string(apt.airports.size()) + " airports: only " + airport.id + " was cloned");
		for (const std::string &warning : apt.warnings)
			report.warnings.push_back("apt.dat: " + warning);
		report.airportId = airport.id;

		// Its placements: every tile of the pack, read as one DSF
		XPlane::DsfFile overlay;
		std::vector<const XPlane::ZipEntry *> tiles;
		for (const XPlane::ZipEntry &entry : submission.files)
		{
			if (IsDsfTile(entry))
				tiles.push_back(&entry);
		}
		std::ranges::sort(tiles, {}, [](const XPlane::ZipEntry *entry) { return NormalizedName(entry->name); });	// Same order whatever the zip's
		for (const XPlane::ZipEntry *entry : tiles)
		{
			XPlane::DsfFile tile;
			if (!XPlane::ReadDsf(entry->data.data(), entry->data.size(), tile, &reason))
			{
				report.warnings.push_back(entry->name + " can't be read (" + reason + "): its placements weren't imported");
				continue;
			}
			XPlane::AppendDsf(overlay, tile);
			++report.tiles;
		}
		DsfImport imported = ImportDsfItems(overlay);
		report.items = imported.items.size();
		for (const std::string &warning : imported.warnings)
			report.warnings.push_back("DSF " + warning);

		// The project
		const std::string name = GetGatewayCloneName(spec, submission.icao.empty() ? airport.id : submission.icao, submission.airportName.empty() ? airport.name : submission.airportName);
		SceneryProject project = MakeNewProject(name);
		project.info.packageName = name;
		project.info.author = std::string(Trim(spec.author)).empty() ? submission.artist : std::string(Trim(spec.author));
		project.info.description = airport.name + " (" + airport.id + "), cloned from Scenery Gateway submission " + std::to_string(submission.sceneryId);
		if (!submission.artist.empty())
			project.info.description += " by " + submission.artist;
		if (!submission.dateUploaded.empty())
			project.info.description += ", uploaded " + submission.dateUploaded.substr(0, std::min<size_t>(10, submission.dateUploaded.size()));

		double latitude = 0.0, longitude = 0.0;
		if (airport.GetDatum(latitude, longitude))
		{
			project.datum.latitude = latitude;
			project.datum.longitude = longitude;
			project.datum.elevation = airport.elevationFt * FEET_TO_METERS;
			project.datum.valid = true;
		}

		ProjectAirport entry;
		entry.airport = airport;
		entry.sceneryId = submission.sceneryId > 0 ? submission.sceneryId : -1;
		project.airports.push_back(std::move(entry));

		SceneryItem item;
		item.kind = ItemKind::Airport;
		item.id = MakeItemId();
		item.name = airport.name.empty() ? airport.id : airport.name;
		item.airportId = airport.id;
		item.children = std::move(imported.items);
		project.items.push_back(std::move(item));
		NormalizeProject(project);

		out = std::move(project);
		return true;
	}

	bool CreateGatewayCloneProject(const GatewaySubmission &submission, const GatewayCloneSpec &spec, GatewayCloneReport &report, std::string *error)
	{
		report = {};

		// Everything that can fail without writing first: the project, the folder, and the file names
		SceneryProject project;
		if (!MakeGatewayCloneProject(submission, spec, project, report, error))
			return false;
		GatewayCloneSpec named = spec;
		named.projectName = project.info.name;	// The name it got (the airport's own id and name when the submission's are missing)
		if (const std::vector<FieldProblem> problems = ValidateGatewayClone(named, submission.icao, submission.airportName); !problems.empty())
			return Fail(error, problems.front().message);
		std::vector<std::pair<std::string, const XPlane::ZipEntry *>> files;
		files.reserve(submission.files.size());
		std::set<std::string> names;	// Lowercase: Windows' and macOS' disks (and the VFS's lookups) don't tell README from readme
		for (const XPlane::ZipEntry &entry : submission.files)
		{
			if (entry.name.empty() || entry.name.back() == '/' || entry.name.back() == '\\')
				continue;	// A folder entry: folders are made for the files in them
			bool escaped = false;
			const std::string relative = VFS::NormalizeRelative(entry.name, &escaped);
			if (escaped || relative.empty() || fs::path(VFS::FromUtf8(entry.name)).has_root_path())
				return Fail(error, "The submission names a file outside its folder: " + entry.name);
			if (!names.insert(ToLowerAscii(relative)).second)
			{
				report.warnings.push_back(entry.name + " has the name of another file in the submission (but for case): only the first was copied");
				continue;
			}
			files.emplace_back(relative, &entry);
		}

		report.folder = GetGatewayCloneFolder(named, submission.icao, submission.airportName);

		// A clone that fails partway takes back what it wrote, so the same name can be tried again. The folder was missing or
		// empty when it was checked above, so everything in it now is this clone's. Declared before the VFS: it runs last.
		struct Undo
		{
			fs::path folder;
			bool existed = false;
			bool keep = false;

			~Undo()
			{
				if (keep)
					return;
				std::error_code ec;
				if (!existed)
				{
					fs::remove_all(folder, ec);
					return;
				}
				std::vector<fs::path> children;
				for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
					children.push_back(it->path());
				for (const fs::path &child : children)
					fs::remove_all(child, ec);
			}
		};
		Undo undo;
		undo.folder = report.folder;
		std::error_code ec;
		undo.existed = fs::exists(report.folder, ec);
		undo.keep = static_cast<bool>(ec);		// Can't tell what was there: leave it alone

		VFS::FileSystem vfs;
		if (!vfs.Mount(CLONE, report.folder, false, error))
			return false;
		for (const std::string &folder : GetPackFolders())
		{
			if (!vfs.CreateFolders(VFS::MakeVirtualPath(CLONE, folder), error))
				return false;
		}
		for (const auto &[relative, entry] : files)
		{
			const std::string_view bytes(reinterpret_cast<const char *>(entry->data.data()), entry->data.size());
			if (!vfs.Write(VFS::MakeVirtualPath(CLONE, relative), bytes, error))
				return false;
			++report.files;
		}

		report.projectFile = report.folder / VFS::FromUtf8(project.info.name + EDX_EXTENSION);
		std::vector<std::string> warnings;
		if (!SaveProjectFile(report.projectFile, project, ProjectFormat::Edx, &warnings, error))
			return false;
		for (std::string &warning : warnings)
			report.warnings.push_back(std::move(warning));
		WriteGitFiles(report.folder);
		undo.keep = true;
		return true;
	}
}

// -------------------------------------------------------
