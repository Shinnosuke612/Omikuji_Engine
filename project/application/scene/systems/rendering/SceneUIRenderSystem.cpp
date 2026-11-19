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
#include <vector>

namespace {
	using SceneUILayout::PaintGroup;
	using SceneUILayout::PaintItem;
	// PauseもvisiblePanelIdsを参照するため一覧は変えず、描画側だけ開始位置を絞る。
	size_t MenuPaintStart(const SceneDocument& document, const SceneMenuFrame& frame) {
		size_t start = 0;
		for (size_t i = 0; i < frame.visiblePanelIds.size(); ++i) {
			const auto* entity = document.FindEntity(frame.visiblePanelIds[i]);
			if (!entity) continue;
			const auto* panel = SceneEntityQuery::FindEnabledComponent(*entity, "MenuPanel");
			if (panel && panel->menuHideLowerPanels) start = i;
		}
		return start;
	}
	// 資源所有/行列/Panel内の描画順を通常UIと共有し、別の文字cacheを作らない。
	void DrawGroups(const SceneDocument& document, const SceneObjectSystem& objects,
		const SceneTextRenderSystem& texts, const std::vector<PaintGroup>& groups, bool editorView = false) {
		for (const PaintGroup& group : groups) {
			for (const PaintItem& item : group.items) {
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
	DrawGroups(document, objects, texts, SceneUILayout::CollectPaintOrder(document, renderSpace, skipEntityId, editorView), editorView != nullptr);
}

bool SceneUIRenderSystem::HasMenu(const SceneDocument& document, const SceneMenuFrame& frame) const {
	if (!frame.panelId || frame.generation != document.GetContentGeneration() || frame.revision != document.GetRevision()) return false;
	if (frame.visiblePanelIds.empty()) return !SceneUILayout::CollectPaintOrder(document, "ScreenOverlay", 0, nullptr, frame.panelId, &frame).empty();
	for (size_t i = MenuPaintStart(document, frame); i < frame.visiblePanelIds.size(); ++i) {
		if (!SceneUILayout::CollectPaintOrder(document, "ScreenOverlay", 0, nullptr, frame.visiblePanelIds[i], &frame).empty()) return true;
	}
	return false;
}
void SceneUIRenderSystem::DrawMenu(const SceneDocument& document, const SceneObjectSystem& objects,
	const SceneTextRenderSystem& texts, const SceneMenuFrame& frame) const {
	if (!HasMenu(document, frame)) return;
	if (frame.visiblePanelIds.empty()) DrawGroups(document, objects, texts, SceneUILayout::CollectPaintOrder(document, "ScreenOverlay", 0, nullptr, frame.panelId, &frame));
	else for (size_t i = MenuPaintStart(document, frame); i < frame.visiblePanelIds.size(); ++i)
		DrawGroups(document, objects, texts, SceneUILayout::CollectPaintOrder(document, "ScreenOverlay", 0, nullptr, frame.visiblePanelIds[i], &frame));
}

bool SceneUIRenderSystem::HasScreenOverlay(const SceneDocument& document) const {
	return !SceneUILayout::CollectPaintOrder(document, "ScreenOverlay", 0).empty();
}
