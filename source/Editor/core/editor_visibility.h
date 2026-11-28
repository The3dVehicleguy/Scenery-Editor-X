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
 * editor_visibility.h
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#pragma once
#include "editor_selection.h"
#include <cstdint>
#include <vector>

// -------------------------------------------------------

namespace SceneryEditorX
{
	struct EditorContext;
}

namespace SceneryEditorX::EditorVisibility
{
	/**
	 * Hide, isolate and lock (issue 104), for anything the OUTLINER lists: scene entities, the open airport's rows,
	 * groups and the airport itself, and project items.
	 *
	 * - Hiding is view-only: hidden things aren't drawn (Entity::SetHidden, which children inherit), hovered, picked,
	 *   box-selected or framed, and they are still exported. A project item's "Exclude from export" (SceneryItem::hidden,
	 *   WorldEditor's hidden flag) is a separate thing.
	 * - Isolating hides everything but the selection, the base terrain under it and the scene's lights, until it ends;
	 *   the hide state from before comes back as it was.
	 * - Locked things are drawn but can't be hovered, picked or box-selected in the viewport, and the tools leave them
	 *   alone (EditorSelection::IsReadOnly). A project item's lock is SceneryItem::locked, saved with the project.
	 *
	 * Targets are kept by what they name (an apt.dat row or group by kind and index, a project item by id), so the state
	 * survives the rebuilds that replace entities. A parent's hide or lock covers what is below it.
	 *
	 * The hide and lock state is saved with the open project (SceneryProject::view, ProjectFile::ProjectViewState) as keys
	 * that name what they are about: a project item by id, an apt.dat row by airport, kind and index, anything else by the
	 * names on its path in the scene. Keys for what isn't in the scene now (another airport's rows, entities still
	 * loading) wait and apply when it is. Isolation isn't saved. Main thread only.
	 */

	using Target = EditorSelection::Target;

	/* @brief What an OUTLINER row stands for: its airport, group or apt.dat row, its project item, or the entity itself. */
	Target ForEntity(uint64_t entityId);

	/* @brief A selection target in the form this module keeps (an airport or group entity becomes its row). */
	Target Normalize(const Target &target);

	// Hide -------------------------------------------------

	/* @brief The target's own hide toggle. */
	bool IsHidden(const Target &target);

	/* @brief Not drawn because a parent row is hidden or isolation hides it (the entity's own toggle aside). */
	bool IsHiddenByParent(uint64_t entityId);

	/* @brief Hides or shows targets as one undo step. Hidden targets leave the selection. */
	void SetHidden(EditorContext *context, const std::vector<Target> &targets, bool hidden);

	void HideSelected(EditorContext *context);
	void HideUnselected(EditorContext *context);
	void ShowAll(EditorContext *context);
	bool AnyHidden();

	// Isolate ----------------------------------------------

	bool IsIsolating();

	/* @brief How many targets the isolation shows. */
	size_t GetIsolatedCount();

	/* @brief Shows only the selection (with the base terrain under it); nothing happens when nothing is selected. */
	void Isolate(EditorContext *context);
	void ExitIsolation();
	void ToggleIsolation(EditorContext *context);

	// Lock -------------------------------------------------

	/* @brief The target's own lock toggle (a project item's SceneryItem::locked). */
	bool IsLockedSelf(const Target &target);

	/* @brief Locked itself, or below a locked row or project folder. */
	bool IsLocked(const Target &target);

	/* @brief Locked because a parent row is (the entity's own toggle aside). */
	bool IsLockedByParent(uint64_t entityId);

	/* @brief The entity or one of its parents is locked (what a pick on it would reach). */
	bool IsEntityLocked(uint64_t entityId);

	/* @brief Locks or unlocks targets as one undo step. */
	void SetLocked(EditorContext *context, const std::vector<Target> &targets, bool locked);

	void LockSelected(EditorContext *context);
	void UnlockAll(EditorContext *context);
	bool AnyLocked();

	/**
	 * @brief Hide / lock changes made between these (a drag down the OUTLINER's toggle column) become one undo step.
	 * Every Begin needs its End.
	 */
	void BeginToggleGroup();
	void EndToggleGroup();

	// Frame --------------------------------------------------

	/* @brief Once per frame before the panels: applies hide and isolation to the scene's entities when anything changed. */
	void Tick(EditorContext *context);

	/* @brief Forgets the hide, isolation and lock state of everything but project items (a project or airport closed). */
	void Reset();

	/* @brief Moves whenever the hide, isolation or lock state changes. */
	uint64_t GetRevision();

	/* @brief The view.* and item.lock commands (after FacadeTool::RegisterCommands, whose item.lock it replaces). */
	void RegisterCommands();

	/* @brief H / Shift+H / Alt+H, isolate keys: the keymap's, wherever the keyboard isn't typing. */
	void HandleShortcuts(EditorContext *context);

	/* @brief Hide, isolate and lock items for a menu (Edit and Scene menus). */
	void DrawMenuItems(EditorContext *context);

	/* @brief "Isolated: 3 items  [Exit]" centered on `centerX` at the top of the viewport image while isolating. */
	void DrawViewportNotice(EditorContext *context, float centerX, float top);
}

// -------------------------------------------------------
