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
 * sha256.h
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// -------------------------------------------------------

namespace SceneryEditorX::Utils
{
	/**
	 * @class Sha256
	 * @brief SHA-256 (FIPS 180-4), standard library only. Feed data with Update() in any number of pieces, then call
	 * Finish() once. Used to check downloads against the hashes GitHub publishes for release files.
	 */
	class Sha256
	{
	public:
		using Digest = std::array<std::uint8_t, 32>;

		Sha256();

		void Update(const void *data, std::size_t size);
		void Update(const std::string_view text) { Update(text.data(), text.size()); }

		/* @brief The digest of everything fed so far. The hasher starts over afterwards. */
		[[nodiscard]] Digest Finish();

		/* @brief Finish() as 64 lowercase hex digits. */
		[[nodiscard]] std::string FinishHex();

		/* @brief The digest of one piece of data, as lowercase hex. */
		[[nodiscard]] static std::string Hex(std::string_view data);

		/* @brief A digest as lowercase hex. */
		[[nodiscard]] static std::string ToHex(const Digest &digest);

	private:
		void Reset();
		void Transform(const std::uint8_t *block);

		std::array<std::uint32_t, 8> m_State{};
		std::array<std::uint8_t, 64> m_Block{};
		std::size_t m_Buffered = 0;		// Bytes waiting in m_Block
		std::uint64_t m_Length = 0;		// Bytes fed in total
	};

	/**
	 * @brief The SHA-256 of a file as lowercase hex, read in pieces (installers are tens of megabytes).
	 * Empty, with error set when given, when the file can't be read.
	 */
	std::string Sha256File(const std::filesystem::path &path, std::string *error = nullptr);

	/* @brief True for 64 hex digits (either case). */
	bool IsSha256Hex(std::string_view text);
}

// -------------------------------------------------------
