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
 * sha256.cpp
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#include "sha256.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

// -------------------------------------------------------

namespace SceneryEditorX::Utils
{
	namespace
	{
		constexpr std::uint32_t K[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
		};

		constexpr std::uint32_t RotateRight(const std::uint32_t value, const unsigned bits)
		{
			return (value >> bits) | (value << (32u - bits));
		}
	}

	Sha256::Sha256()
	{
		Reset();
	}

	void Sha256::Reset()
	{
		m_State = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
		m_Buffered = 0;
		m_Length = 0;
	}

	void Sha256::Transform(const std::uint8_t *block)
	{
		std::uint32_t w[64];
		for (int i = 0; i < 16; ++i)
		{
			w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
				(static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
		}
		for (int i = 16; i < 64; ++i)
		{
			const std::uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const std::uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}

		std::uint32_t a = m_State[0], b = m_State[1], c = m_State[2], d = m_State[3];
		std::uint32_t e = m_State[4], f = m_State[5], g = m_State[6], h = m_State[7];
		for (int i = 0; i < 64; ++i)
		{
			const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
			const std::uint32_t choose = (e & f) ^ (~e & g);
			const std::uint32_t t1 = h + s1 + choose + K[i] + w[i];
			const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
			const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
			const std::uint32_t t2 = s0 + majority;
			h = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		m_State[0] += a;
		m_State[1] += b;
		m_State[2] += c;
		m_State[3] += d;
		m_State[4] += e;
		m_State[5] += f;
		m_State[6] += g;
		m_State[7] += h;
	}

	void Sha256::Update(const void *data, std::size_t size)
	{
		const std::uint8_t *bytes = static_cast<const std::uint8_t *>(data);
		m_Length += size;
		if (m_Buffered > 0)
		{
			const std::size_t take = std::min(size, m_Block.size() - m_Buffered);
			std::memcpy(m_Block.data() + m_Buffered, bytes, take);
			m_Buffered += take;
			bytes += take;
			size -= take;
			if (m_Buffered < m_Block.size())
				return;
			Transform(m_Block.data());
			m_Buffered = 0;
		}
		while (size >= m_Block.size())
		{
			Transform(bytes);
			bytes += m_Block.size();
			size -= m_Block.size();
		}
		if (size > 0)
		{
			std::memcpy(m_Block.data(), bytes, size);
			m_Buffered = size;
		}
	}

	Sha256::Digest Sha256::Finish()
	{
		const std::uint64_t bits = m_Length * 8u;

		// 0x80, zeros up to 56 bytes into a block, then the length in bits (big endian)
		m_Block[m_Buffered++] = 0x80;
		if (m_Buffered > 56)
		{
			std::memset(m_Block.data() + m_Buffered, 0, m_Block.size() - m_Buffered);
			Transform(m_Block.data());
			m_Buffered = 0;
		}
		std::memset(m_Block.data() + m_Buffered, 0, 56 - m_Buffered);
		for (int i = 0; i < 8; ++i)
			m_Block[56 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
		Transform(m_Block.data());

		Digest digest{};
		for (int i = 0; i < 8; ++i)
		{
			digest[i * 4] = static_cast<std::uint8_t>(m_State[i] >> 24);
			digest[i * 4 + 1] = static_cast<std::uint8_t>(m_State[i] >> 16);
			digest[i * 4 + 2] = static_cast<std::uint8_t>(m_State[i] >> 8);
			digest[i * 4 + 3] = static_cast<std::uint8_t>(m_State[i]);
		}
		Reset();
		return digest;
	}

	std::string Sha256::FinishHex()
	{
		return ToHex(Finish());
	}

	std::string Sha256::Hex(const std::string_view data)
	{
		Sha256 hasher;
		hasher.Update(data);
		return hasher.FinishHex();
	}

	std::string Sha256::ToHex(const Digest &digest)
	{
		constexpr char DIGITS[] = "0123456789abcdef";
		std::string hex;
		hex.reserve(digest.size() * 2);
		for (const std::uint8_t byte : digest)
		{
			hex.push_back(DIGITS[byte >> 4]);
			hex.push_back(DIGITS[byte & 0x0F]);
		}
		return hex;
	}

	std::string Sha256File(const std::filesystem::path &path, std::string *error)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			if (error)
				*error = "could not open the file";
			return {};
		}

		Sha256 hasher;
		std::vector<char> buffer(1 << 16);
		while (file)
		{
			file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize read = file.gcount();
			if (read > 0)
				hasher.Update(buffer.data(), static_cast<std::size_t>(read));
		}
		if (file.bad())
		{
			if (error)
				*error = "could not read the file";
			return {};
		}
		return hasher.FinishHex();
	}

	bool IsSha256Hex(const std::string_view text)
	{
		if (text.size() != 64)
			return false;
		for (const char c : text)
		{
			const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
			if (!digit)
				return false;
		}
		return true;
	}
}

// -------------------------------------------------------
