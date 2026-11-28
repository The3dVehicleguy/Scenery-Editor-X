/**
 * -------------------------------------------------------
 * Scenery Editor X - Update check tests
 * -------------------------------------------------------
 * Copyright (c) 2026 Thomas Ray
 * Copyright (c) 2026 Coalition of Freeware Developers
 * -------------------------------------------------------
 * UpdateTests.cpp
 * -------------------------------------------------------
 * The update check's standard-library side (SceneryEditorX/update/update_release.*, utils/sha256.*): release
 * versions, GitHub's release JSON, the installer and checksum files a release carries, trusted download URLs, when
 * a check is due and what it offers, and the SHA-256 that checks a downloaded installer.
 * -------------------------------------------------------
 */
#include <catch2/catch_all.hpp>
#include <SceneryEditorX/update/update_release.h>
#include <SceneryEditorX/utils/sha256.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace SceneryEditorX;
using namespace SceneryEditorX::Update;
namespace fs = std::filesystem;

namespace
{
	constexpr const char *REPOSITORY = "The3dVehicleguy/Scenery-Editor-X";

	/* @brief A release as GitHub's API returns it (trimmed: the fields the editor reads, plus some it ignores). */
	std::string ReleaseJson(const std::string &tag = "v0.2.130.631", const std::string &extraAssets = "")
	{
		return R"({
			"url": "https://api.github.com/repos/The3dVehicleguy/Scenery-Editor-X/releases/1",
			"html_url": "https://github.com/The3dVehicleguy/Scenery-Editor-X/releases/tag/)" + tag + R"(",
			"id": 1,
			"tag_name": ")" + tag + R"(",
			"target_commitish": "release",
			"name": "Scenery Editor X 0.2.130.631",
			"draft": false,
			"prerelease": false,
			"created_at": "2026-11-27T10:00:00Z",
			"published_at": "2026-11-27T10:30:00Z",
			"body": "### Added\r\n\r\n- **Update check** at startup.\r\n",
			"assets": [
				{
					"name": "SceneryEditorX-0.2.130.631-Alpha-Setup.exe",
					"content_type": "application/x-msdownload",
					"size": 42000000,
					"digest": "sha256:BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
					"browser_download_url": "https://github.com/The3dVehicleguy/Scenery-Editor-X/releases/download/v0.2.130.631/SceneryEditorX-0.2.130.631-Alpha-Setup.exe"
				},
				{
					"name": "SHA256SUMS.txt",
					"content_type": "text/plain",
					"size": 160,
					"digest": null,
					"browser_download_url": "https://github.com/The3dVehicleguy/Scenery-Editor-X/releases/download/v0.2.130.631/SHA256SUMS.txt"
				})" + extraAssets + R"(
			]
		})";
	}

	fs::path TempFile(const std::string &name)
	{
		std::random_device random;
		const fs::path folder = fs::temp_directory_path() / ("sedx-update-tests-" + std::to_string(random()));
		fs::create_directories(folder);
		return folder / name;
	}
}

// -------------------------------------------------------

TEST_CASE("Versions parse from version strings and release tags", "[update][version]")
{
	const std::optional<Version> plain = ParseVersion("0.2.127.628");
	REQUIRE(plain);
	CHECK(plain->major == 0);
	CHECK(plain->minor == 2);
	CHECK(plain->patch == 127);
	CHECK(plain->build == 628);
	CHECK(plain->ToString() == "0.2.127.628");

	CHECK(ParseVersion("v0.2.127.628") == plain);
	CHECK(ParseVersion("V0.2.127.628") == plain);
	CHECK(ParseVersion("1.4.0")->ToString() == "1.4.0.0");		// Three parts: build 0
	CHECK(ParseVersion("0.2.007.1")->patch == 7);

	for (const char *bad : { "", "v", "1", "1.2", "1.2.3.4.5", "1..2.3", ".1.2.3", "1.2.3.", "1.2.3-beta", "1.2.3.4 ",
		" 1.2.3.4", "+1.2.3.4", "1.2.x.4", "vv1.2.3.4", "1.2.3.1234567890", "release-1.2.3" })
	{
		INFO(bad);
		CHECK_FALSE(ParseVersion(bad));
	}
}

TEST_CASE("Versions compare part by part, numerically", "[update][version]")
{
	const auto v = [](const char *text) { return *ParseVersion(text); };
	CHECK(v("0.2.128.629") > v("0.2.127.628"));
	CHECK(v("0.2.100.601") > v("0.2.99.600"));			// Numbers, not text
	CHECK(v("0.3.0.0") > v("0.2.999.999"));
	CHECK(v("1.0.0.0") > v("0.99.99.99"));
	CHECK(v("0.2.127.629") > v("0.2.127.628"));			// The build number alone
	CHECK(v("0.2.127.628") == v("v0.2.127.628"));
	CHECK_FALSE(v("0.2.127.628") < v("0.2.127.628"));
}

TEST_CASE("A GitHub release reads into Release", "[update][release]")
{
	Release release;
	std::string error;
	REQUIRE(ParseRelease(ReleaseJson(), release, error));
	CHECK(error.empty());
	CHECK(release.tag == "v0.2.130.631");
	CHECK(release.version.ToString() == "0.2.130.631");
	CHECK(release.title == "Scenery Editor X 0.2.130.631");
	CHECK(release.pageUrl == "https://github.com/The3dVehicleguy/Scenery-Editor-X/releases/tag/v0.2.130.631");
	CHECK(release.PublishedDate() == "2026-11-27");
	CHECK_FALSE(release.draft);
	CHECK_FALSE(release.prerelease);
	CHECK(release.notes.find("Update check") != std::string::npos);
	REQUIRE(release.assets.size() == 2);
	CHECK(release.assets[0].size == 42000000);
	CHECK(release.assets[0].contentType == "application/x-msdownload");
	CHECK(release.assets[0].sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");	// Lowercased
	CHECK(release.assets[1].sha256.empty());	// "digest": null
}

TEST_CASE("Release JSON that isn't a usable release is refused", "[update][release]")
{
	Release release;
	std::string error;

	CHECK_FALSE(ParseRelease("not json", release, error));
	CHECK(error.starts_with("not JSON"));

	CHECK_FALSE(ParseRelease("[1, 2]", release, error));
	CHECK(error == "not a release object");

	CHECK_FALSE(ParseRelease(R"({"name": "x"})", release, error));
	CHECK(error == "the release has no tag");

	CHECK_FALSE(ParseRelease(ReleaseJson("nightly"), release, error));
	CHECK(error.find("\"nightly\" is not a version") != std::string::npos);

	// null and missing fields read as empty: GitHub sends null for an untitled release without notes
	REQUIRE(ParseRelease(R"({"tag_name": "v1.0.0.1", "name": null, "body": null, "assets": [{"name": null}, 3, {"name": "a.txt", "size": -1, "digest": "sha256:xyz"}]})", release, error));
	CHECK(release.title == "v1.0.0.1");
	CHECK(release.notes.empty());
	REQUIRE(release.assets.size() == 1);
	CHECK(release.assets[0].size == 0);
	CHECK(release.assets[0].sha256.empty());	// Not 64 hex digits
}

TEST_CASE("The Windows installer is the setup named for the release's version", "[update][release]")
{
	Release release;
	std::string error;
	REQUIRE(ParseRelease(ReleaseJson(), release, error));
	const ReleaseAsset *installer = FindWindowsInstaller(release);
	REQUIRE(installer);
	CHECK(installer->name == "SceneryEditorX-0.2.130.631-Alpha-Setup.exe");

	CHECK(FindAsset(release, "sha256sums.TXT") == &release.assets[1]);
	CHECK_FALSE(FindAsset(release, "SHA256SUMS"));

	// Another version's setup, a setup without a channel, and other files aren't it
	const std::string others = R"(,
		{ "name": "SceneryEditorX-0.2.129.630-Alpha-Setup.exe", "browser_download_url": "https://x/1" },
		{ "name": "SceneryEditorX-0.2.131.632--Setup.exe", "browser_download_url": "https://x/2" },
		{ "name": "SceneryEditorX-0.2.131.632-Alpha-Setup.zip", "browser_download_url": "https://x/3" })";
	REQUIRE(ParseRelease(ReleaseJson("v0.2.131.632", others), release, error));
	CHECK_FALSE(FindWindowsInstaller(release));

	REQUIRE(ParseRelease(ReleaseJson("v0.2.129.630", others), release, error));
	installer = FindWindowsInstaller(release);
	REQUIRE(installer);
	CHECK(installer->name == "SceneryEditorX-0.2.129.630-Alpha-Setup.exe");

	// Three-part versions: the tag's and the file's are compared as versions, with or without the build number
	const std::string threePart = R"(,
		{ "name": "SceneryEditorX-0.2.63-Beta-Setup.exe", "browser_download_url": "https://x/4" })";
	REQUIRE(ParseRelease(ReleaseJson("v0.2.63", threePart), release, error));
	installer = FindWindowsInstaller(release);
	REQUIRE(installer);
	CHECK(installer->name == "SceneryEditorX-0.2.63-Beta-Setup.exe");
	REQUIRE(ParseRelease(ReleaseJson("v0.2.63.0", threePart), release, error));
	REQUIRE(FindWindowsInstaller(release));
	CHECK(FindWindowsInstaller(release)->name == "SceneryEditorX-0.2.63-Beta-Setup.exe");
	const std::string fourPart = R"(,
		{ "name": "sceneryeditorx-0.2.64.0-rc1-setup.EXE", "browser_download_url": "https://x/5" })";
	REQUIRE(ParseRelease(ReleaseJson("v0.2.64", fourPart), release, error));
	REQUIRE(FindWindowsInstaller(release));
	CHECK(FindWindowsInstaller(release)->name == "sceneryeditorx-0.2.64.0-rc1-setup.EXE");

	// The channel is letters and digits: not empty, no dashes, dots or other characters
	const std::string badChannels = R"(,
		{ "name": "SceneryEditorX-0.2.65.0-Alpha-x86-Setup.exe", "browser_download_url": "https://x/6" },
		{ "name": "SceneryEditorX-0.2.65.0-Al.pha-Setup.exe", "browser_download_url": "https://x/7" },
		{ "name": "SceneryEditorX-0.2.65.0-Al pha-Setup.exe", "browser_download_url": "https://x/8" },
		{ "name": "SceneryEditorX-0.2.65.0--Setup.exe", "browser_download_url": "https://x/9" },
		{ "name": "SceneryEditorX-0.2.65.0.Setup.exe", "browser_download_url": "https://x/10" })";
	REQUIRE(ParseRelease(ReleaseJson("v0.2.65.0", badChannels), release, error));
	CHECK_FALSE(FindWindowsInstaller(release));
}

TEST_CASE("Checksum listings give a file's hash", "[update][checksum]")
{
	const std::string listing =
		"# SHA-256 of the release files\r\n"
		"\r\n"
		"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  SceneryEditorX-0.2.130.631-Alpha-Setup.exe\r\n"
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 *empty.bin\n"
		"not-a-hash  other.txt\n"
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1\tTabbed.txt";

	CHECK(FindChecksum(listing, "sceneryeditorx-0.2.130.631-alpha-setup.exe") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(FindChecksum(listing, "empty.bin") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(FindChecksum(listing, "Tabbed.txt") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(FindChecksum(listing, "other.txt").empty());
	CHECK(FindChecksum(listing, "missing.exe").empty());
	CHECK(FindChecksum("", "empty.bin").empty());
	// A .sha256 file holds the one line
	CHECK(FindChecksum("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  a.exe\n", "a.exe").size() == 64);
}

TEST_CASE("Only the repository's release files are trusted downloads", "[update][url]")
{
	const std::string base = "https://github.com/The3dVehicleguy/Scenery-Editor-X/releases/download/";
	CHECK(IsReleaseDownloadUrl(base + "v0.2.130.631/SceneryEditorX-0.2.130.631-Alpha-Setup.exe", REPOSITORY));
	CHECK(IsReleaseDownloadUrl("https://GitHub.com/the3dvehicleguy/scenery-editor-x/releases/download/v1/a.exe", REPOSITORY));

	CHECK_FALSE(IsReleaseDownloadUrl("http://github.com/The3dVehicleguy/Scenery-Editor-X/releases/download/v1/a.exe", REPOSITORY));
	CHECK_FALSE(IsReleaseDownloadUrl("https://github.com/Someone/Scenery-Editor-X/releases/download/v1/a.exe", REPOSITORY));
	CHECK_FALSE(IsReleaseDownloadUrl("https://github.com.evil.example/The3dVehicleguy/Scenery-Editor-X/releases/download/v1/a.exe", REPOSITORY));
	CHECK_FALSE(IsReleaseDownloadUrl("https://github.com/The3dVehicleguy/Scenery-Editor-X-fork/releases/download/v1/a.exe", REPOSITORY));
	CHECK_FALSE(IsReleaseDownloadUrl("https://github.com/The3dVehicleguy/Scenery-Editor-X/archive/v1.zip", REPOSITORY));
	CHECK_FALSE(IsReleaseDownloadUrl(base, REPOSITORY));					// No file
	CHECK_FALSE(IsReleaseDownloadUrl(base + "v1/../../a.exe", REPOSITORY));
	CHECK_FALSE(IsReleaseDownloadUrl(base + "v1/a b.exe", REPOSITORY));
	CHECK_FALSE(IsReleaseDownloadUrl(base + "v1/a.exe", ""));

	// Exactly <tag>/<file> after the prefix: no dot segments, escapes, extra segments, queries or fragments
	CHECK(IsReleaseDownloadUrl(base + "v0.2.130.631/SHA256SUMS.txt", REPOSITORY));
	CHECK(IsReleaseDownloadUrl(base + "v1.0+build_2/Scenery_Editor-X.exe", REPOSITORY));
	for (const char *bad : { "..", "v1/..", "v1/.", "./a.exe", "../a.exe", "v1/./a.exe", "v1/%2e%2e/a.exe", "%2e%2e/a.exe",
		"v1/a%20b.exe", "v1/sub/a.exe", "v1/", "v1", "/a.exe", "v1//a.exe", "v1/a.exe?x=1", "v1/a.exe#f", "v1/.hidden",
		"v1/a.exe/" })
	{
		INFO(bad);
		CHECK_FALSE(IsReleaseDownloadUrl(base + bad, REPOSITORY));
	}
}

TEST_CASE("Setup file names give their version", "[update][release]")
{
	CHECK(InstallerFileVersion("SceneryEditorX-0.2.130.631-Alpha-Setup.exe")->ToString() == "0.2.130.631");
	CHECK(InstallerFileVersion("sceneryeditorx-0.2.63-beta-setup.EXE")->ToString() == "0.2.63.0");
	CHECK(InstallerFileVersion("SCENERYEDITORX-1.0.0.1-RC1-SETUP.EXE")->ToString() == "1.0.0.1");
	for (const char *other : { "SceneryEditorX-garbage-Alpha-Setup.exe", "SceneryEditorX-0.2.1.2--Setup.exe",
		"SceneryEditorX-0.2.1.2-Alpha-x64-Setup.exe", "SceneryEditorX-0.2.1.2-Setup.exe", "SceneryEditorX-0.2.1.2-Alpha-Setup.zip",
		"Other-0.2.1.2-Alpha-Setup.exe", "SceneryEditorX-0.2.1.2-Alpha-Setup.exe.part", "latest_release.json", "" })
	{
		INFO(other);
		CHECK_FALSE(InstallerFileVersion(other));
	}
}

TEST_CASE("Only servers on this computer are test servers", "[update][url]")
{
	for (const char *local : { "http://127.0.0.1:8765/latest.json", "http://127.0.0.1/x", "https://127.0.0.1", "http://localhost:8000/a.json",
		"HTTP://LocalHost/a", "http://[::1]:9000/latest.json", "http://[::1]", "http://127.0.0.1?x=1", "http://localhost#frag" })
	{
		INFO(local);
		CHECK(IsLoopbackUrl(local));
	}
	for (const char *remote : { "http://example.com/latest.json", "https://api.github.com/repos/x/y/releases/latest",
		"http://127.0.0.1@example.com/latest.json", "http://user@localhost/a", "http://localhost.example.com/a",
		"http://127.0.0.10/a", "http://127.0.0.2/a", "http://[::1]x/a", "http://[::1", "ftp://127.0.0.1/a", "file:///C:/a.json",
		"127.0.0.1:8765/latest.json", "", "http://", "http://127.0.0.1\\@example.com/" })
	{
		INFO(remote);
		CHECK_FALSE(IsLoopbackUrl(remote));
	}
}

TEST_CASE("An automatic check is due once a day", "[update][schedule]")
{
	constexpr std::int64_t DAY = 24 * 60 * 60;
	const std::int64_t now = 1'800'000'000;
	CHECK(IsCheckDue(0, now, DAY));					// Never checked
	CHECK(IsCheckDue(now - DAY, now, DAY));
	CHECK(IsCheckDue(now - 2 * DAY, now, DAY));
	CHECK_FALSE(IsCheckDue(now - DAY + 1, now, DAY));
	CHECK_FALSE(IsCheckDue(now, now, DAY));
	CHECK(IsCheckDue(now + 60, now, DAY));			// The clock went back
}

TEST_CASE("Only a newer release is offered, unless it was skipped", "[update][offer]")
{
	Release latest;
	std::string error;
	REQUIRE(ParseRelease(ReleaseJson(), latest, error));
	const Version running = *ParseVersion("0.2.127.628");

	CHECK(Evaluate(running, latest, "") == Offer::Newer);
	CHECK(Evaluate(running, latest, "0.2.129.630") == Offer::Newer);		// Another version was skipped
	CHECK(Evaluate(running, latest, "0.2.130.631") == Offer::Skipped);
	CHECK(Evaluate(running, latest, "v0.2.130.631") == Offer::Skipped);
	CHECK(Evaluate(running, latest, "garbage") == Offer::Newer);
	CHECK(Evaluate(latest.version, latest, "") == Offer::None);			// The same version
	CHECK(Evaluate(*ParseVersion("0.2.140.641"), latest, "") == Offer::None);	// A newer development build

	latest.draft = true;
	CHECK(Evaluate(running, latest, "") == Offer::None);
	latest.draft = false;
	latest.prerelease = true;
	CHECK(Evaluate(running, latest, "") == Offer::None);			// Pre-releases are never offered
}

TEST_CASE("Requests are only redirected to GitHub", "[update][url]")
{
	for (const char *ok : { "https://github.com/x/y/releases/download/v1/a.exe", "https://api.github.com/repos/x/y/releases/latest",
		"https://objects.githubusercontent.com/github-production-release-asset/1/2?x=1", "https://release-assets.githubusercontent.com/a",
		"HTTPS://Release-Assets.GitHubUserContent.com:443/a", "https://raw.githubusercontent.com/x" })
	{
		INFO(ok);
		CHECK(IsAllowedRedirect(ok, false));
		CHECK(IsAllowedRedirect(ok, true));
	}
	for (const char *bad : { "http://github.com/a", "http://objects.githubusercontent.com/a", "https://githubusercontent.com/a",
		"https://evilgithubusercontent.com/a", "https://githubusercontent.com.evil.example/a", "https://github.com.evil.example/a",
		"https://gist.github.com/a", "https://github.com@evil.example/a", "https://example.com/a", "ftp://github.com/a", "", "https://",
		"https://objects.githubusercontent.com\\@evil.example/a" })
	{
		INFO(bad);
		CHECK_FALSE(IsAllowedRedirect(bad, false));
	}
	// A test server may redirect within itself (curl allows https redirects only, so http here never follows)
	CHECK(IsAllowedRedirect("http://127.0.0.1:8765/files/a.exe", true));
	CHECK_FALSE(IsAllowedRedirect("http://127.0.0.1:8765/files/a.exe", false));
	CHECK_FALSE(IsAllowedRedirect("https://example.com/a", true));
}

TEST_CASE("Release notes lose their Markdown markup", "[update][notes]")
{
	const std::string markdown =
		"## 0.2.130.631\r\n"
		"\r\n"
		"### Added\r\n"
		"\r\n"
		"- **Update check** (`Help > Check for Updates`): press <kbd>Ctrl</kbd>+<kbd>U</kbd>.\r\n"
		"  * nested [item](https://example.com/a) with ![an image](x.png)\r\n"
		"\r\n"
		"\r\n"
		"\r\n"
		"Plain __text__ stays, 2 * 3 too.<br>Next.\r\n"
		"#hashtag isn't a heading\r\n"
		"[broken link(no target\r\n";
	const std::string plain = PlainTextNotes(markdown);
	CHECK(plain ==
		"0.2.130.631\n"
		"\n"
		"Added\n"
		"\n"
		"- Update check (Help > Check for Updates): press Ctrl+U.\n"
		"  - nested item with an image\n"
		"\n"
		"Plain text stays, 2 * 3 too. Next.\n"
		"#hashtag isn't a heading\n"
		"[broken link(no target");
	CHECK(PlainTextNotes("").empty());
	CHECK(PlainTextNotes("\n\n\n").empty());
}

TEST_CASE("Check times read as UTC dates", "[update][time]")
{
	CHECK(FormatUtcTime(0) == "1970-01-01 00:00 UTC");
	CHECK(FormatUtcTime(-1) == "1969-12-31 23:59 UTC");
	CHECK(FormatUtcTime(951782400) == "2000-02-29 00:00 UTC");			// A leap year divisible by 400
	CHECK(FormatUtcTime(1709164800) == "2024-02-29 00:00 UTC");
	CHECK(FormatUtcTime(1795834831) == "2026-11-28 03:00 UTC");
	CHECK(FormatUtcTime(4102444800) == "2100-01-01 00:00 UTC");			// 2100 isn't a leap year
	CHECK(FormatUtcTime(4107542400) == "2100-03-01 00:00 UTC");
	CHECK(FormatUtcTime(253402300799) == "9999-12-31 23:59 UTC");

	// Every day from 1969 to 2101 is the day after the one before
	std::int64_t previousDay = -1;
	std::string previous;
	for (std::int64_t t = -86400 * 366; t < 4133980800; t += 86400)
	{
		const std::string text = FormatUtcTime(t);
		REQUIRE(text.size() == 20);
		const int year = std::stoi(text.substr(0, 4)), month = std::stoi(text.substr(5, 2)), day = std::stoi(text.substr(8, 2));
		REQUIRE(month >= 1);
		REQUIRE(month <= 12);
		REQUIRE(day >= 1);
		const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
		constexpr int LENGTHS[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		REQUIRE(day <= LENGTHS[month - 1] + (month == 2 && leap ? 1 : 0));
		const std::int64_t ordinal = static_cast<std::int64_t>(year) * 400 + month * 32 + day;
		REQUIRE(ordinal > previousDay);
		previousDay = ordinal;
		previous = text;
	}
}

TEST_CASE("Downloaded files get safe names", "[update][files]")
{
	CHECK(SafeFileName("SceneryEditorX-0.2.130.631-Alpha-Setup.exe") == "SceneryEditorX-0.2.130.631-Alpha-Setup.exe");
	CHECK(SafeFileName("../../evil.exe") == "_.._evil.exe");
	CHECK(SafeFileName("..\\x") == "_x");
	CHECK(SafeFileName("a b/c:d") == "a_b_c_d");
	CHECK(SafeFileName(".hidden") == "hidden");
	CHECK(SafeFileName("") == "download");
	CHECK(SafeFileName("...") == "download");
}

TEST_CASE("SHA-256 matches the FIPS 180-4 test vectors", "[update][sha256]")
{
	using Utils::Sha256;
	CHECK(Sha256::Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(Sha256::Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(Sha256::Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(Sha256::Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
		"cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
	CHECK(Sha256::Hex("The quick brown fox jumps over the lazy dog") == "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");

	// One million 'a', fed in uneven pieces
	Sha256 million;
	const std::string chunk(997, 'a');
	size_t fed = 0;
	while (fed + chunk.size() <= 1'000'000)
	{
		million.Update(chunk);
		fed += chunk.size();
	}
	million.Update(std::string(1'000'000 - fed, 'a'));
	CHECK(million.FinishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 gives the same digest however the data is split", "[update][sha256]")
{
	using Utils::Sha256;
	std::string data;
	for (int i = 0; i < 300; ++i)
		data.push_back(static_cast<char>(i * 37 + 11));

	// Lengths around the padding boundaries (55, 56, 63, 64 bytes) and every split point of a 130-byte input
	for (const size_t length : { 0u, 1u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 128u, 130u, 300u })
	{
		const std::string_view piece = std::string_view(data).substr(0, length);
		const std::string whole = Sha256::Hex(piece);
		for (size_t split = 0; split <= length; split += length > 130 ? 17 : 1)
		{
			Sha256 hasher;
			hasher.Update(piece.substr(0, split));
			hasher.Update(piece.substr(split));
			INFO("length " << length << ", split " << split);
			CHECK(hasher.FinishHex() == whole);
		}
	}

	// The hasher starts over after Finish
	Sha256 reused;
	reused.Update("junk");
	(void)reused.Finish();
	reused.Update("abc");
	CHECK(reused.FinishHex() == Sha256::Hex("abc"));
}

TEST_CASE("SHA-256 of a file", "[update][sha256]")
{
	const fs::path path = TempFile("payload.bin");
	{
		std::ofstream file(path, std::ios::binary);
		for (int i = 0; i < 200'000; ++i)	// Larger than the 64 KB read buffer, not a multiple of it
			file.put(static_cast<char>(i % 251));
	}
	std::string data;
	for (int i = 0; i < 200'000; ++i)
		data.push_back(static_cast<char>(i % 251));

	std::string error;
	CHECK(Utils::Sha256File(path, &error) == Utils::Sha256::Hex(data));
	CHECK(error.empty());

	CHECK(Utils::Sha256File(path.parent_path() / "missing.bin", &error).empty());
	CHECK(error == "could not open the file");

	std::error_code ec;
	fs::remove_all(path.parent_path(), ec);

	CHECK(Utils::IsSha256Hex("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"));
	CHECK_FALSE(Utils::IsSha256Hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b85"));
	CHECK_FALSE(Utils::IsSha256Hex("g3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}
