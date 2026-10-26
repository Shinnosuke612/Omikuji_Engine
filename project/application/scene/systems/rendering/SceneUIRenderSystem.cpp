// 役割: Canvas単位の安定したHierarchy順から、新UIの一要素描画要求を生成する。
#include "SceneUIRenderSystem.h"
#include "../../../../engine/scene/SceneMenuRuntime.h"
#include "../../../../engine/scene/SceneUIEditorView.h"

#include "SceneObjectSystem.h"
#include "../text/SceneTextRenderSystem.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/SceneUILayout.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
	struct DrawItem {
		uint64_t entityId = 0;
		SceneUILayout::Layout layout;
	};
	struct CanvasItems {
		int order = 0;
		std::vector<DrawItem> items;
	};
	bool IsMenuOwned(const SceneDocument& document, const SceneEntity& root) {
		const SceneEntity* entity = &root;
		for (size_t depth = 0; entity && depth <= document.GetEntities().size(); ++depth) {
			if (SceneEntityQuery::FindComponent(*entity, "MenuPanel")) return true;
			entity = document.FindEntity(entity->parentId);
		}
		return false;
	}

	std::vector<CanvasItems> Collect(const SceneDocument& document, const char* renderSpace,
		uint64_t skipEntityId, const SceneUIEditorView* editorView = nullptr, uint64_t menuPanelId = 0, const SceneMenuFrame* menu = nullptr) {
		if (!renderSpace) return {};
		using SceneEntityQuery::FindComponent;
		using SceneEntityQuery::FindEnabledComponent;
		std::unordered_map<uint64_t, std::vector<const SceneEntity*>> children;
		for (const SceneEntity& entity : document.GetEntities()) children[entity.parentId].push_back(&entity);
		std::vector<CanvasItems> result;
		for (const SceneEntity& root : document.GetEntities()) {
			if (!FindComponent(root, "UICanvas")) continue;
			SceneUILayout::Layout canvas;
			if (!SceneUILayout::Resolve(document, root.id, canvas) || !canvas.visible ||
				canvas.renderSpace != renderSpace || (!editorView && !menuPanelId && IsMenuOwned(document, root))) continue;
			CanvasItems group;
			group.order = canvas.canvasSortingOrder;
			std::vector<const SceneEntity*> stack;
			auto pushChildren = [&](uint64_t id) {
				const auto found = children.find(id);
				if (found != children.end()) for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) stack.push_back(*it);
			};
			pushChildren(root.id);
			std::unordered_set<uint64_t> visited{ root.id };
			while (!stack.empty()) {
				const SceneEntity* entity = stack.back();
				stack.pop_back();
				if (!visited.insert(entity->id).second || FindComponent(*entity, "UICanvas")) continue;
				// 通常HUDはMenu枝を除外。明示された一つのPanelだけ別Passへ渡す。
				const uint64_t allowedPanel = editorView ? editorView->panelId : menuPanelId;
				if (FindComponent(*entity, "MenuPanel") && entity->id != allowedPanel) continue;
				if (menu && std::find(menu->hiddenMarkerRoots.begin(), menu->hiddenMarkerRoots.end(), entity->id) != menu->hiddenMarkerRoots.end()) continue;
				pushChildren(entity->id);
				if ((menuPanelId && entity->id != menuPanelId && !document.IsDescendantOf(entity->id, menuPanelId)) ||
					(editorView && !editorView->Allows(document, entity->id)) ||
					entity->id == skipEntityId || !FindComponent(*entity, "UITransform") ||
					(!FindEnabledComponent(*entity, "SpriteRenderer") && !FindEnabledComponent(*entity, "TextRenderer"))) continue;
				SceneUILayout::Layout layout;
				if (SceneUILayout::Resolve(document, entity->id, layout) && layout.visible && layout.canvasEntityId == root.id) {
					group.items.push_back({ entity->id, std::move(layout) });
				}
			}
			std::stable_sort(group.items.begin(), group.items.end(), [](const DrawItem& left, const DrawItem& right) {
				return left.layout.sortingOrder < right.layout.sortingOrder;
			});
			if (!group.items.empty()) result.push_back(std::move(group));
		}
		std::stable_sort(result.begin(), result.end(), [](const CanvasItems& left, const CanvasItems& right) {
			return left.order < right.order;
		});
		return result;
	}
	// 資源所有/行列/Panel内の描画順を通常UIと共有し、別の文字cacheを作らない。
	void DrawGroups(const SceneDocument& document, const SceneObjectSystem& objects,
		const SceneTextRenderSystem& texts, const std::vector<CanvasItems>& groups, bool editorView = false) {
		for (const CanvasItems& group : groups) {
			for (const DrawItem& item : group.items) {
				const SceneEntity* entity = document.FindEntity(item.entityId);
				if (!entity) continue;
				const bool overlay = item.layout.renderSpace == "ScreenOverlay";
				if (const auto* sprite = SceneEntityQuery::FindEnabledComponent(*entity, "SpriteRenderer")) {
					objects.DrawUISprite(entity->id, SceneUILayout::UnitQuadToClip(item.layout), sprite->spriteColor,
						overlay ? SpriteCommon::OutputTarget::Display : SpriteCommon::OutputTarget::SceneHdr, editorView);
				}
				if (SceneEntityQuery::FindEnabledComponent(*entity, "TextRenderer")) {
					const auto& layout = item.layout;
					const Matrix4x4 layoutToClip = Multiply(layout.localToCanvas,
						MakeOrthographicMatrix(0.0f, 0.0f, layout.referenceSize.x, layout.referenceSize.y, 0.0f, 100.0f));
					texts.DrawUIText(entity->id, layoutToClip, layout.size, layout.pivot,
						overlay ? TextSprite::OutputTarget::Display : TextSprite::OutputTarget::SceneHdr, editorView);
				}
			}
		}
	}

}

void SceneUIRenderSystem::Draw(const SceneDocument& document, const SceneObjectSystem& objects,
	const SceneTextRenderSystem& texts, const char* renderSpace, uint64_t skipEntityId, const SceneUIEditorView* editorView) const {
	DrawGroups(document, objects, texts, Collect(document, renderSpace, skipEntityId, editorView), editorView != nullptr);
}

bool SceneUIRenderSystem::HasMenu(const SceneDocument& document, const SceneMenuFrame& frame) const {
	if (!frame.panelId || frame.generation != document.GetContentGeneration() || frame.revision != document.GetRevision()) return false;
	if (frame.visiblePanelIds.empty()) return !Collect(document, "ScreenOverlay", 0, nullptr, frame.panelId, &frame).empty();
	return std::any_of(frame.visiblePanelIds.begin(), frame.visiblePanelIds.end(), [&](uint64_t id) { return !Collect(document, "ScreenOverlay", 0, nullptr, id, &frame).empty(); });
}
void SceneUIRenderSystem::DrawMenu(const SceneDocument& document, const SceneObjectSystem& objects,
	const SceneTextRenderSystem& texts, const SceneMenuFrame& frame) const {
	if (!HasMenu(document, frame)) return;
	if (frame.visiblePanelIds.empty()) DrawGroups(document, objects, texts, Collect(document, "ScreenOverlay", 0, nullptr, frame.panelId, &frame));
	else for (uint64_t id : frame.visiblePanelIds) DrawGroups(document, objects, texts, Collect(document, "ScreenOverlay", 0, nullptr, id, &frame));
}

bool SceneUIRenderSystem::HasScreenOverlay(const SceneDocument& document) const {
	return !Collect(document, "ScreenOverlay", 0).empty();
}
