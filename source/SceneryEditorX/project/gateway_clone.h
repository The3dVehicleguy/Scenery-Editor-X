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
 * gateway_clone.h
 * -------------------------------------------------------
 * Created: 17/11/2026
 * -------------------------------------------------------
 */
#pragma once
#include "scenery_pack.h"
#include "scenery_project.h"
#include <SceneryEditorX/xplane/xp_zip.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------

namespace SceneryEditorX::ProjectFile
{
	/**
	 * A Scenery Gateway submission cloned into a local working copy: a project folder of its own (anywhere, usually
	 * outside X-Plane's Custom Scenery, so the simulator doesn't load the copy while it's being worked on) laid out like a
	 * scenery pack, holding the submission's files as the Gateway sent them and a project made from them:
	 *
	 *   <folder>/<ICAO> - <Name>.edX          the airport (apt.dat rows) and the pack's DSF placements as editable items
	 *   <folder>/Earth nav data/apt.dat       the submission's files, as downloaded (README, COPYING... too)
	 *   <folder>/Earth nav data/+NN-NNN/...   (exporting the project to its own folder replaces apt.dat and the tiles)
	 *
	 * The project's airport records the submission it came from (ProjectAirport::sceneryId, WorldEditor's scenery_id:
	 * the parent of the next upload). Standard library only: the editor downloads and unzips the submission
	 * (XPlane::GatewayClient::UnpackScenery) and hands over its files.
	 */

	/* @brief A downloaded submission, unzipped. */
	struct GatewaySubmission
	{
		int sceneryId = 0;
		std::string icao;							// The airport's identifier on the Gateway ("KSEA", "XEN0026")
		std::string airportName;
		std::string artist;							// Who uploaded it
		std::string dateUploaded;					// ISO 8601
		std::string type;							// "2D" or "3D"
		std::vector<XPlane::ZipEntry> files;		// The pack's files, relative to its folder: "Earth nav data/apt.dat", DSF tiles, README...
	};

	/* @brief Where and how a submission is cloned (the clone form). */
	struct GatewayCloneSpec
	{
		std::filesystem::path location;				// Folder the project folder is created in
		std::string projectName;					// Project folder and file name; empty: "<ICAO> - <Name>"
		std::string author;							// The project's author (the submission's artist when empty)
	};

	/* @brief What a clone made. */
	struct GatewayCloneReport
	{
		std::filesystem::path folder;				// The project folder
		std::filesystem::path projectFile;			// <folder>/<name>.edX
		std::string airportId;
		size_t items = 0;							// Items made from the pack's DSF placements
		size_t tiles = 0;							// DSF tiles read
		size_t files = 0;							// Submission files written into the folder
		std::vector<std::string> warnings;			// Rows and placements left out or read in part
	};

	/* @brief The project name a clone gets: the spec's, else "<ICAO> - <Name>" made portable. */
	std::string GetGatewayCloneName(const GatewayCloneSpec &spec, std::string_view icao, std::string_view airportName);

	/* @brief The folder a clone creates: location / GetGatewayCloneName. */
	std::filesystem::path GetGatewayCloneFolder(const GatewayCloneSpec &spec, std::string_view icao, std::string_view airportName);

	/**
	 * @brief Everything wrong with a clone's spec, field by field ("projectName", "location"): a name that isn't a portable
	 * folder name, a location that doesn't exist, a project folder that exists already and isn't empty. Empty: it can be
	 * cloned.
	 */
	std::vector<FieldProblem> ValidateGatewayClone(const GatewayCloneSpec &spec, std::string_view icao, std::string_view airportName);

	/**
	 * @brief The project a submission becomes, without touching the disk: info (name, author, a description naming the
	 * submission), the datum at the airport's reference point, the airport from "Earth nav data/apt.dat" (the one with the
	 * submission's identifier; else its only airport, with a warning; without an identifier, the first) with its scenery
	 * id, and the placements of every DSF tile under "Earth nav data" (ImportDsfItems) inside the airport's item. Tiles
	 * that can't be read and placements that can't be imported are warnings; no apt.dat, no airport in it, or several
	 * airports none of which is the submission's fails.
	 */
	bool MakeGatewayCloneProject(const GatewaySubmission &submission, const GatewayCloneSpec &spec, SceneryProject &out, GatewayCloneReport &report,
		std::string *error = nullptr, const std::atomic<bool> *cancel = nullptr);

	/**
	 * @brief Creates the clone: ValidateGatewayClone, the project (MakeGatewayCloneProject), then the folder with the
	 * pack's folders, the submission's files (names that would leave the folder are refused, folder entries skipped, of
	 * names differing only in case the first is copied, and one named like the project file isn't, with warnings; a file
	 * named like another's folder fails), "<name>.edX" and the git helper files (kept when the submission has its own).
	 * Writes are atomic; nothing is written when the project can't be made, and a clone failing partway removes the files
	 * and folders it made (only those: anything else put in the folder meanwhile stays).
	 *
	 * @param cancel Set from another thread to stop the clone between DSF tiles and between files; it then fails ("Cancelled")
	 * and removes what it wrote. Null: it can't be cancelled.
	 */
	bool CreateGatewayCloneProject(const GatewaySubmission &submission, const GatewayCloneSpec &spec, GatewayCloneReport &report, std::string *error = nullptr,
		const std::atomic<bool> *cancel = nullptr);
}

// -------------------------------------------------------
