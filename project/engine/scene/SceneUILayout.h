// 役割: UIの保存値を参照し、Canvas制作座標の配置・描画順・枠内判定を純粋に解決する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../math/Matrix4x4.h"
#include "../math/Vector2.h"

class SceneDocument;
struct SceneComponent;
struct SceneEntity;
struct SceneUIEditorView;
struct SceneMenuFrame;

namespace SceneUILayout {
	struct Layout {
		uint64_t canvasEntityId = 0;
		Vector2 referenceSize{};
		Vector2 parentSize{};
		Vector2 size{};
		Vector2 pivot{};
		Vector2 pivotInCanvas{};
		Matrix4x4 localToCanvas = MakeIdentity4x4();
		std::string renderSpace;
		int canvasSortingOrder = 0;
		int sortingOrder = 0;
		bool visible = false;
	};

	struct PaintItem {
		uint64_t entityId = 0;
		Layout layout;
	};
	struct PaintGroup {
		int order = 0;
		std::vector<PaintItem> items;
	};
	// Canvas順→sortingOrder→Hierarchy安定順。Menu枝maskを描画と共有する。
	// includeMenuItemsは画像/文字のない操作枠も含める。Documentは変更しない。
	std::vector<PaintGroup> CollectPaintOrder(const SceneDocument& document, const char* renderSpace,
		uint64_t skipEntityId, const SceneUIEditorView* editorView = nullptr,
		uint64_t menuPanelId = 0, const SceneMenuFrame* menu = nullptr, bool includeMenuItems = false);
	// 画像内[0,1)座標を保存枠へ逆変換する。特異/非有限行列は操作不可。
	bool ContainsNormalizedPoint(const Layout& layout, const Vector2& point);

	// 無効Componentも境界として扱う。trueのEntityを旧Sprite/Text経路へ戻さない。
	bool IsUIEntity(const SceneDocument& document, const SceneEntity& entity);
	// 値域検証はenabledに依存しない。失敗時はerrorを置換する。
	bool ValidateCanvas(const SceneComponent& component, std::string& error);
	bool ValidateTransform(const SceneComponent& component, std::string& error);
	// Documentを変更せず、Canvas/親枠/基点/完全な行列を返す。失敗時outは空にする。
	// disabled/非activeは構造エラーとは区別し、成功時のvisible=falseで返す。
	bool Resolve(const SceneDocument& document, uint64_t entityId, Layout& out,
		std::string* error = nullptr);
	// Resolve成功後のLayout専用。[0,1] quad→枠→制作座標→clip。
	// viewportが各表示先の縦横倍率を担い、同じUIの行列は各表示先で共用できる。
	Matrix4x4 UnitQuadToClip(const Layout& layout);
}
