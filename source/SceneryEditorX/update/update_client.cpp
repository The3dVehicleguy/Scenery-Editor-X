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
 * update_client.cpp
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#include "update_client.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <resource.h>
#include <SceneryEditorX/filesystem/vfs.h>
#include <SceneryEditorX/logging/logging.hpp>
#include <SceneryEditorX/utils/sha256.h>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
	#include <shellapi.h>
#endif

// -------------------------------------------------------

namespace SceneryEditorX::Update
{
	namespace fs = std::filesystem;

	namespace
	{
		constexpr const char *LOG_TAG = "Update";
		constexpr const char *LAST_CHECK_FILE = "latest_release.json";
		constexpr std::uint64_t MAX_ANSWER_BYTES = 4ull << 20;		// A release's JSON (notes included) is a few KB
		constexpr std::uint64_t MAX_CHECKSUM_BYTES = 64ull << 10;
		constexpr std::uint64_t MAX_INSTALLER_BYTES = 2ull << 30;	// When the release doesn't give the size

		/* @brief Closes a lock HashAndLock returned (Windows; nothing elsewhere) and clears it. */
		void CloseInstallerLock(void *&lock)
		{
#if defined(_WIN32)
			if (lock)
				CloseHandle(static_cast<HANDLE>(lock));
#endif
			lock = nullptr;
		}

		struct ClientState
		{
			std::mutex mutex;			// Guards status, options and installerLock
			Status status;
			ClientOptions options;
			void *installerLock = nullptr;	// Windows: the handle keeping status.installer unchanged from its check until it runs

			std::mutex workerMutex;		// Guards worker: started, joined and replaced by StartWork, Shutdown, the destructor
			std::thread worker;			// Never takes workerMutex itself
			std::atomic<bool> busy{ false };
			std::atomic<bool> cancel{ false };
			std::atomic<std::uint64_t> downloaded{ 0 };
			std::atomic<std::uint64_t> total{ 0 };
			std::atomic<bool> initialized{ false };
			std::atomic<bool> curlReady{ false };

			~ClientState()
			{
				// Shutdown() has normally joined the worker. If the process exits without it, stop the worker and wait for
				// it (the progress callback aborts a transfer at once): a detached one would go on using this state while
				// static destruction frees it.
				cancel.store(true);
				std::scoped_lock lock(workerMutex);
				if (worker.joinable())
				{
					if (worker.get_id() != std::this_thread::get_id())
						worker.join();
					else
						worker.detach();	// The worker itself is ending the process: it can't wait for itself
				}
				CloseInstallerLock(installerLock);
			}
		};

		ClientState &S()
		{
			static ClientState state;
			return state;
		}

		std::int64_t NowSeconds()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}

		std::string UserAgent()
		{
			return std::string("SceneryEditorX/") + SEDX_VERSION_STRING + " (+https://github.com/" + REPOSITORY + ")";
		}

		/* @brief Changes the status under the lock and moves its generation on. */
		template <typename Change>
		void ChangeStatus(Change &&change)
		{
			ClientState &s = S();
			std::scoped_lock lock(s.mutex);
			change(s.status);
			++s.status.generation;
		}

		// -------------------------------------------------------
		// HTTP (libcurl)

		struct HttpResult
		{
			bool transferred = false;	// The server answered (any status)
			bool cancelled = false;
			bool tooLarge = false;
			long status = 0;
			std::string error;
		};

		struct Transfer
		{
			std::function<bool(const char *, std::size_t)> sink;	// False stops the transfer
			std::uint64_t limit = 0;
			std::uint64_t received = 0;
			bool tooLarge = false;
			bool reportProgress = false;
			long timeoutSeconds = 0;	// Whole transfer; 0 for none (a setup may take long on a slow line)
		};

		std::size_t WriteData(const char *data, const std::size_t size, const std::size_t count, void *userdata)
		{
			Transfer *transfer = static_cast<Transfer *>(userdata);
			const std::size_t bytes = size * count;
			transfer->received += bytes;
			if (transfer->received > transfer->limit)
			{
				transfer->tooLarge = true;
				return 0;	// Stops the transfer (CURLE_WRITE_ERROR)
			}
			return transfer->sink(data, bytes) ? bytes : 0;
		}

		/* @brief Where a redirect may go (IsAllowedRedirect); filled with the refused target when it may not. */
		struct RedirectCheck
		{
			bool testServer = false;
			std::string refused;
		};

		/**
		 * @brief Reads each response header and stops the transfer at a Location header that leads off GitHub, before
		 * curl follows it. A relative Location stays on the same host.
		 */
		std::size_t CheckHeader(char *data, const std::size_t size, const std::size_t count, void *userdata)
		{
			RedirectCheck *check = static_cast<RedirectCheck *>(userdata);
			const std::size_t bytes = size * count;
			std::string_view line(data, bytes);
			constexpr std::string_view NAME = "location:";
			if (line.size() <= NAME.size())
				return bytes;
			for (std::size_t i = 0; i < NAME.size(); ++i)
			{
				const char c = line[i] >= 'A' && line[i] <= 'Z' ? static_cast<char>(line[i] - 'A' + 'a') : line[i];
				if (c != NAME[i])
					return bytes;
			}
			std::string_view target = line.substr(NAME.size());
			while (!target.empty() && (target.front() == ' ' || target.front() == '\t'))
				target.remove_prefix(1);
			while (!target.empty() && (target.back() == '\r' || target.back() == '\n' || target.back() == ' ' || target.back() == '\t'))
				target.remove_suffix(1);
			if (target.empty() || (target.front() == '/' && !target.starts_with("//")))
				return bytes;	// Relative: the same host
			const std::string absolute = target.starts_with("//") ? "https:" + std::string(target) : std::string(target);
			if (IsAllowedRedirect(absolute, check->testServer))
				return bytes;
			check->refused = absolute;
			return 0;	// Stops the transfer (CURLE_WRITE_ERROR) before the redirect is followed
		}

		int Progress(void *userdata, const curl_off_t downloadTotal, const curl_off_t downloadNow, curl_off_t, curl_off_t)
		{
			const Transfer *transfer = static_cast<const Transfer *>(userdata);
			ClientState &s = S();
			if (s.cancel.load())
				return 1;	// Aborts the transfer
			if (transfer->reportProgress)
			{
				s.downloaded.store(downloadNow > 0 ? static_cast<std::uint64_t>(downloadNow) : 0);
				if (downloadTotal > 0)
					s.total.store(static_cast<std::uint64_t>(downloadTotal));
			}
			return 0;
		}

		HttpResult HttpGet(const std::string &url, const bool api, Transfer &transfer, const bool testServer)
		{
			HttpResult result;
			CURL *curl = curl_easy_init();
			if (!curl)
			{
				result.error = "libcurl could not be initialized";
				return result;
			}

			curl_slist *headers = nullptr;
			if (api)
			{
				headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
				headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
			}
			else
				headers = curl_slist_append(headers, "Accept: application/octet-stream");

			const std::string agent = UserAgent();
			char errorBuffer[CURL_ERROR_SIZE] = {};
			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			// Plain http only for a test server on this computer (Init checks). Redirects follow the same rule, and
			// CheckHeader decides each one first (IsAllowedRedirect): https to GitHub, or http only back to the test
			// server's own loopback address
#if LIBCURL_VERSION_NUM >= 0x075500	// 7.85: the string options; the bitmask ones are deprecated since
			const char *protocols = testServer ? "http,https" : "https";
			curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols);
			curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
#else	// An older system libcurl (Linux distributions)
			const long protocols = testServer ? static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS) : static_cast<long>(CURLPROTO_HTTPS);
			curl_easy_setopt(curl, CURLOPT_PROTOCOLS, protocols);
			curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, protocols);
#endif
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
			curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());		// GitHub's API refuses requests without one
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
			curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 256L);
			curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
			if (transfer.timeoutSeconds > 0)
				curl_easy_setopt(curl, CURLOPT_TIMEOUT, transfer.timeoutSeconds);
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			RedirectCheck redirectCheck;
			redirectCheck.testServer = testServer;
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CheckHeader);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, &redirectCheck);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
			curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);

			const CURLcode code = curl_easy_perform(curl);
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
			curl_easy_cleanup(curl);
			curl_slist_free_all(headers);

			result.tooLarge = transfer.tooLarge;
			if (code == CURLE_ABORTED_BY_CALLBACK)
				result.cancelled = true;
			else if (!redirectCheck.refused.empty())
				result.error = "redirected away from GitHub, to " + redirectCheck.refused;
			else if (transfer.tooLarge)
				result.error = "the answer was larger than expected";
			else if (code != CURLE_OK)
				result.error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(code);
			else
				result.transferred = true;
			return result;
		}

		/* @brief An HTTP status as a sentence for the update window. */
		std::string DescribeStatus(const long status, const bool api)
		{
			if (api && (status == 403 || status == 429))
				return "GitHub limits how often it can be asked: try again in an hour";
			if (status == 404)
				return "GitHub has no such file (HTTP 404)";
			return "GitHub answered HTTP " + std::to_string(status);
		}

		bool GetText(const std::string &url, const bool api, const std::uint64_t limit, const bool testServer, std::string &body, HttpResult &result)
		{
			body.clear();
			Transfer transfer;
			transfer.limit = limit;
			transfer.timeoutSeconds = 30;	// Small answers (the release, a checksum listing): never worth waiting longer
			transfer.sink = [&body](const char *data, const std::size_t size)
			{
				body.append(data, size);
				return true;
			};
			result = HttpGet(url, api, transfer, testServer);
			return result.transferred && result.status >= 200 && result.status < 300;
		}

		// -------------------------------------------------------
		// The last check, kept in cache:/updates/latest_release.json: { "checked_at": <unix seconds>, "release": {...} | null }

		struct CachedCheck
		{
			std::int64_t checkedAt = 0;
			std::optional<Release> release;
		};

		std::optional<CachedCheck> ReadCachedCheck()
		{
			std::string text;
			if (!VFS::ReadFile(UpdateClient::CacheFolder() / LAST_CHECK_FILE, text))
				return std::nullopt;
			try
			{
				const nlohmann::json root = nlohmann::json::parse(text);
				CachedCheck cached;
				if (const auto at = root.find("checked_at"); at != root.end() && at->is_number_integer())
					cached.checkedAt = at->get<std::int64_t>();
				if (const auto release = root.find("release"); release != root.end() && release->is_object())
				{
					Release parsed;
					std::string error;
					if (ParseRelease(release->dump(), parsed, error))
						cached.release = std::move(parsed);
				}
				return cached;
			}
			catch (const std::exception &e)
			{
				SEDX_CORE_WARN_TAG(LOG_TAG, "The last update check's answer could not be read ({}): checking again", e.what());
				return std::nullopt;
			}
		}

		void WriteCachedCheck(const std::int64_t checkedAt, const std::string *releaseJson)
		{
			nlohmann::json root;
			root["checked_at"] = checkedAt;
			root["release"] = nullptr;
			if (releaseJson)
			{
				try
				{
					root["release"] = nlohmann::json::parse(*releaseJson);
				}
				catch (const std::exception &)
				{
				}
			}
			std::string error;
			if (!VFS::WriteFileAtomic(UpdateClient::CacheFolder() / LAST_CHECK_FILE, root.dump(1) + "\n", &error))
				SEDX_CORE_WARN_TAG(LOG_TAG, "Could not keep the update check's answer: {}", error);
		}

		/* @brief Status for a release that was found (checked now or read from the cache). */
		void ApplyRelease(Status &status, Release release, const bool manual, const bool fromCache, const std::string &skippedVersion)
		{
			status.offer = Evaluate(UpdateClient::RunningVersion(), release, manual ? std::string_view() : std::string_view(skippedVersion));
			status.state = status.offer == Offer::None ? State::UpToDate : State::Available;
			status.manual = manual;
			status.fromCache = fromCache;
			status.release = std::move(release);
			status.error.clear();
		}

		// -------------------------------------------------------
		// Work (on the client's thread)

		/* @brief Runs work on the client's thread unless something is running already; begin() runs first, here. */
		bool StartWork(const std::function<void()> &begin, std::function<void()> work)
		{
			ClientState &s = S();
			if (!s.initialized.load() || s.busy.exchange(true))
				return false;
			std::scoped_lock lock(s.workerMutex);
			if (!s.initialized.load())
			{
				// Shutdown() ran in between: it clears initialized under this lock, so nothing starts after it
				s.busy.store(false);
				return false;
			}
			if (s.worker.joinable())
				s.worker.join();	// The last piece of work has finished: it cleared busy as its very last step
			s.cancel.store(false);
			{
				std::scoped_lock guard(s.mutex);
				CloseInstallerLock(s.installerLock);	// New work replaces a downloaded setup (a download locks it again)
			}
			begin();	// Before the thread starts, so work that finishes at once can't be overwritten by it
			try
			{
				s.worker = std::thread([work = std::move(work)]()
				{
					work();
					S().busy.store(false);
				});
			}
			catch (const std::system_error &e)
			{
				// No thread: undo busy (or nothing could start again) and say why instead of what begin() announced
				s.busy.store(false);
				SEDX_CORE_ERROR_TAG(LOG_TAG, "The update thread could not start: {}", e.what());
				const std::string error = std::string("a thread could not start: ") + e.what();
				ChangeStatus([&error](Status &status)
				{
					status.state = status.state == State::Downloading ? State::DownloadFailed : State::CheckFailed;
					status.error = error;
				});
				return false;
			}
			return true;
		}

		/**
		 * @brief A check that didn't get an answer. One the user asked for shows why; an automatic one keeps what an
		 * earlier check found (the cache), so an editor started offline still offers a release found yesterday.
		 */
		void CheckFailed(Status &status, const bool manual, const std::string &error)
		{
			status.manual = manual;
			if (!manual && status.release)
			{
				status.state = status.offer == Offer::None ? State::UpToDate : State::Available;
				status.error.clear();
				return;
			}
			status.state = State::CheckFailed;
			status.error = error;
		}

		void RunCheck(const bool manual)
		{
			ClientState &s = S();
			std::string url;
			bool testServer = false;
			std::string skipped;
			{
				std::scoped_lock lock(s.mutex);
				url = s.options.latestReleaseUrl.empty() ? std::string("https://api.github.com/repos/") + REPOSITORY + "/releases/latest" : s.options.latestReleaseUrl;
				testServer = s.options.testServer;
				skipped = s.options.skippedVersion;
			}

			std::string body;
			HttpResult result;
			const bool ok = GetText(url, true, MAX_ANSWER_BYTES, testServer, body, result);
			const std::int64_t now = NowSeconds();
			if (result.cancelled)
			{
				ChangeStatus([manual](Status &status)
				{
					status.manual = manual;
					status.error.clear();
					status.state = !status.release ? State::Idle : status.offer == Offer::None ? State::UpToDate : State::Available;
				});
				return;
			}
			if (result.transferred && result.status == 404)
			{
				// Usually no release has been published yet. A renamed or private repository (or a wrong --update-url)
				// answers 404 too, so a release found before is kept, like after any failed check, and neither the cache
				// nor its check time changes: the next start asks again. The cache counts too (a check from the Help menu
				// with automatic checks off hasn't read it).
				const std::optional<CachedCheck> cached = ReadCachedCheck();
				bool keptRelease = false;
				ChangeStatus([&](Status &status)
				{
					if (!status.release && cached && cached->release)
					{
						ApplyRelease(status, *cached->release, manual, true, skipped);
						status.checkedAt = cached->checkedAt;
					}
					keptRelease = status.release.has_value();
					if (keptRelease)
					{
						CheckFailed(status, manual, "GitHub has no newest release at " + url + " (HTTP 404)");
						return;
					}
					status.state = State::UpToDate;
					status.offer = Offer::None;
					status.manual = manual;
					status.fromCache = false;
					status.checkedAt = now;
					status.error.clear();
				});
				if (keptRelease)
					SEDX_CORE_WARN_TAG(LOG_TAG, "GitHub answered HTTP 404 for {}: keeping the release found before", url);
				else
				{
					WriteCachedCheck(now, nullptr);
					SEDX_CORE_INFO_TAG(LOG_TAG, "No release has been published yet ({})", url);
				}
				return;
			}
			if (!ok)
			{
				const std::string error = result.transferred ? DescribeStatus(result.status, true) : result.error;
				SEDX_CORE_WARN_TAG(LOG_TAG, "The update check failed: {}", error);
				ChangeStatus([&](Status &status) { CheckFailed(status, manual, error); });
				return;
			}

			Release release;
			std::string error;
			if (!ParseRelease(body, release, error))
			{
				SEDX_CORE_WARN_TAG(LOG_TAG, "GitHub's answer to the update check could not be read: {}", error);
				ChangeStatus([&](Status &status) { CheckFailed(status, manual, "GitHub's answer could not be read: " + error); });
				return;
			}

			WriteCachedCheck(now, &body);
			SEDX_CORE_INFO_TAG(LOG_TAG, "Newest release: {} (running {})", release.version.ToString(), SEDX_VERSION_STRING);
			ChangeStatus([&](Status &status)
			{
				ApplyRelease(status, std::move(release), manual, false, skipped);
				status.checkedAt = now;
			});
		}

		/**
		 * @brief The SHA-256 of a downloaded setup. On Windows it is read through a handle that lets others read the file
		 * (Setup itself, the permission prompt) but not write, rename or delete it, and that handle comes back in lock,
		 * open: anything running as the user can write to the cache folder, so the file stays as checked until it has
		 * started. Elsewhere it is a plain read (those builds don't run setups). Empty, error set, when it can't be read.
		 */
		std::string HashAndLock(const fs::path &path, void *&lock, std::string &error, bool &cancelled)
		{
			lock = nullptr;
			cancelled = false;
#if defined(_WIN32)
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				error = "could not open " + VFS::ToUtf8(path.filename()) + " (Windows error " + std::to_string(GetLastError()) + ")";
				return {};
			}
			Utils::Sha256 hasher;
			std::vector<char> buffer(1 << 16);
			for (;;)
			{
				DWORD read = 0;
				if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
				{
					error = "could not read " + VFS::ToUtf8(path.filename()) + " (Windows error " + std::to_string(GetLastError()) + ")";
					CloseHandle(file);
					return {};
				}
				if (read == 0)
					break;
				hasher.Update(buffer.data(), read);
				if (S().cancel.load())	// Cancel / Shutdown: a setup of gigabytes isn't hashed to the end first
				{
					cancelled = true;
					CloseHandle(file);
					return {};
				}
			}
			lock = file;
			return hasher.FinishHex();
#else
			return Utils::Sha256File(path, &error);
#endif
		}

		/**
		 * @brief A file the client may download: one of the repository's release files, or one on a test server on this
		 * computer. The SHA-256 that checks a setup comes from the same trust: GitHub's API (https) and the repository's
		 * own release files, or, with --update-url, the developer's own local server, which stands in for GitHub there.
		 */
		bool IsTrustedDownload(const std::string_view url, const bool testServer)
		{
			return IsReleaseDownloadUrl(url, REPOSITORY) || (testServer && IsLoopbackUrl(url));
		}

		/**
		 * @brief The SHA-256 the release publishes for a file: GitHub's digest, else <file>.sha256, else SHA256SUMS.txt.
		 * Empty when there is none (error says why) or the download was cancelled while a listing was read (cancelled).
		 */
		std::string ExpectedSha256(const Release &release, const ReleaseAsset &file, const bool testServer, std::string &error, bool &cancelled)
		{
			cancelled = false;
			if (!file.sha256.empty())
				return file.sha256;

			for (const std::string &name : { file.name + ".sha256", std::string("SHA256SUMS.txt") })
			{
				const ReleaseAsset *listing = FindAsset(release, name);
				if (!listing)
					continue;
				// Each listing that can't be used says why in the log before the next one is tried
				if (!IsTrustedDownload(listing->downloadUrl, testServer))
				{
					SEDX_CORE_WARN_TAG(LOG_TAG, "{} isn't a release file of {}: not used for the setup's SHA-256", listing->downloadUrl, REPOSITORY);
					continue;
				}
				std::string body;
				HttpResult result;
				if (!GetText(listing->downloadUrl, false, MAX_CHECKSUM_BYTES, testServer, body, result))
				{
					if (result.cancelled)
					{
						cancelled = true;
						return {};
					}
					SEDX_CORE_WARN_TAG(LOG_TAG, "Could not read {} for the setup's SHA-256: {}", name,
						result.transferred ? DescribeStatus(result.status, false) : result.error);
					continue;
				}
				if (std::string hash = FindChecksum(body, file.name); !hash.empty())
					return hash;
				SEDX_CORE_WARN_TAG(LOG_TAG, "{} doesn't list {}", name, file.name);
			}
			error = "the release publishes no SHA-256 for " + file.name + ", so the download can't be checked";
			return {};
		}

		/**
		 * @brief The file a setup downloads into (<setup>.part). On Windows it is held for the whole download: others may
		 * read it but not write, rename or delete it, and MoveTo gives it its final name through the same handle, so
		 * nothing can change or swap it between the download and the name the lock (HashAndLock) then opens. Elsewhere a
		 * plain file and rename (those builds don't run setups).
		 */
		class PartialDownload
		{
		public:
			explicit PartialDownload(fs::path path) : m_Path(std::move(path)) {}
			~PartialDownload() { Close(); }
			PartialDownload(const PartialDownload &) = delete;
			PartialDownload &operator=(const PartialDownload &) = delete;

			bool Open(std::string &error)
			{
#if defined(_WIN32)
				// DELETE: renamed through this handle (MoveTo)
				const HANDLE file = CreateFileW(m_Path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
				if (file == INVALID_HANDLE_VALUE)
				{
					error = "could not write " + VFS::ToUtf8(m_Path) + " (Windows error " + std::to_string(GetLastError()) + ")";
					return false;
				}
				m_File = file;
#else
				m_File.open(m_Path, std::ios::binary | std::ios::trunc);
				if (!m_File)
				{
					error = "could not write " + VFS::ToUtf8(m_Path);
					return false;
				}
#endif
				return true;
			}

			bool Write(const char *data, std::size_t size)
			{
#if defined(_WIN32)
				while (size > 0 && m_Ok)
				{
					DWORD written = 0;
					const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30));
					m_Ok = WriteFile(m_File, data, chunk, &written, nullptr) && written > 0;
					data += written;
					size -= written;
				}
#else
				m_File.write(data, static_cast<std::streamsize>(size));
				m_Ok = m_Ok && static_cast<bool>(m_File);
#endif
				return m_Ok;
			}

			bool Ok() const { return m_Ok; }

			/* @brief Gives the finished download its final name, replacing the file there, and closes it. */
			bool MoveTo(const fs::path &target, std::string &error)
			{
#if defined(_WIN32)
				const std::wstring name = target.wstring();
				std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + (name.size() + 1) * sizeof(wchar_t));
				auto *info = reinterpret_cast<FILE_RENAME_INFO *>(buffer.data());
				info->ReplaceIfExists = TRUE;
				info->RootDirectory = nullptr;
				info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
				std::memcpy(info->FileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
				if (!SetFileInformationByHandle(m_File, FileRenameInfo, info, static_cast<DWORD>(buffer.size())))
				{
					error = "Windows error " + std::to_string(GetLastError());
					return false;
				}
				Close();
#else
				m_File.close();
				if (!m_File)
				{
					error = "could not write " + VFS::ToUtf8(m_Path);
					return false;
				}
				// Replaces the file there in one step (POSIX rename), so a failed move leaves it as it was
				std::error_code ec;
				fs::rename(m_Path, target, ec);
				if (ec)
				{
					error = ec.message();
					return false;
				}
#endif
				return true;
			}

			/* @brief Closes and deletes an unfinished, failed or unmovable download. */
			void Discard()
			{
				Close();
				std::error_code ec;
				fs::remove(m_Path, ec);
			}

		private:
			void Close()
			{
#if defined(_WIN32)
				if (m_File != INVALID_HANDLE_VALUE)
					CloseHandle(m_File);
				m_File = INVALID_HANDLE_VALUE;
#else
				if (m_File.is_open())
					m_File.close();
#endif
			}

			fs::path m_Path;
#if defined(_WIN32)
			HANDLE m_File = INVALID_HANDLE_VALUE;
#else
			std::ofstream m_File;
#endif
			bool m_Ok = true;
		};

		void RunDownload(const Release release, const ReleaseAsset installer)
		{
			ClientState &s = S();
			bool testServer = false;
			{
				std::scoped_lock lock(s.mutex);
				testServer = s.options.testServer;
			}
			const auto fail = [](const std::string &error)
			{
				SEDX_CORE_WARN_TAG(LOG_TAG, "The update download failed: {}", error);
				ChangeStatus([&](Status &status)
				{
					status.state = State::DownloadFailed;
					status.error = error;
				});
			};
			const auto cancelled = []()
			{
				ChangeStatus([](Status &status)
				{
					status.state = State::Available;
					status.error.clear();
				});
			};

			if (!IsTrustedDownload(installer.downloadUrl, testServer))
			{
				fail("the setup isn't a release file of " + std::string(REPOSITORY) + ": " + installer.downloadUrl);
				return;
			}

			std::string error;
			bool stopped = false;
			const std::string expected = ExpectedSha256(release, installer, testServer, error, stopped);
			if (expected.empty())
			{
				if (stopped)
					cancelled();
				else
					fail(error);
				return;
			}

			const fs::path folder = UpdateClient::CacheFolder();
			const fs::path target = folder / SafeFileName(installer.name);
			std::error_code ec;
			fs::create_directories(folder, ec);

			// The setup in place, locked against changes and hashed through that lock (HashAndLock): Downloaded when it
			// matches, and it stays locked until it runs, so what runs is what was checked
			bool hashCancelled = false;
			const auto lockAndCheck = [&](const fs::path &path, std::string &actual)
			{
				void *lock = nullptr;
				actual = HashAndLock(path, lock, error, hashCancelled);
				if (actual != expected)
				{
					CloseInstallerLock(lock);
					return false;
				}
				{
					std::scoped_lock guard(s.mutex);
					CloseInstallerLock(s.installerLock);
					s.installerLock = lock;
				}
				ChangeStatus([&](Status &status)
				{
					status.state = State::Downloaded;
					status.installer = path;
					status.error.clear();
				});
				return true;
			};

			// Downloaded before (and still intact): nothing to fetch
			if (std::string actual; fs::is_regular_file(target, ec) && lockAndCheck(target, actual))
			{
				SEDX_CORE_INFO_TAG(LOG_TAG, "{} was downloaded before and its SHA-256 matches", installer.name);
				return;
			}
			if (hashCancelled)
			{
				cancelled();
				return;
			}

			fs::path partial = target;
			partial += ".part";
			{
				PartialDownload file(partial);
				if (!file.Open(error))
				{
					fail(error);
					return;
				}
				Transfer transfer;
				transfer.limit = installer.size > 0 ? installer.size + (1u << 20) : MAX_INSTALLER_BYTES;
				transfer.reportProgress = true;
				transfer.sink = [&file](const char *data, const std::size_t size) { return file.Write(data, size); };
				s.downloaded.store(0);
				s.total.store(installer.size);
				SEDX_CORE_INFO_TAG(LOG_TAG, "Downloading {} ({} MB)", installer.name, installer.size / (1024 * 1024));
				const HttpResult result = HttpGet(installer.downloadUrl, false, transfer, testServer);

				const bool ok = result.transferred && result.status >= 200 && result.status < 300 && file.Ok();
				if (!ok)
				{
					file.Discard();
					if (result.cancelled)
						cancelled();
					else if (!file.Ok())
						fail("could not write " + VFS::ToUtf8(partial));
					else
						fail(result.transferred ? DescribeStatus(result.status, false) : result.error);
					return;
				}
				// Renamed over the file there (one that didn't match its SHA-256), never deleted first: a failed move leaves
				// it as it was
				if (!file.MoveTo(target, error))
				{
					file.Discard();
					fail("could not move the download into place: " + error);
					return;
				}
			}

			std::string actual;
			if (!lockAndCheck(target, actual))
			{
				if (hashCancelled)
				{
					cancelled();	// The file stays: a later Download checks it again ("downloaded before")
					return;
				}
				fs::remove(target, ec);
				if (!actual.empty())	// The hashes go to the log; the message (also the status bar's latest warning) stays short
					SEDX_CORE_INFO_TAG(LOG_TAG, "{}: SHA-256 {}, the release publishes {}", installer.name, actual, expected);
				fail(actual.empty() ? "the download could not be read back: " + error : "the download doesn't match the SHA-256 the release publishes");
				return;
			}
			SEDX_CORE_INFO_TAG(LOG_TAG, "Downloaded {} to {} (SHA-256 checked)", installer.name, VFS::ToUtf8(target));
		}

		/* @brief ".part" in any letter case. */
		bool InstallerPartialSuffix(const std::string_view suffix)
		{
			constexpr std::string_view PART = ".part";
			if (suffix.size() != PART.size())
				return false;
			for (size_t i = 0; i < PART.size(); ++i)
			{
				const char c = suffix[i] >= 'A' && suffix[i] <= 'Z' ? static_cast<char>(suffix[i] - 'A' + 'a') : suffix[i];
				if (c != PART[i])
					return false;
			}
			return true;
		}

		/* @brief Leftovers of earlier updates: partial downloads, and setups of versions not newer than this one. */
		void RemoveOldDownloads()
		{
			std::error_code ec;
			const fs::path folder = UpdateClient::CacheFolder();
			if (!fs::is_directory(folder, ec))
				return;
			const Version running = UpdateClient::RunningVersion();
			for (const fs::directory_entry &entry : fs::directory_iterator(folder, ec))
			{
				if (!entry.is_regular_file(ec))
					continue;
				const std::string name = VFS::ToUtf8(entry.path().filename());
				// A partial download, or a setup (named the way FindWindowsInstaller reads it, in any case) of a version
				// not newer than this one. Anything else, a file someone put there included, is left alone.
				// Only this client makes these (the download's name + ".part"); compared in any case all the same
				const bool partial = name.size() > 5 && InstallerPartialSuffix(std::string_view(name).substr(name.size() - 5));
				const std::optional<Version> version = InstallerFileVersion(name);
				if (partial || (version && *version <= running))
					fs::remove(entry.path(), ec);
			}
		}
	}

	// -------------------------------------------------------

	void UpdateClient::Init(const ClientOptions &options)
	{
		ClientState &s = S();
		if (s.initialized.load())
			return;
		// Plain http and the server's own downloads are allowed only for a test server on this computer; any other
		// address gets the rules GitHub does (https, the repository's release files only)
		ClientOptions accepted = options;
		if (accepted.testServer && !IsLoopbackUrl(accepted.latestReleaseUrl))
		{
			accepted.testServer = false;
			SEDX_CORE_WARN_TAG(LOG_TAG, "{} isn't at 127.0.0.1 or [::1]: it must use https, and only {}'s release files are downloaded",
				accepted.latestReleaseUrl, REPOSITORY);
		}
		{
			std::scoped_lock lock(s.mutex);
			s.options = accepted;
		}
		// Reference counted: the Gateway client shares it. Without it no check could work: stay uninitialized and say so
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		{
			SEDX_CORE_ERROR_TAG(LOG_TAG, "libcurl could not be initialized: no update checks in this session");
			ChangeStatus([](Status &status)
			{
				status.state = State::CheckFailed;
				status.error = "the network library (libcurl) could not be initialized";
			});
			return;
		}
		s.curlReady.store(true);
		RemoveOldDownloads();
		if (accepted.testServer)
			SEDX_CORE_WARN_TAG(LOG_TAG, "Update checks use a test server: {}", accepted.latestReleaseUrl);
		s.initialized.store(true);	// Last: nothing starts before libcurl is ready
	}

	void UpdateClient::Shutdown()
	{
		ClientState &s = S();
		{
			// Under the worker lock: StartWork checks initialized again under it, so no thread starts after this
			std::scoped_lock lock(s.workerMutex);
			if (!s.initialized.exchange(false))
				return;
			s.cancel.store(true);
			if (s.worker.joinable())
				s.worker.join();
		}
		{
			std::scoped_lock lock(s.mutex);
			CloseInstallerLock(s.installerLock);
		}
		s.busy.store(false);
		if (s.curlReady.exchange(false))
			curl_global_cleanup();
	}

	Version UpdateClient::RunningVersion()
	{
		static const Version version = ParseVersion(SEDX_VERSION_STRING).value_or(Version());
		return version;
	}

	bool UpdateClient::Check(const bool manual)
	{
		const auto begin = [manual]()
		{
			ChangeStatus([manual](Status &status)
			{
				status.state = State::Checking;
				status.manual = manual;
				status.error.clear();
			});
		};
		return StartWork(begin, [manual]() { RunCheck(manual); });
	}

	bool UpdateClient::CheckIfDue()
	{
		ClientState &s = S();
		// The same busy guard StartWork takes: no check or download can start (and be overwritten) while the cache is read
		if (!s.initialized.load() || s.busy.exchange(true))
			return false;

		std::string skipped;
		{
			std::scoped_lock lock(s.mutex);
			skipped = s.options.skippedVersion;
		}
		const std::optional<CachedCheck> cached = ReadCachedCheck();
		if (cached)
		{
			ChangeStatus([&](Status &status)
			{
				// Only over nothing newer: never over a downloaded (and locked) setup, a check's answer or a failed download
				if (status.state != State::Idle && status.state != State::CheckFailed)
					return;
				status.checkedAt = cached->checkedAt;
				if (cached->release)
					ApplyRelease(status, *cached->release, false, true, skipped);
				else
				{
					status.state = State::UpToDate;
					status.fromCache = true;
				}
			});
		}
		s.busy.store(false);
		if (cached && !IsCheckDue(cached->checkedAt, NowSeconds(), CHECK_INTERVAL_SECONDS))
		{
			SEDX_CORE_INFO_TAG(LOG_TAG, "Updates were checked less than a day ago: not asking GitHub again until then");
			return false;
		}
		return Check(false);
	}

	bool UpdateClient::Download()
	{
		const Status status = GetStatus();
		if (!CanInstall() || !status.release || status.offer == Offer::None)
			return false;
		const ReleaseAsset *installer = FindWindowsInstaller(*status.release);
		if (!installer)
			return false;

		const std::uint64_t size = installer->size;
		const auto begin = [size]()
		{
			S().downloaded.store(0);
			S().total.store(size);
			ChangeStatus([](Status &next)
			{
				next.state = State::Downloading;
				next.error.clear();
			});
		};
		return StartWork(begin, [release = *status.release, asset = *installer]() { RunDownload(release, asset); });
	}

	void UpdateClient::Cancel()
	{
		S().cancel.store(true);
	}

	Status UpdateClient::GetStatus()
	{
		ClientState &s = S();
		Status status;
		{
			std::scoped_lock lock(s.mutex);
			status = s.status;
		}
		if (status.state == State::Downloading)
		{
			status.downloaded = s.downloaded.load();
			status.total = s.total.load();
		}
		return status;
	}

	std::uint64_t UpdateClient::GetGeneration()
	{
		ClientState &s = S();
		std::scoped_lock lock(s.mutex);
		return s.status.generation;
	}

	void UpdateClient::GetProgress(std::uint64_t &downloaded, std::uint64_t &total)
	{
		downloaded = S().downloaded.load();
		total = S().total.load();
	}

	void UpdateClient::SetSkippedVersion(const std::string &version)
	{
		ClientState &s = S();
		std::scoped_lock lock(s.mutex);
		s.options.skippedVersion = version;
		if (s.status.release && !s.status.manual)
		{
			s.status.offer = Evaluate(RunningVersion(), *s.status.release, version);
			++s.status.generation;
		}
	}

	bool UpdateClient::CanInstall()
	{
#if defined(_WIN32)
		return true;
#else
		return false;
#endif
	}

	bool UpdateClient::LaunchInstaller(std::string &error)
	{
		ClientState &s = S();
		fs::path installer;
		{
			std::scoped_lock lock(s.mutex);
			if (s.status.state != State::Downloaded)
			{
				error = "no update has been downloaded";
				return false;
			}
#if defined(_WIN32)
			// Locked since its check (HashAndLock): unchanged, so nothing needs hashing again here, on the main thread
			if (!s.installerLock)
			{
				error = "the downloaded setup isn't held as checked any more: download it again";
				return false;
			}
#endif
			installer = s.status.installer;
		}

#if defined(_WIN32)
		const std::wstring file = installer.wstring();
		const std::wstring folder = installer.parent_path().wstring();
		SHELLEXECUTEINFOW info{};
		info.cbSize = sizeof(info);
		info.fMask = SEE_MASK_NOASYNC;
		info.lpVerb = L"open";		// Elevates through the Windows permission prompt when the setup asks for it
		info.lpFile = file.c_str();
		info.lpDirectory = folder.c_str();
		info.nShow = SW_SHOWNORMAL;
		if (!ShellExecuteExW(&info))
		{
			const DWORD code = GetLastError();
			error = code == ERROR_CANCELLED ? std::string("the setup was cancelled at the Windows permission prompt")
				: "Windows could not start the setup (error " + std::to_string(code) + ")";
			return false;	// Still locked: Install can be tried again
		}
		SEDX_CORE_INFO_TAG(LOG_TAG, "Started {}: closing the editor so it can update", VFS::ToUtf8(installer));
		{
			std::scoped_lock lock(s.mutex);
			CloseInstallerLock(s.installerLock);	// Setup is running: it updates (and may copy) its own file from here
		}
		return true;
#else
		error = "this build can't install updates itself: download the release from its page";
		return false;
#endif
	}

	fs::path UpdateClient::CacheFolder()
	{
		return VFS::GetAppLocation(VFS::AppLocation::Cache) / "updates";
	}
}

// -------------------------------------------------------
