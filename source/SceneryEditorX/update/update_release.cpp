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
 * update_release.cpp
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#include "update_release.h"
#include <algorithm>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <SceneryEditorX/utils/sha256.h>

// -------------------------------------------------------

namespace SceneryEditorX::Update
{
	namespace
	{
		char Lower(const char c)
		{
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		}

		std::string ToLower(const std::string_view text)
		{
			std::string out(text);
			for (char &c : out)
				c = Lower(c);
			return out;
		}

		bool EqualsNoCase(const std::string_view a, const std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
			{
				if (Lower(a[i]) != Lower(b[i]))
					return false;
			}
			return true;
		}

		bool StartsWithNoCase(const std::string_view text, const std::string_view prefix)
		{
			return text.size() >= prefix.size() && EqualsNoCase(text.substr(0, prefix.size()), prefix);
		}

		/* @brief A string member; "" when it is missing, null or not a string (GitHub sends null for empty names and notes). */
		std::string StringField(const nlohmann::json &object, const char *key)
		{
			const auto it = object.find(key);
			return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
		}

		bool BoolField(const nlohmann::json &object, const char *key)
		{
			const auto it = object.find(key);
			return it != object.end() && it->is_boolean() && it->get<bool>();
		}

		std::uint64_t SizeField(const nlohmann::json &object, const char *key)
		{
			const auto it = object.find(key);
			if (it == object.end() || !it->is_number_integer())
				return 0;
			const std::int64_t value = it->get<std::int64_t>();
			return value > 0 ? static_cast<std::uint64_t>(value) : 0;
		}

		std::string_view TrimLine(std::string_view line)
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
				line.remove_suffix(1);
			while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
				line.remove_prefix(1);
			return line;
		}

		/* @brief Markdown inline markup out of one line: links keep their text, emphasis / code / <kbd> markers go. */
		std::string PlainInline(const std::string_view line)
		{
			std::string out;
			out.reserve(line.size());
			for (size_t i = 0; i < line.size();)
			{
				const std::string_view rest = line.substr(i);
				if (rest.starts_with("**") || rest.starts_with("__"))
				{
					i += 2;
					continue;
				}
				if (rest.front() == '`')
				{
					++i;
					continue;
				}
				if (StartsWithNoCase(rest, "<kbd>"))
				{
					i += 5;
					continue;
				}
				if (StartsWithNoCase(rest, "</kbd>"))
				{
					i += 6;
					continue;
				}
				if (StartsWithNoCase(rest, "<br>") || StartsWithNoCase(rest, "<br/>"))
				{
					out.push_back(' ');
					i += rest[3] == '>' ? 4 : 5;
					continue;
				}
				// [text](target) and ![alt](target): the text
				const bool image = rest.starts_with("![");
				if (image || rest.front() == '[')
				{
					const size_t open = image ? 1 : 0;
					const size_t close = rest.find("](", open);
					const size_t end = close == std::string_view::npos ? std::string_view::npos : rest.find(')', close + 2);
					if (close != std::string_view::npos && end != std::string_view::npos && rest.substr(open + 1, close - open - 1).find('[') == std::string_view::npos)
					{
						out += PlainInline(rest.substr(open + 1, close - open - 1));
						i += end + 1;
						continue;
					}
				}
				out.push_back(rest.front());
				++i;
			}
			return out;
		}
	}

	std::string Version::ToString() const
	{
		return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) + "." + std::to_string(build);
	}

	std::optional<Version> ParseVersion(std::string_view text)
	{
		if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
			text.remove_prefix(1);

		std::uint32_t parts[4] = { 0, 0, 0, 0 };
		size_t count = 0;
		size_t digits = 0;
		std::uint64_t value = 0;
		for (size_t i = 0; i <= text.size(); ++i)
		{
			if (i == text.size() || text[i] == '.')
			{
				if (digits == 0 || count == 4)
					return std::nullopt;
				parts[count++] = static_cast<std::uint32_t>(value);
				digits = 0;
				value = 0;
				continue;
			}
			if (text[i] < '0' || text[i] > '9' || ++digits > 9)
				return std::nullopt;
			value = value * 10 + static_cast<std::uint64_t>(text[i] - '0');
		}
		if (count < 3)
			return std::nullopt;
		return Version{ parts[0], parts[1], parts[2], parts[3] };
	}

	std::string Release::PublishedDate() const
	{
		if (publishedAt.size() < 10 || publishedAt[4] != '-' || publishedAt[7] != '-')
			return {};
		return publishedAt.substr(0, 10);
	}

	bool ParseRelease(const std::string_view json, Release &release, std::string &error)
	{
		release = Release();
		nlohmann::json root;
		try
		{
			root = nlohmann::json::parse(json);
		}
		catch (const std::exception &e)
		{
			error = std::string("not JSON: ") + e.what();
			return false;
		}
		if (!root.is_object())
		{
			error = "not a release object";
			return false;
		}

		release.tag = StringField(root, "tag_name");
		const std::optional<Version> version = ParseVersion(release.tag);
		if (!version)
		{
			error = release.tag.empty() ? std::string("the release has no tag") : "the release tag \"" + release.tag + "\" is not a version";
			return false;
		}
		release.version = *version;
		release.title = StringField(root, "name");
		if (release.title.empty())
			release.title = release.tag;
		release.notes = StringField(root, "body");
		release.pageUrl = StringField(root, "html_url");
		release.publishedAt = StringField(root, "published_at");
		release.draft = BoolField(root, "draft");
		release.prerelease = BoolField(root, "prerelease");

		if (const auto assets = root.find("assets"); assets != root.end() && assets->is_array())
		{
			for (const nlohmann::json &item : *assets)
			{
				if (!item.is_object())
					continue;
				ReleaseAsset asset;
				asset.name = StringField(item, "name");
				asset.downloadUrl = StringField(item, "browser_download_url");
				asset.contentType = StringField(item, "content_type");
				asset.size = SizeField(item, "size");
				const std::string digest = StringField(item, "digest");
				if (StartsWithNoCase(digest, "sha256:") && Utils::IsSha256Hex(std::string_view(digest).substr(7)))
					asset.sha256 = ToLower(std::string_view(digest).substr(7));
				if (!asset.name.empty())
					release.assets.push_back(std::move(asset));
			}
		}
		return true;
	}

	const ReleaseAsset *FindAsset(const Release &release, const std::string_view name)
	{
		for (const ReleaseAsset &asset : release.assets)
		{
			if (EqualsNoCase(asset.name, name))
				return &asset;
		}
		return nullptr;
	}

	std::optional<Version> InstallerFileVersion(const std::string_view fileName)
	{
		constexpr std::string_view PREFIX = "sceneryeditorx-";
		constexpr std::string_view SUFFIX = "-setup.exe";
		const std::string name = ToLower(fileName);
		if (name.size() <= PREFIX.size() + SUFFIX.size() || !name.starts_with(PREFIX) || !name.ends_with(SUFFIX))
			return std::nullopt;
		const std::string_view middle = std::string_view(name).substr(PREFIX.size(), name.size() - PREFIX.size() - SUFFIX.size());
		const size_t dash = middle.find('-');
		if (dash == std::string_view::npos)
			return std::nullopt;
		const std::string_view channel = middle.substr(dash + 1);
		const bool channelOk = !channel.empty() && std::all_of(channel.begin(), channel.end(), [](const char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');	// "alpha", "beta", "rc1" (lowercased above)
		});
		return channelOk ? ParseVersion(middle.substr(0, dash)) : std::nullopt;
	}

	const ReleaseAsset *FindWindowsInstaller(const Release &release)
	{
		for (const ReleaseAsset &asset : release.assets)
		{
			// Compared as versions, so a three-part one ("0.2.63") matches the tag's with the build number (".0") or without
			if (const std::optional<Version> version = InstallerFileVersion(asset.name); version && *version == release.version)
				return &asset;
		}
		return nullptr;
	}

	std::string FindChecksum(const std::string_view listing, const std::string_view fileName)
	{
		size_t start = 0;
		while (start <= listing.size())
		{
			size_t end = listing.find('\n', start);
			if (end == std::string_view::npos)
				end = listing.size();
			const std::string_view line = TrimLine(listing.substr(start, end - start));
			start = end + 1;

			if (line.empty() || line.front() == '#' || line.size() < 66)
				continue;
			const std::string_view hash = line.substr(0, 64);
			if (!Utils::IsSha256Hex(hash) || (line[64] != ' ' && line[64] != '\t'))
				continue;
			std::string_view name = TrimLine(line.substr(65));
			if (!name.empty() && name.front() == '*')
				name.remove_prefix(1);
			if (EqualsNoCase(name, fileName))
				return ToLower(hash);
		}
		return {};
	}

	bool IsReleaseDownloadUrl(const std::string_view url, const std::string_view repository)
	{
		if (repository.empty() || repository.find('/') == std::string_view::npos)
			return false;
		for (const char c : url)
		{
			if (static_cast<unsigned char>(c) <= ' ' || c == '\\' || c == '"' || c == '<' || c == '>')
				return false;
		}
		const std::string prefix = "https://github.com/" + std::string(repository) + "/releases/download/";
		if (url.size() <= prefix.size() || !StartsWithNoCase(url, prefix))
			return false;

		// Exactly <tag>/<file> after the prefix: two names of letters, digits and . _ + -, neither starting with '.'
		// (so no "." or ".." segments), no '%' escapes, no further segments, query or fragment
		const std::string_view rest = url.substr(prefix.size());
		const size_t slash = rest.find('/');
		if (slash == std::string_view::npos)
			return false;
		const auto validName = [](const std::string_view name)
		{
			if (name.empty() || name.front() == '.')
				return false;
			return std::all_of(name.begin(), name.end(), [](const char c)
			{
				return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-';
			});
		};
		return validName(rest.substr(0, slash)) && validName(rest.substr(slash + 1));
	}

	bool IsLoopbackUrl(const std::string_view url)
	{
		std::string_view rest;
		if (StartsWithNoCase(url, "http://"))
			rest = url.substr(7);
		else if (StartsWithNoCase(url, "https://"))
			rest = url.substr(8);
		else
			return false;

		const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
		if (authority.find('@') != std::string_view::npos || authority.find_first_of(" \\") != std::string_view::npos)
			return false;
		std::string_view host = authority;
		if (host.starts_with('['))
		{
			const size_t close = host.find(']');
			if (close == std::string_view::npos)
				return false;
			const std::string_view after = host.substr(close + 1);
			if (!after.empty() && after.front() != ':')
				return false;
			host = host.substr(0, close + 1);
		}
		else if (const size_t colon = host.find(':'); colon != std::string_view::npos)
			host = host.substr(0, colon);
		return host == "127.0.0.1" || EqualsNoCase(host, "localhost") || host == "[::1]";
	}

	bool IsCheckDue(const std::int64_t lastCheck, const std::int64_t now, const std::int64_t interval)
	{
		if (lastCheck <= 0 || now < lastCheck)
			return true;
		return now - lastCheck >= interval;
	}

	Offer Evaluate(const Version &running, const Release &latest, const std::string_view skippedVersion)
	{
		if (latest.draft || latest.version <= running)
			return Offer::None;
		if (const std::optional<Version> skipped = ParseVersion(skippedVersion); skipped && *skipped == latest.version)
			return Offer::Skipped;
		return Offer::Newer;
	}

	std::string PlainTextNotes(const std::string_view markdown)
	{
		std::string out;
		int blankRun = 0;
		size_t start = 0;
		while (start <= markdown.size())
		{
			size_t end = markdown.find('\n', start);
			if (end == std::string_view::npos)
				end = markdown.size();
			std::string_view line = markdown.substr(start, end - start);
			start = end + 1;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
				line.remove_suffix(1);

			if (line.empty())
			{
				if (!out.empty() && ++blankRun == 1)
					out.push_back('\n');
				continue;
			}
			blankRun = 0;

			// Indentation, then a heading's #s or a list item's marker
			size_t indent = 0;
			while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t'))
				++indent;
			std::string_view body = line.substr(indent);
			std::string lead(line.substr(0, indent));
			if (body.starts_with('#'))
			{
				const size_t hashes = body.find_first_not_of('#');
				if (hashes != std::string_view::npos && hashes <= 6 && body[hashes] == ' ')
					body.remove_prefix(hashes + 1);
			}
			else if (body.size() > 1 && (body[0] == '-' || body[0] == '*' || body[0] == '+') && body[1] == ' ')
			{
				lead += "- ";
				body.remove_prefix(2);
			}
			out += lead;
			out += PlainInline(body);
			out.push_back('\n');
		}
		while (!out.empty() && out.back() == '\n')
			out.pop_back();
		return out;
	}

	std::string FormatUtcTime(const std::int64_t seconds)
	{
		std::int64_t days = seconds / 86400;
		std::int64_t time = seconds % 86400;
		if (time < 0)
		{
			time += 86400;
			--days;
		}
		// Days since 1970-01-01 to year / month / day (Howard Hinnant's civil_from_days)
		days += 719468;
		const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
		const std::int64_t dayOfEra = days - era * 146097;
		const std::int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
		const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
		const std::int64_t monthIndex = (5 * dayOfYear + 2) / 153;	// March = 0
		const std::int64_t day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
		const std::int64_t month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
		const std::int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);

		char text[64];
		snprintf(text, sizeof(text), "%04lld-%02lld-%02lld %02lld:%02lld UTC", static_cast<long long>(year), static_cast<long long>(month),
			static_cast<long long>(day), static_cast<long long>(time / 3600), static_cast<long long>(time % 3600 / 60));
		return text;
	}

	std::string SafeFileName(const std::string_view name)
	{
		std::string out;
		out.reserve(name.size());
		for (const char c : name)
		{
			const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
			if (keep && !(c == '.' && out.empty()))
				out.push_back(c);
			else if (!keep)
				out.push_back('_');
		}
		while (!out.empty() && out.front() == '.')
			out.erase(out.begin());
		return out.empty() ? std::string("download") : out;
	}
}

// -------------------------------------------------------
