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
 * editor_visibility.cpp
 * -------------------------------------------------------
 * Created: 27/11/2026
 * -------------------------------------------------------
 */
#include "editor_visibility.h"
#include "editor_airports.h"
#include "editor_commands.h"
#include "editor_context.h"
#include "editor_history.h"
#include "editor_project.h"
#include "editor_scene.h"
#include "editor_theme.h"
#include "editor_ui.h"
#include "tools_selftest.h"
#include "viewport_menus.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <Editor/settings/editor_preferences.h>
#include <SceneryEditorX/logging/logging.hpp>
#include <SceneryEditorX/renderer/font/font_awesome.h>
#include <SceneryEditorX/xplane/xp_airport_scene.h>
#include <SceneryEditorX/xplane/xp_terrain_scene.h>

// -------------------------------------------------------

namespace SceneryEditorX::EditorVisibility
{
	using EditorSelection::TargetType;
	using XPlane::AirportElementComponent;
	using XPlane::SceneAirports;

	namespace
	{
		constexpr const char *LOG_TAG = "Visibility";
		constexpr uint32_t MAX_DEPTH = 64;	// Guards the parent walks against relationship cycles

		/* @brief What a target names (what Target::SameAs compares) as a hash key. */
		std::string TargetId(const Target &target)
		{
			switch (target.type)
			{
				case TargetType::Entity:			return "e" + std::to_string(target.entity);
				case TargetType::AirportElement:	return "r" + std::to_string(static_cast<int>(target.kind)) + ":" + std::to_string(target.index);
				case TargetType::ProjectItem:		return "i" + target.itemId;
			}
			return {};
		}

		/**
		 * @brief Targets with an index by what they name: adding, removing and finding one are O(1), so hiding, showing or
		 * undoing thousands of rows stays linear. The order isn't kept (a removal moves the last one into the gap).
		 */
		class TargetSet
		{
		public:
			[[nodiscard]] bool Contains(const Target &target) const { return m_Index.contains(TargetId(target)); }

			/* @brief False when it was there already. */
			bool Add(const Target &target)
			{
				const auto [it, added] = m_Index.try_emplace(TargetId(target), m_Targets.size());
				if (added)
					m_Targets.push_back(target);
				return added;
			}

			bool Remove(const Target &target)
			{
				const auto it = m_Index.find(TargetId(target));
				if (it == m_Index.end())
					return false;
				const size_t at = it->second;
				m_Index.erase(it);
				if (at + 1 != m_Targets.size())
				{
					m_Targets[at] = std::move(m_Targets.back());
					m_Index[TargetId(m_Targets[at])] = at;
				}
				m_Targets.pop_back();
				return true;
			}

			void Set(const Target &target, const bool present)
			{
				if (present)
					Add(target);
				else
					Remove(target);
			}

			/* @brief Removes the targets `remove` returns true for (called once for each); the index is rebuilt only then. */
			template<typename Predicate>
			void EraseIf(Predicate remove)
			{
				if (std::erase_if(m_Targets, remove) == 0)
					return;
				m_Index.clear();
				for (size_t i = 0; i < m_Targets.size(); ++i)
					m_Index[TargetId(m_Targets[i])] = i;
			}

			void clear()
			{
				m_Targets.clear();
				m_Index.clear();
			}

			[[nodiscard]] bool empty() const { return m_Targets.empty(); }
			[[nodiscard]] size_t size() const { return m_Targets.size(); }
			[[nodiscard]] std::vector<Target>::const_iterator begin() const { return m_Targets.begin(); }
			[[nodiscard]] std::vector<Target>::const_iterator end() const { return m_Targets.end(); }
			[[nodiscard]] const std::vector<Target> &List() const { return m_Targets; }

		private:
			std::vector<Target> m_Targets;
			std::unordered_map<std::string, size_t> m_Index;	// TargetId -> position in m_Targets
		};

		struct State
		{
			TargetSet hidden;							// Own hide toggles (normalized)
			TargetSet locked;							// Own lock toggles of what isn't a project item (items: SceneryItem::locked)
			bool isolating = false;
			std::vector<Target> isolated;				// What the isolation shows

			// Applied to the scene (Apply)
			std::unordered_set<uint64_t> flagged;		// Entities Entity::SetHidden(true) was called on
			std::unordered_set<uint64_t> lockedEntities;	// Entities of locked targets (their children are locked through them)
			std::unordered_set<std::string> lockedItems;	// Project items locked themselves or below a locked folder
			bool dirty = true;
			uint64_t appliedStructure = 0;
			uint64_t appliedHistory = ~0ull;
			std::string appliedAirport;				// The open airport's id: apt.dat rows are kept by kind and index in it

			// Saved with the project (Save / Load): keys of what isn't in the scene now (another airport's rows, entities
			// not loaded yet or gone), kept so a save writes them back, and the keys of entity targets, kept for when their
			// entity goes. syncedView is what the open project held when it was last loaded or saved here.
			std::vector<std::string> pendingHidden;
			std::vector<std::string> pendingLocked;
			std::unordered_map<uint64_t, std::string> entityKeys;
			bool synced = false;
			ProjectFile::ProjectViewState syncedView;
			std::string syncedProject;				// The open project's identity (its uuid, else its path) when synced
			uint64_t generation = 1;				// Bumped by Load: undo steps made before refuse (another project's state)
			uint64_t resolvedStructure = 0;		// Scene structure the waiting keys were last looked for in
			double nextResolve = 0.0;				// Not again before (ImGui time): a lookup walks the scene's roots

			uint64_t revision = 1;
			NavigationKeymap commandsKeymap = NavigationKeymap::Count;

			// BeginToggleGroup / EndToggleGroup: the steps made in between, pushed as one
			enum class Verb : uint8_t { Hide, Show, Lock, Unlock };
			struct Step
			{
				std::string label;
				Verb verb = Verb::Hide;
				size_t count = 0;
				std::function<bool()> undo;
				std::function<bool()> redo;
				size_t bytes = 0;
			};
			int groupDepth = 0;
			std::vector<Step> group;
		};

		State &S()
		{
			static State state;
			return state;
		}

		void Changed(State &s)
		{
			s.dirty = true;
			++s.revision;
			// Keys of entities no longer hidden or locked go (the cache would grow with every entity ever toggled)
			std::erase_if(s.entityKeys, [&s](const auto &entry)
			{
				const Target target = Target::ForEntity(entry.first);
				return !s.hidden.Contains(target) && !s.locked.Contains(target);
			});
		}

		bool Contains(const TargetSet &targets, const Target &target)
		{
			return targets.Contains(target);
		}

		void Set(TargetSet &targets, const Target &target, const bool present)
		{
			targets.Set(target, present);
		}

		entt::entity ParentOf(const entt::registry &registry, const entt::entity handle)
		{
			const RelationshipComponent *relationship = registry.try_get<RelationshipComponent>(handle);
			if (!relationship || static_cast<uint64_t>(relationship->pParentHandle) == 0)
				return entt::null;
			return Scene::TryGetEntityHandle(relationship->pParentHandle);
		}

		/* @brief Scene lights and cameras: never hidden by isolation or Hide Unselected (the sun would go out). */
		bool IsSceneFixture(const entt::entity handle)
		{
			return EditorScene::Has<CameraComponent>(handle) || EditorScene::Has<DirectionalLightComponent>(handle) ||
				   EditorScene::Has<PointLightComponent>(handle) || EditorScene::Has<SpotLightComponent>(handle);
		}

		ProjectFile::SceneryItem *FindItem(const std::string &id)
		{
			ProjectFile::ProjectDocument *document = EditorProjects::GetDocument();
			return document ? document->FindItem(id) : nullptr;
		}

		// Keys ---------------------------------------------------

		constexpr std::string_view KEY_ITEM = "item/";
		constexpr std::string_view KEY_ROW = "row/";
		constexpr std::string_view KEY_ENTITY = "entity/";

		/* @brief A name as one part of a key: '%', '/' and '#' written %25, %2F, %23. */
		std::string EscapeKeyPart(const std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (const char c : text)
			{
				if (c == '%')
					out += "%25";
				else if (c == '/')
					out += "%2F";
				else if (c == '#')
					out += "%23";
				else
					out += c;
			}
			return out;
		}

		std::string UnescapeKeyPart(const std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (size_t i = 0; i < text.size(); ++i)
			{
				if (text[i] == '%' && i + 2 < text.size())
				{
					const std::string_view code = text.substr(i + 1, 2);
					if (code == "25" || code == "2F" || code == "2f" || code == "23")
					{
						out += code == "25" ? '%' : code == "23" ? '#' : '/';
						i += 2;
						continue;
					}
				}
				out += text[i];
			}
			return out;
		}

		std::vector<std::string_view> SplitKey(const std::string_view key)
		{
			std::vector<std::string_view> parts;
			size_t start = 0;
			while (start <= key.size())
			{
				const size_t slash = key.find('/', start);
				const size_t end = slash == std::string_view::npos ? key.size() : slash;
				parts.push_back(key.substr(start, end - start));
				if (slash == std::string_view::npos)
					break;
				start = slash + 1;
			}
			return parts;
		}

		std::optional<XPlane::AirportElementKind> KindFromName(const std::string_view name)
		{
			for (int k = 0; k < static_cast<int>(XPlane::AirportElementKind::Count); ++k)
			{
				const XPlane::AirportElementKind kind = static_cast<XPlane::AirportElementKind>(k);
				if (name == XPlane::GetAirportElementKindName(kind))
					return kind;
			}
			return std::nullopt;
		}

		/* @brief "row/KHVN/Runway/0"; a group's index is the kind of rows it holds, so it is named too ("row/KHVN/Group/Runway"). */
		std::string RowKey(const std::string &airportId, const Target &target)
		{
			const std::string index = target.kind == XPlane::AirportElementKind::Group && target.index < static_cast<uint32_t>(XPlane::AirportElementKind::Count) ?
				XPlane::GetAirportElementKindName(static_cast<XPlane::AirportElementKind>(target.index)) : std::to_string(target.index);
			return std::string(KEY_ROW) + EscapeKeyPart(airportId) + "/" + EscapeKeyPart(XPlane::GetAirportElementKindName(target.kind)) + "/" + EscapeKeyPart(index);
		}

		bool ParseRowKey(const std::string_view key, std::string &airportId, XPlane::AirportElementKind &kind, uint32_t &index)
		{
			const std::vector<std::string_view> parts = SplitKey(key.substr(KEY_ROW.size()));
			if (parts.size() != 3)
				return false;
			airportId = UnescapeKeyPart(parts[0]);
			const std::optional<XPlane::AirportElementKind> parsedKind = KindFromName(UnescapeKeyPart(parts[1]));
			if (!parsedKind)
				return false;
			kind = *parsedKind;
			const std::string indexText = UnescapeKeyPart(parts[2]);
			if (kind == XPlane::AirportElementKind::Group)
			{
				const std::optional<XPlane::AirportElementKind> groupKind = KindFromName(indexText);
				if (!groupKind)
					return false;
				index = static_cast<uint32_t>(*groupKind);
				return true;
			}
			const auto [end, error] = std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
			return error == std::errc() && end == indexText.data() + indexText.size();
		}

		/* @brief The scene's top-level entities, in the scene's order. */
		std::vector<entt::entity> Roots(const entt::registry &registry)
		{
			std::vector<entt::entity> roots;
			for (Entity *entity : Scene::GetEntities())
			{
				if (!entity || !*entity)
					continue;
				const entt::entity handle = static_cast<entt::entity>(*entity);
				if (ParentOf(registry, handle) == entt::null)
					roots.push_back(handle);
			}
			return roots;
		}

		std::vector<entt::entity> Children(const entt::registry &registry, const entt::entity handle)
		{
			std::vector<entt::entity> children;
			if (const RelationshipComponent *relationship = registry.try_get<RelationshipComponent>(handle))
			{
				for (const UUID &child : relationship->pChildren)
				{
					const entt::entity childHandle = Scene::TryGetEntityHandle(child);
					if (childHandle != entt::null && registry.valid(childHandle))
						children.push_back(childHandle);
				}
			}
			return children;
		}

		/**
		 * @brief What working out and looking up entity keys needs from the scene, read at most once per pass (a save, a
		 * lookup of waiting keys) and only when needed: the roots, by name too, and each sibling's place among the siblings
		 * with its name, numbered once per parent. A key level is then a lookup, not a walk of the siblings, so Hide
		 * Unselected over thousands of roots stays linear.
		 */
		struct KeyIndex
		{
			std::optional<std::vector<entt::entity>> roots;
			std::optional<std::unordered_map<std::string, std::vector<entt::entity>>> rootsByName;
			std::unordered_map<entt::entity, uint32_t> nth;			// Place among same-named siblings
			std::unordered_set<entt::entity> numbered;				// Parents whose children are in nth (entt::null: the roots)

			const std::vector<entt::entity> &GetRoots(const entt::registry &registry)
			{
				if (!roots)
					roots = Roots(registry);
				return *roots;
			}

			/* @brief The roots with a name, in the scene's order (nullptr when none). */
			const std::vector<entt::entity> *RootsNamed(const entt::registry &registry, const std::string &name)
			{
				if (!rootsByName)
				{
					rootsByName.emplace();
					for (const entt::entity root : GetRoots(registry))
						(*rootsByName)[EditorScene::GetName(root)].push_back(root);
				}
				const auto it = rootsByName->find(name);
				return it != rootsByName->end() ? &it->second : nullptr;
			}

			/* @brief How many siblings with the entity's name come before it under `parent` (entt::null: among the roots). */
			uint32_t Nth(const entt::registry &registry, const entt::entity handle, const entt::entity parent)
			{
				if (numbered.insert(parent).second)
				{
					std::unordered_map<std::string_view, uint32_t> counts;
					const auto number = [this, &counts](const entt::entity sibling) { nth[sibling] = counts[EditorScene::GetName(sibling)]++; };
					if (parent == entt::null)
					{
						for (const entt::entity sibling : GetRoots(registry))
							number(sibling);
					}
					else
					{
						for (const entt::entity sibling : Children(registry, parent))
							number(sibling);
					}
				}
				const auto it = nth.find(handle);
				return it != nth.end() ? it->second : 0;
			}
		};

		/**
		 * @brief "entity/Terrain scenery KHVN", "entity/Airport KHVN/Objects/traffic_cone_2#3": the names from the top of the
		 * scene down to the entity, "#n" for the nth of siblings with that name (from 0, when n > 0). Empty when it is gone.
		 */
		std::string EntityKey(const uint64_t entityId, KeyIndex &index)
		{
			entt::registry *registry = EditorScene::GetRegistry();
			entt::entity handle = EditorScene::FindHandle(entityId);
			if (!registry || handle == entt::null)
				return {};
			std::vector<std::string> parts;
			for (uint32_t depth = 0; depth < MAX_DEPTH && handle != entt::null; ++depth)
			{
				const std::string_view name = EditorScene::GetName(handle);
				const entt::entity parent = ParentOf(*registry, handle);
				const uint32_t nth = index.Nth(*registry, handle, parent);
				parts.push_back(EscapeKeyPart(name) + (nth > 0 ? "#" + std::to_string(nth) : std::string()));
				handle = parent;
			}
			std::string key(KEY_ENTITY);
			for (auto it = parts.rbegin(); it != parts.rend(); ++it)
				key += (it == parts.rbegin() ? "" : "/") + *it;
			return key;
		}

		/* @brief The entity an EntityKey() names now, 0 when there is none. */
		uint64_t ResolveEntityKey(const std::string_view key, KeyIndex &index)
		{
			entt::registry *registry = EditorScene::GetRegistry();
			if (!registry)
				return 0;
			std::vector<entt::entity> candidates;
			bool top = true;	// The first part names a root: looked up by name
			entt::entity found = entt::null;
			for (const std::string_view part : SplitKey(key.substr(KEY_ENTITY.size())))
			{
				// "name#n" ('#' in a name is escaped, so the last one starts the count)
				const size_t hash = part.rfind('#');
				const std::string name = UnescapeKeyPart(hash == std::string_view::npos ? part : part.substr(0, hash));
				uint32_t nth = 0;
				if (hash != std::string_view::npos)
				{
					const std::string_view count = part.substr(hash + 1);
					if (std::from_chars(count.data(), count.data() + count.size(), nth).ec != std::errc())
						return 0;
				}
				found = entt::null;
				if (top)
				{
					const std::vector<entt::entity> *named = index.RootsNamed(*registry, name);
					if (named && nth < named->size())
						found = (*named)[nth];
					top = false;
				}
				else
				{
					uint32_t seen = 0;
					for (const entt::entity candidate : candidates)
					{
						if (name == EditorScene::GetName(candidate) && seen++ == nth)
						{
							found = candidate;
							break;
						}
					}
				}
				if (found == entt::null)
					return 0;
				candidates = Children(*registry, found);
			}
			return found != entt::null ? EditorScene::GetID(found) : 0;
		}

		/**
		 * @brief The key a target is saved under (empty when it can't be: a row with no airport open). An entity keeps the
		 * key it had when it was hidden or locked (or found from a saved key): the scene entities that aren't project items
		 * or apt.dat rows take their names and order from the data they are loaded from, and a rename in the editor isn't
		 * saved, so after reopening the project that key is the one that matches again.
		 */
		std::string KeyOf(State &s, const Target &target, KeyIndex &index)
		{
			switch (target.type)
			{
				case TargetType::ProjectItem:
					return std::string(KEY_ITEM) + EscapeKeyPart(target.itemId);
				case TargetType::AirportElement:
					return s.appliedAirport.empty() ? std::string() : RowKey(s.appliedAirport, target);
				case TargetType::Entity:
				{
					if (const auto it = s.entityKeys.find(target.entity); it != s.entityKeys.end())
						return it->second;
					std::string key = EntityKey(target.entity, index);
					if (!key.empty())
						s.entityKeys[target.entity] = key;
					return key;
				}
			}
			return {};
		}

		/**
		 * @brief Turns waiting keys into targets where what they name is in the scene now. Keys of a kind this editor
		 * doesn't know (a newer one wrote them) keep waiting, so they are saved back as they were.
		 */
		void ResolvePending(State &s)
		{
			if (s.pendingHidden.empty() && s.pendingLocked.empty())
				return;
			KeyIndex index;
			bool any = false;
			const auto resolve = [&](std::vector<std::string> &pending, TargetSet &targets, const bool items)
			{
				std::erase_if(pending, [&](const std::string &key)
				{
					std::optional<Target> target;
					if (key.starts_with(KEY_ITEM))
					{
						if (!items)
							return true;	// An item's lock is the item's own flag
						target = Target::ForItem(UnescapeKeyPart(std::string_view(key).substr(KEY_ITEM.size())));
					}
					else if (key.starts_with(KEY_ROW))
					{
						std::string airportId;
						XPlane::AirportElementKind kind = XPlane::AirportElementKind::Airport;
						uint32_t row = 0;
						if (!ParseRowKey(key, airportId, kind, row))
							return true;	// Damaged: dropped
						if (!airportId.empty() && airportId == s.appliedAirport)
							target = Target::ForElement(kind, row);
					}
					else if (key.starts_with(KEY_ENTITY) && EditorScene::GetRegistry())
					{
						if (const uint64_t entity = ResolveEntityKey(key, index))
						{
							target = Target::ForEntity(entity);
							s.entityKeys[entity] = key;
						}
					}
					if (!target)
						return false;
					Set(targets, *target, true);
					any = true;
					return true;
				});
			};
			resolve(s.pendingHidden, s.hidden, true);
			resolve(s.pendingLocked, s.locked, false);
			if (any)
				Changed(s);
		}

		/**
		 * @brief Writes the hide and lock state into the open project (its View section). Marks the project edited when it
		 * changed, unless `markEdited` is false (a project just opened taking the view as it is: not a change of the user's).
		 */
		void Save(State &s, const bool markEdited = true)
		{
			ProjectFile::ProjectDocument *document = EditorProjects::GetDocument();
			if (!document)
				return;
			KeyIndex index;
			ProjectFile::ProjectViewState view;
			for (const Target &target : s.hidden)
			{
				if (target.type == TargetType::ProjectItem && !document->FindItem(target.itemId))
					continue;	// Deleted since
				if (std::string key = KeyOf(s, target, index); !key.empty())
					view.hidden.push_back(std::move(key));
			}
			view.hidden.insert(view.hidden.end(), s.pendingHidden.begin(), s.pendingHidden.end());
			for (const Target &target : s.locked)
			{
				if (std::string key = KeyOf(s, target, index); !key.empty())
					view.locked.push_back(std::move(key));
			}
			view.locked.insert(view.locked.end(), s.pendingLocked.begin(), s.pendingLocked.end());
			for (std::vector<std::string> *keys : { &view.hidden, &view.locked })
			{
				std::ranges::sort(*keys);	// Same state, same file (git diffs)
				keys->erase(std::unique(keys->begin(), keys->end()), keys->end());
			}
			s.synced = true;
			s.syncedView = view;
			if (view == document->GetProject().view)
				return;
			document->GetProject().view = std::move(view);
			if (markEdited)
				EditorProjects::MarkEdited(ProjectFile::ProjectDocument::Section::Info);
		}

		/* @brief Hidden, locked or isolated things, or saved keys waiting for theirs. */
		bool HasState(const State &s)
		{
			return !s.hidden.empty() || !s.locked.empty() || !s.pendingHidden.empty() || !s.pendingLocked.empty() || s.isolating;
		}

		/**
		 * @brief Takes the hide and lock state the open project was saved with (or none); isolation ends, and the undo steps
		 * made before refuse (they name another project's state).
		 */
		void Load(State &s, const ProjectFile::ProjectViewState &view, const bool synced, std::string project)
		{
			s.hidden.clear();
			s.locked.clear();
			s.isolating = false;
			s.isolated.clear();
			s.entityKeys.clear();
			s.pendingHidden = view.hidden;
			s.pendingLocked = view.locked;
			s.synced = synced;
			s.syncedView = view;
			s.syncedProject = std::move(project);
			++s.generation;
			ResolvePending(s);
			Changed(s);
		}

		std::vector<Target> SelectedTargets()
		{
			TargetSet targets;
			for (const Target &target : EditorSelection::Get())
				targets.Add(Normalize(target));
			return targets.List();	// Nothing removed: still in the selection's order
		}

		/**
		 * @brief The entities to hide so that only `keep` stays drawn (each kept entity with everything below it), plus the
		 * base terrain, the scene's lights and cameras: the roots off the kept paths, and the siblings along them.
		 */
		void CollectOthers(const std::vector<Target> &keepTargets, std::unordered_set<uint64_t> &out)
		{
			entt::registry *registry = EditorScene::GetRegistry();
			if (!registry)
				return;

			std::unordered_set<uint64_t> keep;
			std::vector<uint64_t> entities;
			for (const Target &target : keepTargets)
			{
				entities.clear();
				EditorSelection::CollectEntities(target, entities);
				keep.insert(entities.begin(), entities.end());
			}
			if (XPlane::SceneTerrain::IsLoaded())
				keep.insert(XPlane::SceneTerrain::GetRootId());

			// Every parent of what is kept: walked into, so its other children can be hidden
			std::unordered_set<uint64_t> path;
			for (const uint64_t id : keep)
			{
				entt::entity handle = EditorScene::FindHandle(id);
				for (uint32_t depth = 0; depth < MAX_DEPTH && handle != entt::null; ++depth)
				{
					handle = ParentOf(*registry, handle);
					if (handle == entt::null || !path.insert(EditorScene::GetID(handle)).second)
						break;
				}
			}

			std::vector<entt::entity> stack;
			for (Entity *entity : Scene::GetEntities())
			{
				if (!entity || !*entity)
					continue;
				const entt::entity handle = static_cast<entt::entity>(*entity);
				if (ParentOf(*registry, handle) == entt::null)
					stack.push_back(handle);
			}
			while (!stack.empty())
			{
				const entt::entity handle = stack.back();
				stack.pop_back();
				const uint64_t id = EditorScene::GetID(handle);
				if (id == 0 || keep.contains(id) || IsSceneFixture(handle))
					continue;
				if (!path.contains(id))
				{
					out.insert(id);
					continue;
				}
				if (const RelationshipComponent *relationship = registry->try_get<RelationshipComponent>(handle))
				{
					for (const UUID &child : relationship->pChildren)
					{
						const entt::entity childHandle = Scene::TryGetEntityHandle(child);
						if (childHandle != entt::null && registry->valid(childHandle))
							stack.push_back(childHandle);
					}
				}
			}
		}

		void SetEntityHidden(const uint64_t id, const bool hidden)
		{
			const entt::entity handle = EditorScene::FindHandle(id);
			if (handle == entt::null)
				return;
			if (Entity *entity = EditorScene::FindEntity(handle))
				entity->SetHidden(hidden);
		}

		/**
		 * @brief Entity targets whose entity is gone (UUIDs aren't reused) wait as their saved key instead, found again when
		 * an entity at that path comes back (terrain reloaded, say).
		 */
		void ForgetGone(State &s, TargetSet &targets, std::vector<std::string> &pending)
		{
			targets.EraseIf([&s, &pending](const Target &target)
			{
				if (target.type != TargetType::Entity || EditorScene::FindHandle(target.entity) != entt::null)
					return false;
				if (const auto it = s.entityKeys.find(target.entity); it != s.entityKeys.end())
				{
					pending.push_back(it->second);
					s.entityKeys.erase(it);
				}
				return true;
			});
		}

		/* @brief Brings the scene's hidden flags in line with the hide toggles and the isolation. */
		void ApplyHidden(State &s)
		{
			ForgetGone(s, s.hidden, s.pendingHidden);
			std::unordered_set<uint64_t> desired;
			std::vector<uint64_t> entities;
			for (const Target &target : s.hidden)
			{
				entities.clear();
				EditorSelection::CollectEntities(target, entities);
				desired.insert(entities.begin(), entities.end());
			}
			if (s.isolating)
				CollectOthers(s.isolated, desired);

			for (const uint64_t id : s.flagged)
			{
				if (!desired.contains(id))
					SetEntityHidden(id, false);
			}
			for (const uint64_t id : desired)
			{
				if (!s.flagged.contains(id))
					SetEntityHidden(id, true);
			}
			s.flagged = std::move(desired);
		}

		/* @brief The project items locked themselves or under a locked folder (they change with the project, not the scene). */
		void ApplyLockedItems(State &s)
		{
			s.lockedItems.clear();
			if (const ProjectFile::ProjectDocument *document = EditorProjects::GetDocument())
			{
				int lockedDepth = -1;	// Depth of the locked folder being walked through, -1 when none
				document->GetProject().ForEachItem([&s, &lockedDepth](const ProjectFile::SceneryItem &item, const int depth)
				{
					if (lockedDepth >= 0 && depth <= lockedDepth)
						lockedDepth = -1;
					if (item.locked && lockedDepth < 0)
						lockedDepth = depth;
					if (lockedDepth >= 0)
						s.lockedItems.insert(item.id);
					return true;
				});
			}
		}

		/* @brief The entities of every locked target (what picks are checked against). */
		void ApplyLockedEntities(State &s)
		{
			ForgetGone(s, s.locked, s.pendingLocked);
			s.lockedEntities.clear();
			std::vector<uint64_t> entities;
			for (const Target &target : s.locked)
			{
				entities.clear();
				EditorSelection::CollectEntities(target, entities);
				s.lockedEntities.insert(entities.begin(), entities.end());
			}
			for (const std::string &id : s.lockedItems)
			{
				if (const uint64_t entity = EditorProjects::GetPlacedEntity(id))
					s.lockedEntities.insert(entity);
			}
		}

		/* @brief Pushes an undo step, or keeps it for the open toggle group. */
		void Record(State &s, State::Step step)
		{
			if (s.groupDepth > 0)
			{
				s.group.push_back(std::move(step));
				return;
			}
			EditorHistory::Push({ std::move(step.label), std::move(step.undo), std::move(step.redo), step.bytes });
		}

		/* @brief "3 items" / "Runway 1", for the log (English, like the rest of it). */
		std::string Count(const std::vector<Target> &targets)
		{
			if (targets.size() == 1)
				return EditorSelection::Describe(targets.front());
			return std::to_string(targets.size()) + " items";
		}

		using Verb = State::Verb;

		/* @brief An undo step's label for a number of targets ("Hide 3 items"), translated. */
		std::string Label(const Verb verb, const size_t count)
		{
			const std::int64_t n = static_cast<std::int64_t>(count);
			switch (verb)
			{
				// TRANSLATORS: undo steps (Edit > Undo ...); {} is how many things were hidden, shown, locked or unlocked
				case Verb::Hide:	return TrNF("Hide {} item", "Hide {} items", n);
				case Verb::Show:	return TrNF("Show {} item", "Show {} items", n);
				case Verb::Lock:	return TrNF("Lock {} item", "Lock {} items", n);
				case Verb::Unlock:	return TrNF("Unlock {} item", "Unlock {} items", n);
			}
			return {};
		}

		/* @brief An undo step's label for these targets: the one's name ("Hide Runway 1"), else how many there are. */
		std::string Label(const Verb verb, const std::vector<Target> &targets)
		{
			if (targets.size() != 1)
				return Label(verb, targets.size());
			const std::string name = EditorSelection::Describe(targets.front());
			switch (verb)
			{
				// TRANSLATORS: undo steps (Edit > Undo ...); {} is the name of what was hidden, shown, locked or unlocked
				case Verb::Hide:	return TrF("Hide {}", name);
				case Verb::Show:	return TrF("Show {}", name);
				case Verb::Lock:	return TrF("Lock {}", name);
				case Verb::Unlock:	return TrF("Unlock {}", name);
			}
			return {};
		}

		/* @brief The keys shown beside a command for the chosen keymap (the keys themselves are HandleShortcuts'). */
		struct Keys
		{
			const char *hideSelected = "H";
			const char *hideUnselected = "Shift+H";
			const char *showAll = "Alt+H";
			const char *isolate = "Numpad / or Alt+Q";
		};

		Keys KeysFor(const NavigationKeymap keymap)
		{
			Keys keys;
			if (keymap == NavigationKeymap::Maya)
			{
				keys.hideSelected = "Ctrl+H";
				keys.hideUnselected = "Alt+H";
				keys.showAll = "Ctrl+Shift+H";
				keys.isolate = "Shift+I or Alt+Q";
			}
			else if (keymap == NavigationKeymap::Max)
			{
				keys.isolate = "Alt+Q";
			}
			return keys;
		}
	}

	// -------------------------------------------------------

	Target ForEntity(const uint64_t entityId)
	{
		// Every airport entity by its row (the airport itself and its groups too): rebuilds replace the entities, not the rows
		if (const AirportElementComponent *element = SceneAirports::FindElement(entityId))
			return Target::ForElement(element->kind, element->index, entityId);
		if (std::optional<Target> target = EditorSelection::FromEntity(entityId))
			return *target;
		return Target::ForEntity(entityId);
	}

	Target Normalize(const Target &target)
	{
		if (target.type == TargetType::Entity)
			return ForEntity(target.entity);
		return target;
	}

	// Hide -------------------------------------------------

	bool IsHidden(const Target &target)
	{
		const State &s = S();
		return !s.hidden.empty() && Contains(s.hidden, Normalize(target));
	}

	bool IsHiddenByParent(const uint64_t entityId)
	{
		const entt::entity handle = EditorScene::FindHandle(entityId);
		const Entity *entity = handle != entt::null ? EditorScene::FindEntity(handle) : nullptr;
		if (!entity || !entity->IsHiddenInTree())
			return false;
		return !IsHidden(ForEntity(entityId));
	}

	void SetHidden(EditorContext *context, const std::vector<Target> &targets, const bool hidden)
	{
		State &s = S();
		TargetSet changing;
		for (const Target &target : targets)
		{
			const Target normalized = Normalize(target);
			if (IsHidden(normalized) != hidden)
				changing.Add(normalized);
		}
		if (changing.empty())
			return;
		const std::vector<Target> changed = changing.List();

		KeyIndex index;
		for (const Target &target : changed)
		{
			Set(s.hidden, target, hidden);
			if (hidden)
				KeyOf(s, target, index);	// An entity's key while it is there to name (no project open: no save does it)
		}
		Changed(s);
		Save(s);

		// What is hidden can't stay selected (it couldn't be seen or picked)
		if (hidden)
		{
			std::vector<Target> deselect;
			for (const Target &selected : EditorSelection::Get())
			{
				if (changing.Contains(Normalize(selected)))
					deselect.push_back(selected);
			}
			if (!deselect.empty())
				EditorSelection::Select(context, deselect, EditorSelection::Mode::Remove);
		}

		const Verb verb = hidden ? Verb::Hide : Verb::Show;
		EDITOR_INFO_TAG(LOG_TAG, "{} {}", hidden ? "Hide" : "Show", Count(changed));
		const auto apply = [changed, generation = s.generation](const bool hide)
		{
			// Not after another project opened, or this one was reverted: its targets are another state's
			State &state = S();
			if (state.generation != generation)
				return false;
			for (const Target &target : changed)
				Set(state.hidden, target, hide);
			Changed(state);
			Save(state);
			return true;
		};
		Record(s, { Label(verb, changed), verb, changed.size(), [apply, hidden]() { return apply(!hidden); },
			[apply, hidden]() { return apply(hidden); }, changed.size() * sizeof(Target) });
	}

	void HideSelected(EditorContext *context)
	{
		SetHidden(context, SelectedTargets(), true);
	}

	void HideUnselected(EditorContext *context)
	{
		const std::vector<Target> selected = SelectedTargets();
		if (selected.empty())
			return;
		std::unordered_set<uint64_t> others;
		CollectOthers(selected, others);
		TargetSet targets;
		for (const uint64_t id : others)
			targets.Add(ForEntity(id));
		SetHidden(context, targets.List(), true);
	}

	void ShowAll(EditorContext *context)
	{
		const std::vector<Target> hidden = S().hidden.List();
		SetHidden(context, hidden, false);
	}

	bool AnyHidden()
	{
		return !S().hidden.empty();
	}

	// Isolate ----------------------------------------------

	bool IsIsolating()
	{
		return S().isolating;
	}

	size_t GetIsolatedCount()
	{
		return S().isolated.size();
	}

	void Isolate(EditorContext *)
	{
		std::vector<Target> targets = SelectedTargets();
		if (targets.empty())
			return;
		State &s = S();
		EDITOR_INFO_TAG(LOG_TAG, "Isolate {}", Count(targets));
		s.isolating = true;
		s.isolated = std::move(targets);
		Changed(s);
	}

	void ExitIsolation()
	{
		State &s = S();
		if (!s.isolating)
			return;
		EDITOR_INFO_TAG(LOG_TAG, "Exit isolation");
		s.isolating = false;
		s.isolated.clear();
		Changed(s);
	}

	void ToggleIsolation(EditorContext *context)
	{
		if (IsIsolating())
			ExitIsolation();
		else
			Isolate(context);
	}

	// Lock -------------------------------------------------

	bool IsLockedSelf(const Target &target)
	{
		const Target normalized = Normalize(target);
		if (normalized.type == TargetType::ProjectItem)
		{
			const ProjectFile::SceneryItem *item = FindItem(normalized.itemId);
			return item && item->locked;
		}
		const State &s = S();
		return !s.locked.empty() && Contains(s.locked, normalized);
	}

	bool IsEntityLocked(const uint64_t entityId)
	{
		const State &s = S();
		entt::registry *registry = EditorScene::GetRegistry();
		if (s.lockedEntities.empty() || !registry)
			return false;
		entt::entity handle = EditorScene::FindHandle(entityId);
		for (uint32_t depth = 0; depth < MAX_DEPTH && handle != entt::null; ++depth)
		{
			if (s.lockedEntities.contains(EditorScene::GetID(handle)))
				return true;
			handle = ParentOf(*registry, handle);
		}
		return false;
	}

	bool IsLockedByParent(const uint64_t entityId)
	{
		entt::registry *registry = EditorScene::GetRegistry();
		const entt::entity handle = EditorScene::FindHandle(entityId);
		if (!registry || handle == entt::null)
			return false;
		const Target target = ForEntity(entityId);
		if (IsLockedSelf(target))
			return false;
		if (target.type == TargetType::ProjectItem && S().lockedItems.contains(target.itemId))
			return true;
		const entt::entity parent = ParentOf(*registry, handle);
		return parent != entt::null && IsEntityLocked(EditorScene::GetID(parent));
	}

	bool IsLocked(const Target &target)
	{
		const State &s = S();
		const Target normalized = Normalize(target);
		if (IsLockedSelf(normalized))
			return true;
		if (normalized.type == TargetType::ProjectItem && s.lockedItems.contains(normalized.itemId))
			return true;
		if (s.lockedEntities.empty())
			return false;
		const uint64_t entity = EditorSelection::Resolve(normalized);
		return entity != 0 && IsEntityLocked(entity);
	}

	void SetLocked(EditorContext *, const std::vector<Target> &targets, const bool locked)
	{
		State &s = S();
		KeyIndex index;
		std::vector<Target> changed;
		TargetSet seen;
		bool items = false;
		for (const Target &target : targets)
		{
			const Target normalized = Normalize(target);
			if (IsLockedSelf(normalized) == locked || !seen.Add(normalized))
				continue;
			if (normalized.type == TargetType::ProjectItem)
			{
				ProjectFile::SceneryItem *item = FindItem(normalized.itemId);
				if (!item)
					continue;
				item->locked = locked;
				items = true;
			}
			else
			{
				Set(s.locked, normalized, locked);
				if (locked)
					KeyOf(s, normalized, index);	// An entity's key while it is there to name
			}
			changed.push_back(normalized);
		}
		if (changed.empty())
			return;
		if (items)
			EditorProjects::MarkEdited(ProjectFile::ProjectDocument::Section::Items);
		Changed(s);
		Save(s);

		const Verb verb = locked ? Verb::Lock : Verb::Unlock;
		EDITOR_INFO_TAG(LOG_TAG, "{} {}", locked ? "Lock" : "Unlock", Count(changed));
		const auto apply = [changed, generation = s.generation](const bool lock)
		{
			// Only in the project (and state) it was made in: the document object is reused, so another project opened
			// or this one reverted shows as a new generation (Load), not as another document
			State &state = S();
			if (state.generation != generation)
				return false;
			bool itemsChanged = false;
			for (const Target &target : changed)
			{
				if (target.type != TargetType::ProjectItem)
				{
					Set(state.locked, target, lock);
					continue;
				}
				if (ProjectFile::SceneryItem *item = FindItem(target.itemId); item && item->locked != lock)
				{
					item->locked = lock;
					itemsChanged = true;
				}
			}
			if (itemsChanged)
				EditorProjects::MarkEdited(ProjectFile::ProjectDocument::Section::Items);
			Changed(state);
			Save(state);
			return true;
		};
		Record(s, { Label(verb, changed), verb, changed.size(), [apply, locked]() { return apply(!locked); },
			[apply, locked]() { return apply(locked); }, changed.size() * sizeof(Target) });
	}

	void LockSelected(EditorContext *context)
	{
		SetLocked(context, SelectedTargets(), true);
	}

	void UnlockAll(EditorContext *context)
	{
		std::vector<Target> targets = S().locked.List();
		if (const ProjectFile::ProjectDocument *document = EditorProjects::GetDocument())
		{
			document->GetProject().ForEachItem([&targets](const ProjectFile::SceneryItem &item, int)
			{
				if (item.locked)
					targets.push_back(Target::ForItem(item.id));
				return true;
			});
		}
		SetLocked(context, targets, false);
	}

	bool AnyLocked()
	{
		const State &s = S();
		return !s.locked.empty() || !s.lockedItems.empty();
	}

	void BeginToggleGroup()
	{
		++S().groupDepth;
	}

	void EndToggleGroup()
	{
		State &s = S();
		if (s.groupDepth == 0 || --s.groupDepth > 0)
			return;
		std::vector<State::Step> steps = std::move(s.group);
		s.group.clear();
		if (steps.empty())
			return;
		if (steps.size() == 1)
		{
			Record(s, std::move(steps.front()));
			return;
		}

		// "Hide 5 items" when every step did the same, else one step that changes several
		size_t count = 0, bytes = 0;
		bool sameVerb = true;
		for (const State::Step &step : steps)
		{
			count += step.count;
			bytes += step.bytes;
			sameVerb &= step.verb == steps.front().verb;
		}
		// TRANSLATORS: an undo step that hid some rows and showed others, or locked some and unlocked others; {} is how many
		const std::string label = sameVerb ? Label(steps.front().verb, count) :
			TrNF("Change visibility of {} item", "Change visibility of {} items", static_cast<std::int64_t>(count));
		auto shared = std::make_shared<std::vector<State::Step>>(std::move(steps));
		EditorHistory::Push({ label,
			[shared]()
			{
				bool ok = true;
				for (auto it = shared->rbegin(); it != shared->rend(); ++it)
					ok &= it->undo();
				return ok;
			},
			[shared]()
			{
				bool ok = true;
				for (State::Step &step : *shared)
					ok &= step.redo();
				return ok;
			},
			bytes });
	}

	// Frame --------------------------------------------------

	void Tick(EditorContext *)
	{
		State &s = S();

		// The open project's saved state, when it isn't what was loaded or saved here last: another project opened (the
		// document object is reused: told apart by the project's identity), the project reverted. Closing it takes its state
		// away too. A project opened while none was, saved with nothing hidden or locked (a new one, say), takes what the
		// view shows then instead of losing it; it is written into the project without marking it edited.
		if (const ProjectFile::ProjectDocument *document = EditorProjects::GetDocument())
		{
			const ProjectFile::SceneryProject &project = document->GetProject();
			std::string identity = project.info.uuid;
			if (identity.empty())
			{
				const std::u8string path = document->GetPath().generic_u8string();
				identity.assign(reinterpret_cast<const char *>(path.data()), path.size());
			}
			if (!s.synced && project.view.IsEmpty() && HasState(s))
			{
				s.syncedProject = std::move(identity);
				Save(s, false);
			}
			else if (!s.synced || identity != s.syncedProject || project.view != s.syncedView)
			{
				Load(s, project.view, true, std::move(identity));
			}
		}
		else if (s.synced)
		{
			Load(s, {}, false, {});
		}

		// Rows are kept by kind and index in their airport: another airport's "Runway 1" is another runway. The rows of
		// the airport closed wait as saved keys; the new airport's saved ones apply.
		const XPlane::AirportDocument *airport = EditorAirports::GetDocument();
		const std::string airportId = airport ? airport->airport.id : std::string();
		if (airportId != s.appliedAirport)
		{
			KeyIndex index;
			const auto park = [&s, &index](TargetSet &targets, std::vector<std::string> &pending)
			{
				targets.EraseIf([&s, &index, &pending](const Target &target)
				{
					if (target.type != TargetType::AirportElement)
						return false;
					if (std::string key = KeyOf(s, target, index); !key.empty())
						pending.push_back(std::move(key));
					return true;
				});
			};
			park(s.hidden, s.pendingHidden);
			park(s.locked, s.pendingLocked);
			std::erase_if(s.isolated, [](const Target &target) { return target.type == TargetType::AirportElement; });
			s.appliedAirport = airportId;
			ResolvePending(s);
			Changed(s);
		}

		// Saved keys of entities that weren't there yet: looked for again as the scene fills in (a few times a second at
		// most while it loads; the last change is still looked at once it settles)
		const bool waiting = !s.pendingHidden.empty() || !s.pendingLocked.empty();
		if (waiting && Scene::GetStructureVersion() != s.resolvedStructure && ImGui::GetTime() >= s.nextResolve)
		{
			s.resolvedStructure = Scene::GetStructureVersion();
			s.nextResolve = ImGui::GetTime() + 0.25;
			ResolvePending(s);
		}
		// The state changed (or an undo / redo may have changed an item's lock): everything again. The scene's structure
		// changed (entities made or replaced, every frame while an airport loads): only what names entities, when there is any.
		const uint64_t structure = Scene::GetStructureVersion();
		const uint64_t history = EditorHistory::GetRevision();
		const bool stateChanged = s.dirty || history != s.appliedHistory;
		const bool structureChanged = structure != s.appliedStructure;
		if (!stateChanged && !structureChanged)
			return;
		if (stateChanged)
			ApplyLockedItems(s);
		if (stateChanged || !s.hidden.empty() || s.isolating || !s.flagged.empty())
			ApplyHidden(s);
		if (stateChanged || !s.locked.empty() || !s.lockedItems.empty())
			ApplyLockedEntities(s);
		s.dirty = false;
		s.appliedStructure = Scene::GetStructureVersion();
		s.appliedHistory = history;
	}

	void Reset()
	{
		State &s = S();
		Load(s, {}, false, {});
	}

	uint64_t GetRevision()
	{
		return S().revision;
	}

	void RegisterCommands()
	{
		State &s = S();
		const NavigationKeymap keymap = EditorPreferences::Get().keymap;
		s.commandsKeymap = keymap;
		const Keys keys = KeysFor(keymap);

		const auto hasSelection = [](EditorContext *) { return !EditorSelection::IsEmpty(); };
		EditorCommands::Register({ "view.hide_selected", "Hide Selected", SEDX_ICON_EYE_SLASH, keys.hideSelected,
			"Hide the selection in the view (it is still exported)", hasSelection, nullptr,
			[](EditorContext *context) { HideSelected(context); } });
		EditorCommands::Register({ "view.hide_unselected", "Hide Unselected", SEDX_ICON_EYE_SLASH, keys.hideUnselected,
			"Hide everything but the selection and the ground under it", hasSelection, nullptr,
			[](EditorContext *context) { HideUnselected(context); } });
		EditorCommands::Register({ "view.show_all", "Show All", SEDX_ICON_EYE, keys.showAll, "Show everything hidden in the view",
			[](EditorContext *) { return AnyHidden(); }, nullptr,
			[](EditorContext *context) { ShowAll(context); } });
		EditorCommands::Register({ "view.isolate", "Isolate Selection", SEDX_ICON_BULLSEYE, keys.isolate,
			"Show only the selection and the ground under it, until isolation ends (the same key)",
			[](EditorContext *) { return IsIsolating() || !EditorSelection::IsEmpty(); },
			[](EditorContext *) { return IsIsolating(); },
			[](EditorContext *context) { ToggleIsolation(context); } });
		EditorCommands::Register({ "item.lock", "Lock / Unlock", SEDX_ICON_LOCK, "",
			"Locked things can't be picked in the viewport, moved or edited; select them in the OUTLINER to unlock them", hasSelection,
			[](EditorContext *)
			{
				const std::vector<Target> &selected = EditorSelection::Get();
				return !selected.empty() && std::ranges::all_of(selected, [](const Target &target) { return IsLockedSelf(target); });
			},
			[](EditorContext *context)
			{
				const std::vector<Target> &selected = EditorSelection::Get();
				const bool lock = !std::ranges::all_of(selected, [](const Target &target) { return IsLockedSelf(target); });
				SetLocked(context, selected, lock);
			} });
		EditorCommands::Register({ "view.unlock_all", "Unlock All", SEDX_ICON_UNLOCK, "", "Unlock everything locked",
			[](EditorContext *) { return AnyLocked(); }, nullptr,
			[](EditorContext *context) { UnlockAll(context); } });
	}

	void HandleShortcuts(EditorContext *context)
	{
		// The keys are asked for every frame: ImGui routes a shortcut from the frame after it is first asked for
		const NavigationKeymap keymap = EditorPreferences::Get().keymap;
		const bool maya = keymap == NavigationKeymap::Maya;
		const bool hideSelected = ImGui::Shortcut(maya ? ImGuiMod_Ctrl | ImGuiKey_H : ImGuiKey_H, ImGuiInputFlags_RouteGlobal);
		const bool hideUnselected = ImGui::Shortcut(maya ? ImGuiMod_Alt | ImGuiKey_H : ImGuiMod_Shift | ImGuiKey_H, ImGuiInputFlags_RouteGlobal);
		const bool showAll = ImGui::Shortcut(maya ? ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_H : ImGuiMod_Alt | ImGuiKey_H, ImGuiInputFlags_RouteGlobal);
		// Alt+Q in every keymap, for keyboards without a numpad (laptops)
		bool isolate = ImGui::Shortcut(ImGuiKey_KeypadDivide, ImGuiInputFlags_RouteGlobal);
		isolate |= ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal);
		if (maya)
			isolate |= ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_I, ImGuiInputFlags_RouteGlobal);

		// The labels follow the keymap
		if (S().commandsKeymap != keymap)
			RegisterCommands();

		const ImGuiIO &io = ImGui::GetIO();
		if (io.WantTextInput || ToolsSelfTest::IsEnabled() || !context)
			return;
		// Not while a mouse button drags the camera, and not behind a menu or modal (global routes don't stop at them)
		if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ViewportMenus::IsOpen() ||
			ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			return;

		if (hideSelected)
			EditorCommands::Run("view.hide_selected", context);
		if (hideUnselected)
			EditorCommands::Run("view.hide_unselected", context);
		if (showAll)
			EditorCommands::Run("view.show_all", context);
		if (isolate)
			EditorCommands::Run("view.isolate", context);
	}

	void DrawMenuItems(EditorContext *context)
	{
		EditorCommands::MenuItem("view.hide_selected", context);
		EditorCommands::MenuItem("view.hide_unselected", context);
		EditorCommands::MenuItem("view.show_all", context);
		EditorCommands::MenuItem("view.isolate", context);
		ImGui::Separator();
		EditorCommands::MenuItem("item.lock", context);
		EditorCommands::MenuItem("view.unlock_all", context);
	}

	void DrawViewportNotice(EditorContext *context, const float centerX, const float top)
	{
		if (!IsIsolating())
			return;
		const float dpi = ImGui::GetStyle().FontScaleDpi;
		const std::string text = TrNF("Isolated: {} item", "Isolated: {} items", static_cast<std::int64_t>(GetIsolatedCount()));
		const std::string label = EditorUI::IconLabel(SEDX_ICON_BULLSEYE, text);

		// Centered: the label, the button and the paddings between
		const ImGuiStyle &style = ImGui::GetStyle();
		const float width = ImGui::CalcTextSize(label.c_str()).x + ImGui::CalcTextSize(Tr("Exit")).x + style.FramePadding.x * 2.0f +
							style.ItemSpacing.x + 16.0f * dpi;
		ImGui::SetCursorScreenPos(ImVec2(std::floor(centerX - width * 0.5f), top));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Palette::Separator);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpi, 4.0f * dpi));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 2.0f * dpi);
		const ImGuiChildFlags flags = ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding;
		if (ImGui::BeginChild("##IsolationNotice", ImVec2(0.0f, 0.0f), flags, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(TrL("Exit##isolation")))
				ExitIsolation();
			const EditorCommands::Command *command = EditorCommands::Find("view.isolate");
			const std::string tip = TrF("Show everything again ({})", command ? command->shortcut : std::string());
			EditorUI::ItemTooltip(tip);
		}
		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
		(void)context;
	}
}

// -------------------------------------------------------
