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
 * GatewayCloneTests.cpp
 * -------------------------------------------------------
 * Created: 17/11/2026
 * -------------------------------------------------------
 */
#include <catch2/catch_all.hpp>
#include <SceneryEditorX/filesystem/vfs.h>
#include <SceneryEditorX/project/gateway_clone.h>
#include <SceneryEditorX/project/project_dsf.h>
#include <SceneryEditorX/project/project_io.h>
#include <SceneryEditorX/project/scenery_pack.h>
#include <SceneryEditorX/xplane/xp_airport_document.h>
#include <SceneryEditorX/xplane/xp_apt.h>
#include <SceneryEditorX/xplane/xp_dsf.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace SceneryEditorX;
using namespace SceneryEditorX::ProjectFile;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

// -------------------------------------------------------

namespace
{
	constexpr double LAT = 45.58861111;		// KTST, a made-up field in tile +45-123
	constexpr double LON = -122.6;

	struct Scratch
	{
		fs::path path;

		Scratch()
		{
			path = fs::temp_directory_path() / ("sedx_clone_tests_" + MakeItemId());
			fs::create_directories(path);
		}

		~Scratch()
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	};

	std::string ReadText(const fs::path &path)
	{
		std::ifstream file(path, std::ios::binary);
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}

	XPlane::ZipEntry Entry(std::string name, const std::string_view bytes)
	{
		XPlane::ZipEntry entry;
		entry.name = std::move(name);
		entry.data.assign(bytes.begin(), bytes.end());
		return entry;
	}

	XPlane::ZipEntry Entry(std::string name, std::vector<uint8_t> bytes)
	{
		XPlane::ZipEntry entry;
		entry.name = std::move(name);
		entry.data = std::move(bytes);
		return entry;
	}

	GeoNode Node(const double latitude, const double longitude)
	{
		GeoNode node;
		node.latitude = latitude;
		node.longitude = longitude;
		return node;
	}

	SceneryItem Item(const ItemKind kind, const std::string &name, const std::string &resource)
	{
		SceneryItem item;
		item.kind = kind;
		item.id = MakeItemId();
		item.name = name;
		item.resource = resource;
		return item;
	}

	/* @brief What a Gateway pack's DSF places: a hangar, an apron polygon under a marking, a facade and an exclusion. */
	std::vector<SceneryItem> PackItems(const double latitude, const double longitude)
	{
		std::vector<SceneryItem> items;
		SceneryItem hangar = Item(ItemKind::Object, "hangar", "lib/airport/Ramp_Equipment/hangar.obj");
		hangar.latitude = latitude + 0.001;
		hangar.longitude = longitude + 0.001;
		hangar.heading = 90.0f;
		items.push_back(hangar);

		SceneryItem marking = Item(ItemKind::Polygon, "marking", "lib/airport/ground/markings/marking.pol");	// On top
		marking.rings = { { Node(latitude, longitude), Node(latitude, longitude + 0.001), Node(latitude + 0.001, longitude + 0.001), Node(latitude + 0.001, longitude) } };
		items.push_back(marking);
		SceneryItem apron = Item(ItemKind::Polygon, "apron", "lib/airport/pavement/asphalt_1D.pol");
		apron.textureHeading = 12.5f;
		apron.rings = { { Node(latitude, longitude), Node(latitude, longitude + 0.002), Node(latitude + 0.002, longitude + 0.002), Node(latitude + 0.002, longitude) } };
		items.push_back(apron);

		SceneryItem terminal = Item(ItemKind::Facade, "terminal", "lib/airport/buildings/terminal.fac");
		terminal.height = 12.0f;
		terminal.rings = { { Node(latitude + 0.003, longitude), Node(latitude + 0.003, longitude + 0.001), Node(latitude + 0.004, longitude + 0.001), Node(latitude + 0.004, longitude) } };
		items.push_back(terminal);

		SceneryItem exclusion = Item(ItemKind::ExclusionZone, "Exclusion zone", "");
		exclusion.exclusions = 1u;	// Objects
		exclusion.rings = { { Node(latitude - 0.01, longitude - 0.01), Node(latitude + 0.01, longitude + 0.01) } };
		items.push_back(exclusion);
		return items;
	}

	XPlane::DsfFile PackDsf(const std::vector<SceneryItem> &items)
	{
		SceneryProject project = MakeNewProject("pack");
		project.items = items;
		DsfConversionOptions options;
		options.includeObjects = true;
		DsfConversion conversion = BuildProjectDsf(project, options);
		REQUIRE(conversion.warnings.empty());
		return conversion.dsf;
	}

	std::vector<uint8_t> WriteTile(const XPlane::DsfFile &dsf)
	{
		std::vector<uint8_t> bytes;
		std::string error;
		REQUIRE(XPlane::WriteDsf(dsf, bytes, &error));
		return bytes;
	}

	std::string AptDat(const std::vector<XPlane::AptAirport> &airports)
	{
		XPlane::AptFile file;
		file.airports = airports;
		return XPlane::WriteAptDat(file);
	}

	XPlane::AptAirport TestAirport(const std::string &id = "KTST", const std::string &name = "Test Field")
	{
		XPlane::AptAirport airport = XPlane::MakeNewAirport(id, name, LAT, LON, XPlane::AirportType::Land, 120);
		airport.SetMetadata("datum_lat", FormatCoordinate(LAT));
		airport.SetMetadata("datum_lon", FormatCoordinate(LON));
		return airport;
	}

	/* @brief Submission 98765 of KTST by Test Artist, the way GatewayClient::UnpackScenery hands it over. */
	GatewaySubmission TestSubmission()
	{
		GatewaySubmission submission;
		submission.sceneryId = 98765;
		submission.icao = "KTST";
		submission.airportName = "Test Field";
		submission.artist = "Test Artist";
		submission.dateUploaded = "2026-11-01T10:20:30.000Z";
		submission.type = "3D";
		submission.files.push_back(Entry("Earth nav data/apt.dat", AptDat({ TestAirport() })));
		submission.files.push_back(Entry("Earth nav data/+40-130/+45-123.dsf", WriteTile(PackDsf(PackItems(LAT, LON)))));
		submission.files.push_back(Entry("README.txt", std::string_view("Gateway scenery, see COPYING\n")));
		submission.files.push_back(Entry("COPYING", std::string_view("GNU General Public License\n")));
		return submission;
	}

	const SceneryItem *AirportItem(const SceneryProject &project)
	{
		for (const SceneryItem &item : project.items)
		{
			if (item.kind == ItemKind::Airport)
				return &item;
		}
		return nullptr;
	}
}

// -------------------------------------------------------

TEST_CASE("Gateway clone: a submission becomes a project of its airport and DSF placements", "[project][gateway][clone]")
{
	const GatewaySubmission submission = TestSubmission();
	GatewayCloneSpec spec;
	spec.location = "/nowhere";		// Not touched: nothing is written

	SceneryProject project;
	GatewayCloneReport report;
	std::string error;
	REQUIRE(MakeGatewayCloneProject(submission, spec, project, report, &error));
	CHECK(report.airportId == "KTST");
	CHECK(report.tiles == 1);
	CHECK(report.items == 5);
	CHECK(report.warnings.empty());

	CHECK(project.info.name == "KTST - Test Field");
	CHECK(project.info.packageName == "KTST - Test Field");
	CHECK(project.info.author == "Test Artist");		// Nobody else named
	CHECK_THAT(project.info.description, ContainsSubstring("submission 98765") && ContainsSubstring("Test Artist") && ContainsSubstring("2026-11-01"));
	REQUIRE(project.datum.valid);
	CHECK(project.datum.latitude == Catch::Approx(LAT));
	CHECK(project.datum.longitude == Catch::Approx(LON));
	CHECK(project.datum.elevation == Catch::Approx(120 * 0.3048));

	REQUIRE(project.airports.size() == 1);
	CHECK(project.airports[0].airport.id == "KTST");
	CHECK(project.airports[0].sceneryId == 98765);		// The next upload's parent
	CHECK(project.airports[0].airport.runways.size() == TestAirport().runways.size());

	// One Airport item holding the placements, which export as the pack's DSF did (the marking still on top)
	REQUIRE(std::ranges::count_if(project.items, [](const SceneryItem &item) { return item.kind == ItemKind::Airport; }) == 1);
	const SceneryItem *airport = AirportItem(project);
	REQUIRE(airport);
	CHECK(airport->airportId == "KTST");
	REQUIRE(airport->children.size() == 5);
	CHECK(airport->children[0].kind == ItemKind::Object);
	CHECK(airport->children[1].name == "marking");
	CHECK(airport->children[2].name == "asphalt_1D");
	DsfConversionOptions options;
	options.includeObjects = true;
	const XPlane::DsfFile original = PackDsf(PackItems(LAT, LON));
	const DsfConversion again = BuildProjectDsf(project, options);
	CHECK(again.warnings.empty());
	REQUIRE(again.dsf.polygons.size() == original.polygons.size());
	CHECK(again.dsf.polygonDefinitions == original.polygonDefinitions);
	for (size_t i = 0; i < original.polygons.size(); ++i)
		CHECK(again.dsf.polygonDefinitions[again.dsf.polygons[i].definition] == original.polygonDefinitions[original.polygons[i].definition]);
	CHECK(again.dsf.objects.size() == 1);
	CHECK(again.dsf.GetExclusions().size() == 1);
	CHECK(ValidateProject(project).empty());

	// A name and author of one's own
	spec.projectName = "  My KTST  ";
	spec.author = "Me";
	REQUIRE(MakeGatewayCloneProject(submission, spec, project, report, &error));
	CHECK(project.info.name == "My KTST");
	CHECK(project.info.author == "Me");
}

TEST_CASE("Gateway clone: the folder holds the submission's files and the project", "[project][gateway][clone]")
{
	Scratch scratch;
	const GatewaySubmission submission = TestSubmission();
	GatewayCloneSpec spec;
	spec.location = scratch.path;
	REQUIRE(ValidateGatewayClone(spec, submission.icao, submission.airportName).empty());
	CHECK(GetGatewayCloneFolder(spec, submission.icao, submission.airportName) == scratch.path / "KTST - Test Field");

	GatewayCloneReport report;
	std::string error;
	REQUIRE(CreateGatewayCloneProject(submission, spec, report, &error));
	const fs::path folder = scratch.path / "KTST - Test Field";
	CHECK(report.folder == folder);
	CHECK(report.projectFile == folder / "KTST - Test Field.edX");
	CHECK(report.files == submission.files.size());
	CHECK(report.items == 5);

	// The pack as the Gateway sent it, laid out like a new project's
	for (const XPlane::ZipEntry &entry : submission.files)
	{
		const fs::path file = folder / VFS::FromUtf8(entry.name);
		REQUIRE(fs::is_regular_file(file));
		CHECK(ReadText(file) == std::string(entry.data.begin(), entry.data.end()));
	}
	for (const std::string &name : GetPackFolders())
		CHECK(fs::is_directory(folder / VFS::FromUtf8(name)));
	CHECK(fs::is_regular_file(folder / ".gitattributes"));

	// The project reads back as it was made
	SceneryProject loaded;
	std::vector<std::string> warnings;
	REQUIRE(LoadProjectFile(report.projectFile, loaded, nullptr, &warnings, &error));
	SceneryProject made;
	GatewayCloneReport madeReport;
	REQUIRE(MakeGatewayCloneProject(submission, spec, made, madeReport, &error));
	REQUIRE(loaded.airports.size() == 1);
	CHECK(loaded.airports[0].sceneryId == 98765);
	CHECK(loaded.airports[0].airport.id == "KTST");
	const SceneryItem *airport = AirportItem(loaded);
	REQUIRE(airport);
	CHECK(airport->children.size() == 5);
	DsfConversionOptions options;
	options.includeObjects = true;
	CHECK(GetDsfFingerprint(BuildProjectDsf(loaded, options).dsf) == GetDsfFingerprint(BuildProjectDsf(made, options).dsf));

	// Cloning there again is refused: the folder has files in it
	const std::vector<FieldProblem> problems = ValidateGatewayClone(spec, submission.icao, submission.airportName);
	REQUIRE(problems.size() == 1);
	CHECK(problems[0].field == "projectName");
	CHECK_THAT(problems[0].message, ContainsSubstring("already exists"));
	CHECK_FALSE(CreateGatewayCloneProject(submission, spec, report, &error));
	CHECK_THAT(error, ContainsSubstring("already exists"));

	// Under another name it works
	spec.projectName = "KTST second copy";
	REQUIRE(CreateGatewayCloneProject(submission, spec, report, &error));
	CHECK(fs::is_regular_file(scratch.path / "KTST second copy" / "KTST second copy.edX"));
}

TEST_CASE("Gateway clone: names, folders and hostile archives are checked before anything is written", "[project][gateway][clone]")
{
	Scratch scratch;
	GatewaySubmission submission = TestSubmission();
	GatewayCloneSpec spec;
	std::string error;
	GatewayCloneReport report;

	// No location, a missing one, a name that isn't a folder name
	std::vector<FieldProblem> problems = ValidateGatewayClone(spec, submission.icao, submission.airportName);
	REQUIRE(problems.size() == 1);
	CHECK(problems[0].field == "location");
	spec.location = scratch.path / "missing";
	problems = ValidateGatewayClone(spec, submission.icao, submission.airportName);
	REQUIRE(problems.size() == 1);
	CHECK_THAT(problems[0].message, ContainsSubstring("doesn't exist"));
	spec.location = scratch.path;
	spec.projectName = "bad/name";
	problems = ValidateGatewayClone(spec, submission.icao, submission.airportName);
	REQUIRE(problems.size() == 1);
	CHECK(problems[0].field == "projectName");
	CHECK_FALSE(CreateGatewayCloneProject(submission, spec, report, &error));
	spec.projectName.clear();

	// An empty folder of that name is fine
	fs::create_directories(scratch.path / "KTST - Test Field");
	CHECK(ValidateGatewayClone(spec, submission.icao, submission.airportName).empty());
	fs::remove(scratch.path / "KTST - Test Field");

	// A file that would land outside the folder: refused, and the folder isn't created
	for (const char *name : { "../escaped.txt", "Earth nav data/../../escaped.txt" })
	{
		GatewaySubmission hostile = submission;
		hostile.files.push_back(Entry(name, std::string_view("x")));
		CHECK_FALSE(CreateGatewayCloneProject(hostile, spec, report, &error));
		CHECK_THAT(error, ContainsSubstring("outside its folder"));
		CHECK_FALSE(fs::exists(scratch.path / "KTST - Test Field"));
		CHECK_FALSE(fs::exists(scratch.path / "escaped.txt"));
	}

	// No apt.dat, or one without airports: nothing to clone
	GatewaySubmission empty = submission;
	std::erase_if(empty.files, [](const XPlane::ZipEntry &entry) { return entry.name == "Earth nav data/apt.dat"; });
	CHECK_FALSE(CreateGatewayCloneProject(empty, spec, report, &error));
	CHECK_THAT(error, ContainsSubstring("apt.dat"));
	empty.files.push_back(Entry("Earth nav data/apt.dat", std::string_view("I\n1200 Generated by hand\n\n99\n")));
	CHECK_FALSE(CreateGatewayCloneProject(empty, spec, report, &error));
	CHECK_FALSE(fs::exists(scratch.path / "KTST - Test Field"));
}

TEST_CASE("Gateway clone: folder entries are skipped, and of names differing only in case the first is copied", "[project][gateway][clone]")
{
	Scratch scratch;
	GatewaySubmission submission = TestSubmission();
	submission.files.insert(submission.files.begin(), Entry("objects/", std::string_view()));
	submission.files.push_back(Entry("Earth nav data\\", std::string_view()));
	submission.files.push_back(Entry("readme.TXT", std::string_view("A second readme\n")));	// Same file as README.txt on Windows / macOS
	submission.files.push_back(Entry("objects/hangar.obj", std::string_view("A\n800\nOBJ\n")));
	GatewayCloneSpec spec;
	spec.location = scratch.path;
	GatewayCloneReport report;
	std::string error;
	REQUIRE(CreateGatewayCloneProject(submission, spec, report, &error));
	CHECK(report.files == submission.files.size() - 3);	// Not the two folder entries, not the second readme
	CHECK(std::ranges::any_of(report.warnings, [](const std::string &warning) { return warning.find("readme.TXT has the name of another file") != std::string::npos; }));

	const fs::path folder = report.folder;
	CHECK(ReadText(folder / "README.txt") == "Gateway scenery, see COPYING\n");	// The first one
	CHECK(fs::is_regular_file(folder / "objects" / "hangar.obj"));
	CHECK(fs::is_directory(folder / "Earth nav data"));
	size_t readmes = 0;
	for (const fs::directory_entry &entry : fs::directory_iterator(folder))
		readmes += VFS::ToUtf8(entry.path().filename()) == "README.txt" || VFS::ToUtf8(entry.path().filename()) == "readme.TXT";
	CHECK(readmes == 1);	// Even on a disk that tells them apart
}

TEST_CASE("Gateway clone: a submission file named like the project file isn't copied over it", "[project][gateway][clone]")
{
	Scratch scratch;
	GatewaySubmission submission = TestSubmission();
	submission.files.push_back(Entry("ktst - test field.EDX", std::string_view("Not a project\n")));
	submission.files.push_back(Entry(".gitignore", std::string_view("# The submission's own\n")));
	GatewayCloneSpec spec;
	spec.location = scratch.path;
	GatewayCloneReport report;
	std::string error;
	REQUIRE(CreateGatewayCloneProject(submission, spec, report, &error));
	CHECK(report.files == submission.files.size() - 1);
	CHECK(std::ranges::any_of(report.warnings, [](const std::string &warning) { return warning.find("has the name of the clone's project file") != std::string::npos; }));

	// The project file is the clone's, and the submission's own git file is kept
	SceneryProject loaded;
	REQUIRE(LoadProjectFile(report.projectFile, loaded, nullptr, nullptr, &error));
	CHECK(loaded.airports.size() == 1);
	CHECK(ReadText(report.folder / ".gitignore") == "# The submission's own\n");
}

TEST_CASE("Gateway clone: a clone that fails partway leaves nothing behind", "[project][gateway][clone]")
{
	Scratch scratch;
	const GatewaySubmission submission = TestSubmission();
	GatewaySubmission broken = submission;
	broken.files.push_back(Entry("README.txt/inner.txt", std::string_view("x")));	// README.txt is a file by then: this write fails
	GatewayCloneSpec spec;
	spec.location = scratch.path;
	const fs::path folder = scratch.path / "KTST - Test Field";
	GatewayCloneReport report;
	std::string error;

	SECTION("A folder the clone made is removed")
	{
		CHECK_FALSE(CreateGatewayCloneProject(broken, spec, report, &error));
		CHECK_FALSE(error.empty());
		CHECK(report.files > 0);	// It got partway
		CHECK_FALSE(fs::exists(folder));
	}

	SECTION("An empty folder that was there stays, empty")
	{
		fs::create_directories(folder);
		CHECK_FALSE(CreateGatewayCloneProject(broken, spec, report, &error));
		CHECK(fs::is_directory(folder));
		CHECK(fs::is_empty(folder));
	}

	// So the same name can be tried again
	CHECK(ValidateGatewayClone(spec, submission.icao, submission.airportName).empty());
	REQUIRE(CreateGatewayCloneProject(submission, spec, report, &error));
	CHECK(fs::is_regular_file(folder / "KTST - Test Field.edX"));
	CHECK(std::distance(fs::directory_iterator(scratch.path), fs::directory_iterator()) == 1);	// Nothing else left beside it
}

TEST_CASE("Gateway clone: the folder made is the one that was checked", "[project][gateway][clone]")
{
	// The hub checks the folder with the airport's code and name from the Gateway's airport record, then hands the name
	// it showed over (GetGatewayCloneName); the downloaded submission may name its airport differently
	Scratch scratch;
	GatewaySubmission submission = TestSubmission();
	submission.airportName = "Test Field Municipal";
	std::string error;
	GatewayCloneReport report;

	// Names with line ends around them (pasted, or an airport name the Gateway sends that way) lose them too
	CHECK(GetGatewayCloneName({}, "KTST\r\n", "Test Field\r\n") == "KTST - Test Field");
	GatewayCloneSpec pasted;
	pasted.projectName = "\nPasted Name\r\n";
	CHECK(GetGatewayCloneName(pasted, "KTST", "Test Field") == "Pasted Name");
	pasted.location = scratch.path;
	CHECK(ValidateGatewayClone(pasted, "KTST", "Test Field").empty());

	for (const char *typed : { "", "  Spaced Name\t" })
	{
		INFO(typed);
		GatewayCloneSpec spec;
		spec.location = scratch.path;
		spec.projectName = typed;
		REQUIRE(ValidateGatewayClone(spec, "KTST", "Test Field").empty());
		const fs::path shown = GetGatewayCloneFolder(spec, "KTST", "Test Field");
		spec.projectName = GetGatewayCloneName(spec, "KTST", "Test Field");
		REQUIRE(CreateGatewayCloneProject(submission, spec, report, &error));
		CHECK(report.folder == shown);
		CHECK(report.projectFile == shown / VFS::FromUtf8(VFS::ToUtf8(shown.filename()) + ".edX"));
		CHECK(fs::is_regular_file(report.projectFile));
	}
	CHECK(fs::is_directory(scratch.path / "KTST - Test Field"));
	CHECK(fs::is_directory(scratch.path / "Spaced Name"));
	CHECK(std::distance(fs::directory_iterator(scratch.path), fs::directory_iterator()) == 2);
}

TEST_CASE("Gateway clone: a pack's airport filters come back on export, in every tile", "[project][gateway][clone][export]")
{
	// Placements filtered to the airport in two tiles: the clone drops the filters (the placements become the airport
	// item's children) and the export writes them again from that, per tile
	Scratch scratch;
	GatewaySubmission submission = TestSubmission();
	std::erase_if(submission.files, [](const XPlane::ZipEntry &entry) { return entry.name.ends_with(".dsf"); });
	for (const double longitude : { LON, -123.2 })
	{
		XPlane::DsfFile tile = PackDsf(PackItems(LAT, longitude));
		tile.properties.emplace_back("sim/filter/aptid", "KTST");
		for (XPlane::DsfObjectPlacement &object : tile.objects)
			object.filter = 0;
		for (XPlane::DsfPolygonPlacement &polygon : tile.polygons)
			polygon.filter = 0;
		submission.files.push_back(Entry("Earth nav data/+40-130/" + XPlane::GetDsfTileName(LAT, longitude) + ".dsf", WriteTile(tile)));
	}
	GatewayCloneSpec spec;
	spec.location = scratch.path;
	GatewayCloneReport report;
	std::string error;
	REQUIRE(CreateGatewayCloneProject(submission, spec, report, &error));
	CHECK(report.tiles == 2);
	SceneryProject project;
	REQUIRE(LoadProjectFile(report.projectFile, project, nullptr, nullptr, &error));

	const fs::path pack = scratch.path / "Exported";
	ExportOptions options;
	ExportReport exported;
	REQUIRE(ExportSceneryPack(project, report.folder, pack, options, exported, &error));
	CHECK(exported.tiles == 2);
	for (const double longitude : { LON, -123.2 })
	{
		const std::string name = XPlane::GetDsfTileName(LAT, longitude);
		INFO(name);
		const std::string bytes = ReadText(pack / "Earth nav data" / "+40-130" / (name + ".dsf"));
		XPlane::DsfFile tile;
		REQUIRE(XPlane::ReadDsf(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), tile, &error));
		const auto filter = std::ranges::find(tile.properties, std::pair<std::string, std::string>("sim/filter/aptid", "KTST"));
		REQUIRE(filter != tile.properties.end());
		CHECK(std::ranges::count(tile.properties, std::string("sim/filter/aptid"), &std::pair<std::string, std::string>::first) == 1);
		CHECK(tile.objects.size() == 1);
		CHECK(tile.polygons.size() == 3);
		CHECK(std::ranges::all_of(tile.objects, [](const XPlane::DsfObjectPlacement &object) { return object.filter == 0; }));
		CHECK(std::ranges::all_of(tile.polygons, [](const XPlane::DsfPolygonPlacement &polygon) { return polygon.filter == 0; }));
	}
}

TEST_CASE("Gateway clone: exporting the project writes its airport once", "[project][gateway][clone][export]")
{
	// The clone keeps the submission's own Earth nav data/apt.dat; an export writes apt.dat from the project's airports
	// instead of adding to it, whether into the project's folder or somewhere else
	Scratch scratch;
	const GatewaySubmission submission = TestSubmission();
	GatewayCloneSpec spec;
	spec.location = scratch.path;
	GatewayCloneReport report;
	std::string error;
	REQUIRE(CreateGatewayCloneProject(submission, spec, report, &error));
	SceneryProject project;
	REQUIRE(LoadProjectFile(report.projectFile, project, nullptr, nullptr, &error));

	for (const fs::path &pack : { report.folder, scratch.path / "Exported" })
	{
		INFO(VFS::ToUtf8(pack));
		ExportOptions options;
		options.removeStaleTiles = true;
		ExportReport exported;
		REQUIRE(ExportSceneryPack(project, report.folder, pack, options, exported, &error));
		XPlane::AptFile apt;
		REQUIRE(XPlane::ParseAptDat(ReadText(pack / "Earth nav data" / "apt.dat"), apt, &error));
		REQUIRE(apt.airports.size() == 1);
		CHECK(apt.airports[0].id == "KTST");

		// And one tile, holding the placements once
		XPlane::DsfFile tile;
		const std::string bytes = ReadText(pack / "Earth nav data" / "+40-130" / "+45-123.dsf");
		REQUIRE(XPlane::ReadDsf(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), tile, &error));
		CHECK(tile.objects.size() == 1);
		CHECK(std::distance(fs::recursive_directory_iterator(pack / "Earth nav data"), fs::recursive_directory_iterator()) == 3);	// apt.dat, +40-130, its .dsf
	}
}

TEST_CASE("Gateway clone: the submission's airport, every tile, and warnings for what can't be read", "[project][gateway][clone]")
{
	GatewaySubmission submission = TestSubmission();
	GatewayCloneSpec spec;
	SceneryProject project;
	GatewayCloneReport report;
	std::string error;

	// apt.dat with a neighbour first: the submission's own airport is taken
	submission.files[0] = Entry("earth nav data/APT.DAT", AptDat({ TestAirport("KNBR", "Neighbour"), TestAirport() }));		// Any spelling
	REQUIRE(MakeGatewayCloneProject(submission, spec, project, report, &error));
	CHECK(report.airportId == "KTST");
	REQUIRE(project.airports.size() == 1);
	CHECK(project.airports[0].airport.name == "Test Field");
	CHECK(std::ranges::any_of(report.warnings, [](const std::string &warning) { return warning.find("2 airports") != std::string::npos; }));

	// A second tile (the airport's parking across the tile edge) and a broken one
	submission.files.push_back(Entry("Earth nav data/+40-130/+45-124.dsf", WriteTile(PackDsf(PackItems(LAT, -123.2)))));
	submission.files.push_back(Entry("Earth nav data/+40-130/+46-123.dsf", std::string_view("not a DSF")));
	REQUIRE(MakeGatewayCloneProject(submission, spec, project, report, &error));
	CHECK(report.tiles == 2);
	CHECK(report.items == 10);
	CHECK(std::ranges::any_of(report.warnings, [](const std::string &warning) { return warning.find("+46-123.dsf can't be read") != std::string::npos; }));
	const SceneryItem *airport = AirportItem(project);
	REQUIRE(airport);
	CHECK(airport->children.size() == 10);

	// Several airports, none of them the submission's: refused rather than guessed
	GatewaySubmission others = submission;
	others.files[0] = Entry("Earth nav data/apt.dat", AptDat({ TestAirport("KNBR", "Neighbour"), TestAirport("KOTH", "Other") }));
	CHECK_FALSE(MakeGatewayCloneProject(others, spec, project, report, &error));
	CHECK_THAT(error, ContainsSubstring("none is KTST"));
	CHECK_THAT(error, ContainsSubstring("KNBR, KOTH"));

	// Its only airport under another code (renamed on the Gateway since): taken, with a warning
	others.files[0] = Entry("Earth nav data/apt.dat", AptDat({ TestAirport("KNEW", "Test Field") }));
	REQUIRE(MakeGatewayCloneProject(others, spec, project, report, &error));
	CHECK(report.airportId == "KNEW");
	CHECK(std::ranges::any_of(report.warnings, [](const std::string &warning) { return warning.find("no airport KTST: KNEW was taken") != std::string::npos; }));

	// Without the submission's identifier, the first airport (and its name for the project)
	submission.icao.clear();
	submission.airportName.clear();
	submission.files[0] = Entry("Earth nav data/apt.dat", AptDat({ TestAirport("KNBR", "Neighbour") }));
	REQUIRE(MakeGatewayCloneProject(submission, spec, project, report, &error));
	CHECK(report.airportId == "KNBR");
	CHECK(project.info.name == "KNBR - Neighbour");

	// A submission's id isn't recorded when there is none
	submission.sceneryId = 0;
	REQUIRE(MakeGatewayCloneProject(submission, spec, project, report, &error));
	CHECK(project.airports[0].sceneryId == -1);
}

TEST_CASE("Gateway clone: tiles merge into one DSF", "[project][gateway][clone][dsf]")
{
	XPlane::DsfFile a = PackDsf(PackItems(LAT, LON));
	XPlane::DsfFile b = PackDsf(PackItems(LAT, -123.2));
	b.objectDefinitions.insert(b.objectDefinitions.begin(), "objects/own.obj");		// Shifts b's object indices
	for (XPlane::DsfObjectPlacement &object : b.objects)
		++object.definition;
	b.polygons.front().filter = 0;

	// Roads in both, with junction ids that would collide
	XPlane::DsfRoadSegment road;
	road.definition = 0;
	road.startJunction = 1;
	road.endJunction = 2;
	road.points = { { LON, LAT, false }, { LON + 0.001, LAT, false } };
	a.networkDefinitions = { "lib/g10/roads.net" };
	a.roads = { road };
	b.networkDefinitions = { "lib/g10/roads.net" };
	b.roads = { road };

	XPlane::DsfFile merged;
	XPlane::AppendDsf(merged, a);
	XPlane::AppendDsf(merged, b);
	CHECK(merged.objects.size() == a.objects.size() + b.objects.size());
	CHECK(merged.polygons.size() == a.polygons.size() + b.polygons.size());
	CHECK(merged.objectDefinitions == std::vector<std::string>{ "lib/airport/Ramp_Equipment/hangar.obj", "objects/own.obj" });	// One entry per path
	CHECK(merged.polygonDefinitions == a.polygonDefinitions);
	CHECK(merged.objects.back().definition == 0);
	CHECK(merged.objectDefinitions[merged.objects.back().definition] == "lib/airport/Ramp_Equipment/hangar.obj");
	CHECK(std::ranges::all_of(merged.polygons, [](const XPlane::DsfPolygonPlacement &polygon) { return polygon.filter == -1; }));
	REQUIRE(merged.roads.size() == 2);
	CHECK(merged.roads[1].startJunction == 3);
	CHECK(merged.roads[1].endJunction == 4);
	CHECK(merged.GetExclusions().size() == 2);						// Different zones
	CHECK(XPlane::AppendDsf(merged, a) == 0);
	CHECK(merged.GetExclusions().size() == 2);						// The same zone isn't listed twice
	CHECK(merged.west == -124.0);
	CHECK(merged.east == -122.0);

	// A damaged tile's placements naming definitions it doesn't list are left out, and counted
	XPlane::DsfFile damaged = a;
	damaged.objects.front().definition = 99;
	damaged.polygons.front().definition = 99;
	damaged.roads.front().definition = 99;
	const size_t objects = merged.objects.size(), polygons = merged.polygons.size(), roads = merged.roads.size();
	CHECK(XPlane::AppendDsf(merged, damaged) == 3);
	CHECK(merged.objects.size() == objects + damaged.objects.size() - 1);
	CHECK(merged.polygons.size() == polygons + damaged.polygons.size() - 1);
	CHECK(merged.roads.size() == roads + damaged.roads.size() - 1);
}

TEST_CASE("Gateway clone: road networks of two tiles stay apart, and joined inside each", "[project][gateway][clone][dsf]")
{
	// Two tiles written and read back the way a submission's are, each numbering its junctions from 1: a chain of two
	// segments meeting at junction 2, and the tiles listing their networks in a different order
	const auto segment = [](const uint32_t definition, const uint32_t start, const uint32_t end, const double longitude, const double latitude)
	{
		XPlane::DsfRoadSegment road;
		road.definition = definition;
		road.subtype = 3;
		road.startJunction = start;
		road.endJunction = end;
		road.points = { { longitude, latitude, false }, { longitude + 0.001, latitude, false } };
		return road;
	};
	const auto readBack = [](const XPlane::DsfFile &tile)
	{
		const std::vector<uint8_t> bytes = WriteTile(tile);
		XPlane::DsfFile read;
		std::string error;
		REQUIRE(XPlane::ReadDsf(bytes.data(), bytes.size(), read, &error));
		return read;
	};
	XPlane::DsfFile a = PackDsf(PackItems(LAT, LON));
	a.networkDefinitions = { "lib/g10/roads.net" };
	a.roads = { segment(0, 1, 2, LON, LAT), segment(0, 2, 3, LON + 0.001, LAT) };
	XPlane::DsfFile b = PackDsf(PackItems(LAT, -123.2));
	b.networkDefinitions = { "lib/g10/rails.net", "lib/g10/roads.net" };
	b.roads = { segment(1, 1, 2, -123.2, LAT), segment(1, 2, 3, -123.199, LAT) };
	a = readBack(a);
	b = readBack(b);
	REQUIRE(a.roads.size() == 2);
	REQUIRE(b.roads.size() == 2);

	XPlane::DsfFile merged;
	XPlane::AppendDsf(merged, a);
	XPlane::AppendDsf(merged, b);
	REQUIRE(merged.roads.size() == 4);
	CHECK(merged.networkDefinitions == std::vector<std::string>{ "lib/g10/roads.net", "lib/g10/rails.net" });
	for (const XPlane::DsfRoadSegment &road : merged.roads)
	{
		CHECK(merged.networkDefinitions[road.definition] == "lib/g10/roads.net");
		CHECK(road.startJunction != 0);
		CHECK(road.endJunction != 0);
	}

	// Each tile's chain still meets at its middle junction; no junction of one tile is one of the other's
	CHECK(merged.roads[0].endJunction == merged.roads[1].startJunction);
	CHECK(merged.roads[2].endJunction == merged.roads[3].startJunction);
	std::vector<uint32_t> first = { merged.roads[0].startJunction, merged.roads[0].endJunction, merged.roads[1].endJunction };
	for (const XPlane::DsfRoadSegment &road : { merged.roads[2], merged.roads[3] })
	{
		CHECK(std::ranges::find(first, road.startJunction) == first.end());
		CHECK(std::ranges::find(first, road.endJunction) == first.end());
	}

	// And the merged network writes and reads back as four segments
	XPlane::DsfFile written = readBack(merged);
	CHECK(written.roads.size() == 4);
}
