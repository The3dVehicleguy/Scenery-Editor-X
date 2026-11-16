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
 * xp_drape_schedule.h
 * -------------------------------------------------------
 * Created: 15/11/2026
 * -------------------------------------------------------
 */
#pragma once
#include <array>
#include <cstdint>

// -------------------------------------------------------

namespace SceneryEditorX::XPlane
{
	/* @brief A placement's root transform as compared between checks: translation, rotation (w x y z), scale. */
	using DrapeTransform = std::array<float, 10>;

	/**
	 * @struct DrapeSchedule
	 * @brief When a placement's ATTR_draped batches are cut along the terrain again (SceneObjects keeps one per draped
	 * placement): once, when the terrain changed (or whether the placement stands on it, Invalidate()), and when a move
	 * has settled. The root's transform is read only on the placement's check frames (one in CHECK_PHASES, picked by a
	 * stable id) or when it is due anyway; a move counts as settled when two checks in a row see the same transform, so
	 * a dragged object moves rigidly and is draped once it stands still. Standard library only.
	 */
	struct DrapeSchedule
	{
		/* @brief Frames between a placement's checks (KDEN has some 3,600 draped placements: a lookup each every frame cost). */
		static constexpr uint32_t CHECK_PHASES = 8;

		bool done = false;					// Draped for transform and terrain (or left flat there)
		uint64_t terrain = 0;				// Terrain generation it was draped for
		DrapeTransform transform = {};		// Root transform it was draped for
		DrapeTransform seen = {};			// The root's transform at its last check

		/* @brief Whether a frame is one of a placement's checks, from a stable id (not an iteration order, which changes). */
		[[nodiscard]] static bool IsCheckFrame(const uint64_t id, const uint64_t frame) { return id % CHECK_PHASES == frame % CHECK_PHASES; }

		/* @brief True when it must be draped for the terrain: never yet, or the terrain changed since. */
		[[nodiscard]] bool TerrainDue(const uint64_t terrainGeneration) const { return !done || terrain != terrainGeneration; }

		/* @brief Whether the root's transform needs reading this frame. */
		[[nodiscard]] bool NeedsTransform(const uint64_t terrainGeneration, const bool checkFrame) const { return checkFrame || TerrainDue(terrainGeneration); }

		/**
		 * @brief Takes this frame's root transform (when NeedsTransform()) and says whether to drape now: for the terrain,
		 * or a move that has settled (the same transform at this check and the one before, other than the draped one).
		 */
		[[nodiscard]] bool Due(const uint64_t terrainGeneration, const bool checkFrame, const DrapeTransform &current)
		{
			bool settled = false;
			if (checkFrame)
			{
				settled = current == seen;
				seen = current;
			}
			return TerrainDue(terrainGeneration) || (settled && current != transform);
		}

		/* @brief Records a drape (or the decision to leave it flat) for a transform and terrain. */
		void Draped(const uint64_t terrainGeneration, const DrapeTransform &current)
		{
			done = true;
			terrain = terrainGeneration;
			transform = current;
		}

		/* @brief Drape again with the next frame (whether the placement stands on the terrain changed). */
		void Invalidate() { done = false; }
	};
}

// -------------------------------------------------------
