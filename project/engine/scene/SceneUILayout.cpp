// 役割: GPUやEditor状態を持たず、UIの親子行列と構造・数値の妥当性を解決する。
#include "SceneUILayout.h"

#include "SceneDocument.h"
#include "SceneEntityQuery.h"
#include "SceneMenuRuntime.h"
#include "SceneUIEditorView.h"
#include <unordered_map>
#include <utility>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace {
	using SceneEntityQuery::FindComponent;

	bool Finite(const Vector2& value) {
		return std::isfinite(value.x) && std::isfinite(value.y);
	}
	bool Normalized(const Vector2& value) {
		return Finite(value) && value.x >= 0.0f && value.x <= 1.0f &&
			value.y >= 0.0f && value.y <= 1.0f;
	}
	bool Finite(const Matrix4x4& matrix) {
		for (const auto& row : matrix.m) {
			for (float value : row) if (!std::isfinite(value)) return false;
		}
		return true;
	}
	bool IsNeutralFolder(const SceneEntity& entity) {
		const auto& transform = entity.transform;
		return entity.folder && transform.translate.x == 0.0f &&
			transform.translate.y == 0.0f && transform.translate.z == 0.0f &&
			transform.scale.x == 1.0f && transform.scale.y == 1.0f &&
			transform.scale.z == 1.0f && transform.rotate.x == 0.0f &&
			transform.rotate.y == 0.0f && transform.rotate.z == 0.0f &&
			std::abs(transform.rotate.w) == 1.0f;
	}
	bool ValidateNode(const SceneEntity& entity, bool canvas, std::string& error) {
		for (const char* type : { "UICanvas", "UITransform" }) {
			if (std::count_if(entity.components.begin(), entity.components.end(),
				[type](const SceneComponent& component) { return component.type == type; }) > 1) {
				error = std::string(type) + " must be unique on an Entity";
				return false;
			}
		}
		for (const char* type : { "MeshRenderer", "PhysicsBody", "OBBCollider", "SphereCollider", "PrefabAnimator" }) {
			if (FindComponent(entity, type)) {
				error = "UI Entity cannot share a spatial component: " + std::string(type);
				return false;
			}
		}
		const auto* placement = FindComponent(entity, "UITransform");
		if (const auto* item = FindComponent(entity, "MenuItem")) {
			if (!Finite(item->menuTextPadding) || !Finite(item->menuTextOffset) ||
				item->menuTextPadding.x < 0 || item->menuTextPadding.y < 0 ||
				(placement && (!item->menuLabelEntityId || item->menuLabelEntityId == entity.id) &&
					(2 * item->menuTextPadding.x >= placement->uiSize.x || 2 * item->menuTextPadding.y >= placement->uiSize.y))) {
				error = "MenuRuntime:TextPlacement"; return false;
			}
		}
		if (canvas) {
			if (placement) {
				error = "UICanvas cannot also have UITransform";
				return false;
			}
			return true;
		}
		if (placement) return SceneUILayout::ValidateTransform(*placement, error);
		if (IsNeutralFolder(entity) && !FindComponent(entity, "SpriteRenderer") && !FindComponent(entity, "TextRenderer")) return true;
		error = "UICanvas descendants require UITransform or a neutral folder";
		return false;
	}
}

bool SceneUILayout::IsUIEntity(const SceneDocument& document, const SceneEntity& entity) {
	if (FindComponent(entity, "UITransform")) return true;
	const SceneEntity* current = &entity;
	std::unordered_set<uint64_t> visited;
	while (current && visited.insert(current->id).second) {
		if (FindComponent(*current, "UICanvas") || FindComponent(*current, "UITransform")) return true;
		current = document.FindEntity(current->parentId);
	}
	return false;
}

bool SceneUILayout::ValidateCanvas(const SceneComponent& component, std::string& error) {
	error.clear();
	if (component.uiReferenceSize[0] == 0 || component.uiReferenceSize[1] == 0) {
		error = "UICanvas referenceSize must contain two positive integers";
	} else if (component.uiRenderSpace != "ScreenOverlay" && component.uiRenderSpace != "Scene2D") {
		error = "UICanvas has an unknown renderSpace";
	}
	return error.empty();
}

bool SceneUILayout::ValidateTransform(const SceneComponent& component, std::string& error) {
	error.clear();
	if (!Finite(component.uiPosition) || !Finite(component.uiSize) ||
		!Finite(component.uiScale) || !std::isfinite(component.uiRotation) ||
		component.uiSize.x <= 0.0f || component.uiSize.y <= 0.0f ||
		component.uiScale.x <= 0.0f || component.uiScale.y <= 0.0f ||
		!Normalized(component.uiAnchor) || !Normalized(component.uiPivot)) {
		error = "UITransform requires finite position/rotation, positive size/scale and anchor/pivot in [0,1]";
	}
	return error.empty();
}

bool SceneUILayout::Resolve(const SceneDocument& document, uint64_t entityId,
	Layout& out, std::string* error) {
	out = {};
	if (error) error->clear();
	auto fail = [&](const std::string& reason) {
		if (error) *error = reason;
		return false;
	};
	if (entityId == 0) return fail("UI Entity ID must be greater than zero");
	const SceneEntity* entity = document.FindEntity(entityId);
	if (!entity) return fail("UI Entity does not exist");
	std::unordered_set<uint64_t> visited;
	std::vector<const SceneEntity*> chain;
	const SceneEntity* canvasEntity = nullptr;
	const SceneComponent* canvas = nullptr;
	const SceneEntity* current = entity;
	bool visible = true;
	// Canvasより上も循環と入れ子を確認する。3D祖先の空間変換は合成しない。
	while (current) {
		if (!visited.insert(current->id).second) return fail("UI hierarchy contains a cycle");
		visible = visible && current->active;
		if (const auto* found = FindComponent(*current, "UICanvas")) {
			if (canvas) return fail("Nested UICanvas is not supported");
			canvas = found;
			canvasEntity = current;
		} else if (!canvas) {
			chain.push_back(current);
		}
		if (current->parentId == 0) break;
		current = document.FindEntity(current->parentId);
		if (!current) return fail("UI hierarchy references a missing parent");
	}
	std::string reason;
	// Prefab部品がCanvas未接続でも、配置値と親子構造のエラーを欠落Canvas警告へ隠さない。
	for (const SceneEntity* node : chain) if (!ValidateNode(*node, false, reason)) return fail(reason);
	if (!canvas) return fail("UITransform requires a UICanvas ancestor");
	if (!ValidateCanvas(*canvas, reason)) return fail(reason);
	if (!ValidateNode(*canvasEntity, true, reason)) return fail(reason);
	// 標準Menuでは所有階層と画面基準を一致させ、隠れた親Transformを作らない。
	for (const SceneEntity* node : chain) {
		if (FindComponent(*node, "MenuItem")) {
			const auto* parent = document.FindEntity(node->parentId);
			for (size_t depth = 0; parent && depth <= document.GetEntities().size(); ++depth) {
				if (const auto* panel = FindComponent(*parent, "MenuPanel")) {
					if (panel->menuLayoutMode == "CanvasItems" && (node->parentId != parent->id || canvasEntity->id != parent->id))
						return fail("MenuRuntime:StandardHierarchy");
					break;
				}
				parent = document.FindEntity(parent->parentId);
			}
		}
	}
	auto validatePanel = [&](const SceneEntity& owner) {
		const auto* panel = FindComponent(owner, "MenuPanel");
		if (!panel) return true;
		// SceneValidatorと同じ入力方式を許可し、読込と描画で配置の判定を揃える。
		if ((panel->menuLayoutMode != "Hierarchical" && panel->menuLayoutMode != "CanvasItems") ||
			(panel->menuInputMode != "Events" && panel->menuInputMode != "Standard" && panel->menuInputMode != "Pointer")) return false;
		if (panel->menuLayoutMode != "CanvasItems") return true;
		const auto& t = owner.transform;
		if (owner.id != canvasEntity->id || FindComponent(owner, "UITransform") ||
			FindComponent(owner, "TextRenderer") || FindComponent(owner, "SpriteRenderer") ||
			t.translate.x != 0 || t.translate.y != 0 || t.translate.z != 0 ||
			t.scale.x != 1 || t.scale.y != 1 || t.scale.z != 1 ||
			t.rotate.x != 0 || t.rotate.y != 0 || t.rotate.z != 0 || std::abs(t.rotate.w) != 1) return false;
		const auto* ancestor = document.FindEntity(owner.parentId);
		for (size_t depth = 0; ancestor && depth <= document.GetEntities().size(); ++depth) {
			if (!IsNeutralFolder(*ancestor) || FindComponent(*ancestor, "UITransform") || FindComponent(*ancestor, "UICanvas") || FindComponent(*ancestor, "MenuPanel")) return false;
			for (const char* type : {"MeshRenderer", "PhysicsBody", "OBBCollider", "SphereCollider", "PrefabAnimator", "TextRenderer", "SpriteRenderer"})
				if (FindComponent(*ancestor, type)) return false;
			ancestor = document.FindEntity(ancestor->parentId);
		}
		return !ancestor && canvas->uiRenderSpace == "ScreenOverlay";
	};
	if (!validatePanel(*canvasEntity)) return fail("MenuRuntime:StandardHierarchy");
	for (const auto* node : chain) if (!validatePanel(*node)) return fail("MenuRuntime:StandardHierarchy");
	Layout result;
	result.canvasEntityId = canvasEntity->id;
	result.referenceSize = { static_cast<float>(canvas->uiReferenceSize[0]),
		static_cast<float>(canvas->uiReferenceSize[1]) };
	result.renderSpace = canvas->uiRenderSpace;
	result.canvasSortingOrder = canvas->uiCanvasSortingOrder;
	result.size = result.referenceSize;
	visible = visible && canvas->enabled;
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		const SceneEntity& node = **it;
		const SceneComponent* placement = FindComponent(node, "UITransform");
		if (!placement) continue;
		result.parentSize = result.size;
		const Vector2 offset{ placement->uiAnchor.x * result.parentSize.x + placement->uiPosition.x,
			placement->uiAnchor.y * result.parentSize.y + placement->uiPosition.y };
		const Matrix4x4 local = Multiply(
			MakeTranslateMatrix({ -placement->uiPivot.x * placement->uiSize.x,
				-placement->uiPivot.y * placement->uiSize.y, 0.0f }),
			MakeAffineMatrix({ placement->uiScale.x, placement->uiScale.y, 1.0f },
				Vector3{ 0.0f, 0.0f, placement->uiRotation }, { offset.x, offset.y, 0.0f }));
		result.localToCanvas = Multiply(local, result.localToCanvas);
		if (!Finite(result.localToCanvas) || !Finite(Multiply(
			MakeScaleMatrix({ placement->uiSize.x, placement->uiSize.y, 1.0f }), result.localToCanvas))) {
			return fail("UI layout matrix is not finite");
		}
		result.size = placement->uiSize;
		result.pivot = placement->uiPivot;
		result.sortingOrder = placement->uiSortingOrder;
		visible = visible && placement->enabled;
	}
	const float pivotX = result.pivot.x * result.size.x;
	const float pivotY = result.pivot.y * result.size.y;
	result.pivotInCanvas = {
		pivotX * result.localToCanvas.m[0][0] + pivotY * result.localToCanvas.m[1][0] + result.localToCanvas.m[3][0],
		pivotX * result.localToCanvas.m[0][1] + pivotY * result.localToCanvas.m[1][1] + result.localToCanvas.m[3][1]
	};
	if (!Finite(result.pivotInCanvas)) return fail("UI layout pivot is not finite");
	if (!Finite(UnitQuadToClip(result))) return fail("UI projected layout matrix is not finite");
	result.visible = visible;
	out = result;
	return true;
}

Matrix4x4 SceneUILayout::UnitQuadToClip(const Layout& layout) {
	return Multiply(Multiply(MakeScaleMatrix({ layout.size.x, layout.size.y, 1.0f }),
		layout.localToCanvas), MakeOrthographicMatrix(0.0f, 0.0f,
			layout.referenceSize.x, layout.referenceSize.y, 0.0f, 100.0f));
}

namespace {
	bool IsMenuOwned(const SceneDocument& document, const SceneEntity& root) {
		const SceneEntity* entity = &root;
		for (size_t depth = 0; entity && depth <= document.GetEntities().size(); ++depth) {
			if (SceneEntityQuery::FindComponent(*entity, "MenuPanel")) return true;
			entity = document.FindEntity(entity->parentId);
		}
		return false;
	}

}
std::vector<SceneUILayout::PaintGroup> SceneUILayout::CollectPaintOrder(const SceneDocument& document, const char* renderSpace,
	uint64_t skipEntityId, const SceneUIEditorView* editorView, uint64_t menuPanelId, const SceneMenuFrame* menu, bool includeMenuItems) {
	if (!renderSpace) return {};
	using SceneEntityQuery::FindComponent;
	using SceneEntityQuery::FindEnabledComponent;
	std::unordered_map<uint64_t, std::vector<const SceneEntity*>> children;
	for (const SceneEntity& entity : document.GetEntities()) children[entity.parentId].push_back(&entity);
	std::vector<PaintGroup> result;
	for (const SceneEntity& root : document.GetEntities()) {
		if (!FindComponent(root, "UICanvas")) continue;
		SceneUILayout::Layout canvas;
		if (!SceneUILayout::Resolve(document, root.id, canvas) || !canvas.visible ||
			canvas.renderSpace != renderSpace || (!editorView && !menuPanelId && IsMenuOwned(document, root))) continue;
		PaintGroup group;
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
			if (menu && std::find(menu->hiddenVisualRoots.begin(), menu->hiddenVisualRoots.end(), entity->id) != menu->hiddenVisualRoots.end()) continue;
			pushChildren(entity->id);
			if ((menuPanelId && entity->id != menuPanelId && !document.IsDescendantOf(entity->id, menuPanelId)) ||
				(editorView && !editorView->Allows(document, entity->id)) ||
				entity->id == skipEntityId || !FindComponent(*entity, "UITransform") ||
				(!FindEnabledComponent(*entity, "SpriteRenderer") && !FindEnabledComponent(*entity, "TextRenderer") && !(includeMenuItems && FindEnabledComponent(*entity, "MenuItem")))) continue;
			SceneUILayout::Layout layout;
			if (SceneUILayout::Resolve(document, entity->id, layout) && layout.visible && layout.canvasEntityId == root.id) {
				group.items.push_back({ entity->id, std::move(layout) });
			}
		}
		std::stable_sort(group.items.begin(), group.items.end(), [](const PaintItem& left, const PaintItem& right) {
			return left.layout.sortingOrder < right.layout.sortingOrder;
		});
		if (!group.items.empty()) result.push_back(std::move(group));
	}
	std::stable_sort(result.begin(), result.end(), [](const PaintGroup& left, const PaintGroup& right) {
		return left.order < right.order;
	});
	return result;
}

bool SceneUILayout::ContainsNormalizedPoint(const Layout& layout, const Vector2& point) {
	if (!layout.visible || layout.renderSpace != "ScreenOverlay" || !Finite(point) ||
		point.x < 0 || point.x >= 1 || point.y < 0 || point.y >= 1) return false;
	const auto& m = layout.localToCanvas.m;
	// UIは2D affine。doubleで逆変換し、小scaleでもfloat逆行列のoverflowを避ける。
	const double determinant = double(m[0][0]) * m[1][1] - double(m[0][1]) * m[1][0];
	if (!std::isfinite(determinant) || determinant == 0) return false;
	const double x = double(point.x) * layout.referenceSize.x - m[3][0];
	const double y = double(point.y) * layout.referenceSize.y - m[3][1];
	const double localX = (x * m[1][1] - y * m[1][0]) / determinant;
	const double localY = (y * m[0][0] - x * m[0][1]) / determinant;
	return std::isfinite(localX) && std::isfinite(localY) &&
		localX >= 0 && localX < layout.size.x && localY >= 0 && localY < layout.size.y;
}
