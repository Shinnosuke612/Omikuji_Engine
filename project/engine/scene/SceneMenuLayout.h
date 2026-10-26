// 役割: Menuの静的配置を読み取り、Editorの編集境界で保存済み選択順を同期する。
#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "SceneDocument.h"
#include "SceneEntityQuery.h"
#include "SceneUILayout.h"

namespace SceneMenuLayout {
	struct ItemPosition {
		uint64_t entityId = 0;
		Vector2 point{};
	};

	// TextMotionや選択時の見た目を含めず、保存されたUI枠の基点だけを比較する。
	// 一つでも不正なら全体を失敗させる。Document/順序/初期選択は変更しない。
	inline bool EvaluatePositions(const SceneDocument& document, const SceneEntity& owner,
		const SceneComponent& panel, std::vector<ItemPosition>& positions, std::string& error) {
		positions.clear();
		error.clear();
		auto fail = [&](const std::string& reason) { positions.clear(); error = reason; return false; };
		if (panel.menuNavigationAxis != "Vertical" && panel.menuNavigationAxis != "Horizontal")
			return fail("Menu navigation axis must be Vertical or Horizontal.");
		SceneUILayout::Layout panelLayout;
		if (!SceneUILayout::Resolve(document, owner.id, panelLayout, &error)) return fail("MenuPanel: " + error);
		const SceneEntity* ancestor = document.FindEntity(owner.parentId);
		std::unordered_set<uint64_t> panelAncestors;
		while (ancestor && panelAncestors.insert(ancestor->id).second) {
			if (SceneEntityQuery::FindComponent(*ancestor, "MenuPanel"))
				return fail("MenuPanel roots must not be nested.");
			ancestor = document.FindEntity(ancestor->parentId);
		}
		const SceneEntity* canvasEntity = document.FindEntity(panelLayout.canvasEntityId);
		const SceneComponent* canvas = canvasEntity ? SceneEntityQuery::FindComponent(*canvasEntity, "UICanvas") : nullptr;
		if (!canvas || !canvas->enabled || panelLayout.renderSpace != "ScreenOverlay")
			return fail("Menu requires an enabled ScreenOverlay UICanvas.");
		std::unordered_set<uint64_t> seen;
		for (uint64_t id : panel.menuItemEntityIds) {
			const SceneEntity* entity = document.FindEntity(id);
			const SceneComponent* item = entity ? SceneEntityQuery::FindComponent(*entity, "MenuItem") : nullptr;
			if (!item || !seen.insert(id).second || id == owner.id || !document.IsDescendantOf(id, owner.id))
				return fail("Menu item is missing, duplicated, or outside its panel: " + std::to_string(id));
			// 入れ子Panelに属するItemを外側Panelの順序へ取り込まない。
			const SceneEntity* parent = document.FindEntity(entity->parentId);
			std::unordered_set<uint64_t> ancestors;
			while (parent && parent->id != owner.id && ancestors.insert(parent->id).second) {
				if (SceneEntityQuery::FindComponent(*parent, "MenuPanel"))
					return fail("Menu items must not cross a nested MenuPanel.");
				parent = document.FindEntity(parent->parentId);
			}
			SceneUILayout::Layout itemLayout;
			if (!SceneUILayout::Resolve(document, id, itemLayout, &error)) return fail("MenuItem: " + error);
			if (itemLayout.canvasEntityId != panelLayout.canvasEntityId)
				return fail("All menu items must use their panel's UICanvas.");
			uint64_t pointId = id;
			if (panel.menuLayoutMode != "CanvasItems" && item->menuLabelEntityId) {
				const SceneEntity* label = document.FindEntity(item->menuLabelEntityId);
				if (!label || !SceneEntityQuery::FindComponent(*label, "TextRenderer") ||
					(label->id != id && !document.IsDescendantOf(label->id, id)))
					return fail("Menu text must be a TextRenderer on the item or its descendant.");
				pointId = label->id;
			}
			SceneUILayout::Layout pointLayout;
			if (!SceneUILayout::Resolve(document, pointId, pointLayout, &error)) return fail("Menu text: " + error);
			if (pointLayout.canvasEntityId != panelLayout.canvasEntityId)
				return fail("Menu text must use its panel's UICanvas.");
			positions.push_back({ id, pointLayout.pivotInCanvas });
		}
		return true;
	}

	inline float Coordinate(const ItemPosition& position, const SceneComponent& panel) {
		return panel.menuNavigationAxis == "Horizontal" ? position.point.x : position.point.y;
	}

	namespace Detail {
		// 依存する祖先の保存値だけを記録する。文言/Marker/viewport変更では再整列しない。
		inline void AppendAncestors(const SceneDocument& document, uint64_t id, std::ostream& out) {
			std::unordered_set<uint64_t> visited;
			while (id && visited.insert(id).second) {
				out << "|entity:" << id;
				const SceneEntity* entity = document.FindEntity(id);
				if (!entity) { out << ":missing"; break; }
				out << ':' << entity->parentId << ':' << entity->folder << ':' << entity->active;
				if (entity->folder) {
					const auto& t = entity->transform;
					out << ':' << t.translate.x << ':' << t.translate.y << ':' << t.translate.z
						<< ':' << t.scale.x << ':' << t.scale.y << ':' << t.scale.z
						<< ':' << t.rotate.x << ':' << t.rotate.y << ':' << t.rotate.z << ':' << t.rotate.w;
				}
				for (const auto& component : entity->components) {
					if (component.type == "UITransform") {
						out << "|transform:" << component.enabled
							<< ':' << component.uiPosition.x << ':' << component.uiPosition.y
							<< ':' << component.uiSize.x << ':' << component.uiSize.y
							<< ':' << component.uiAnchor.x << ':' << component.uiAnchor.y
							<< ':' << component.uiPivot.x << ':' << component.uiPivot.y
							<< ':' << component.uiRotation << ':' << component.uiScale.x << ':' << component.uiScale.y;
					} else if (component.type == "UICanvas") {
						out << "|canvas:" << component.enabled << ':' << component.uiReferenceSize[0]
							<< ':' << component.uiReferenceSize[1] << ':' << component.uiRenderSpace;
					} else if (component.type == "MenuPanel" || component.type == "MenuItem" ||
						component.type == "TextRenderer" || component.type == "SpriteRenderer" || component.type == "MeshRenderer" ||
						component.type == "PhysicsBody" || component.type == "OBBCollider" ||
						component.type == "SphereCollider" || component.type == "PrefabAnimator") {
						out << '|' << component.type << ':' << component.enabled;
					}
				}
				id = entity->parentId;
			}
			if (id) out << "|end:" << id;
		}

		inline std::string Dependencies(const SceneDocument& document, const SceneEntity& owner,
			const SceneComponent& panel) {
			std::ostringstream out;
			out << std::setprecision(std::numeric_limits<float>::max_digits10) << panel.menuNavigationAxis << '|' << panel.menuLayoutMode;
			AppendAncestors(document, owner.id, out);
			for (uint64_t id : panel.menuItemEntityIds) {
				out << "|item:" << id;
				AppendAncestors(document, id, out);
				const SceneEntity* entity = document.FindEntity(id);
				const SceneComponent* item = entity ? SceneEntityQuery::FindComponent(*entity, "MenuItem") : nullptr;
				if (item && panel.menuLayoutMode != "CanvasItems") {
					out << "|label:" << item->menuLabelEntityId;
					if (item->menuLabelEntityId) AppendAncestors(document, item->menuLabelEntityId, out);
				}
			}
			return out.str();
		}
	}

	// Session専有の非永続cache。DocumentやComponentへのポインタを保持しない。
	class EditorOrderCache {
	public:
		// Load/Undo/Redoでは復元された保存順を採用し、その場で再整列しない。
		void Adopt(const SceneDocument& document) {
			dependencies_.clear();
			for (const auto& entity : document.GetEntities()) {
				const auto* panel = SceneEntityQuery::FindComponent(entity, "MenuPanel");
				if (panel) dependencies_[entity.id] = Detail::Dependencies(document, entity, *panel);
			}
		}

		// Undo確定前に呼ぶ。失敗したPanelは元の配列を保持し、同じ失敗を毎frame計算しない。
		void Refresh(SceneDocument& document, bool force = false) {
			std::unordered_set<uint64_t> live;
			for (auto& entity : document.GetEntities()) {
				auto* panel = SceneEntityQuery::FindComponent(entity, "MenuPanel");
				if (!panel) continue;
				live.insert(entity.id);
				const std::string signature = Detail::Dependencies(document, entity, *panel);
				const auto previous = dependencies_.find(entity.id);
				if (!force && previous != dependencies_.end() && previous->second == signature) continue;
				std::vector<ItemPosition> positions;
				std::string error;
				if (EvaluatePositions(document, entity, *panel, positions, error)) {
					std::stable_sort(positions.begin(), positions.end(), [&](const auto& a, const auto& b) {
						return Coordinate(a, *panel) < Coordinate(b, *panel);
					});
					std::vector<uint64_t> ids;
					for (const auto& position : positions) ids.push_back(position.entityId);
					if (ids != panel->menuItemEntityIds) {
						panel->menuItemEntityIds = std::move(ids);
						document.MarkDirty();
					}
				}
				dependencies_[entity.id] = Detail::Dependencies(document, entity, *panel);
			}
			for (auto it = dependencies_.begin(); it != dependencies_.end();) {
				if (!live.count(it->first)) it = dependencies_.erase(it); else ++it;
			}
		}

	private:
		std::unordered_map<uint64_t, std::string> dependencies_;
	};
}
