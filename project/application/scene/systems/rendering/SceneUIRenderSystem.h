// 役割: 共通UI配置でText/Spriteの描画順をまとめ、既存所有Systemへ描画を委譲する。
#pragma once

#include <cstdint>

class SceneDocument;
struct SceneUIEditorView;
struct SceneMenuFrame;
class SceneObjectSystem;
class SceneTextRenderSystem;

// RuntimeScene所有。Document/Entity pointerやGPU資源をFrame間で保持しない。
class SceneUIRenderSystem {
public:
	// 通常は非MenuのCanvasを描く。検証済みEditor packetがある場合だけ対象Panelを含める。
	// packetは呼出し中だけ借用し、制作座標の投影は表示先viewportで拡縮する。
	void Draw(const SceneDocument& document, const SceneObjectSystem& objects,
		const SceneTextRenderSystem& texts, const char* renderSpace, uint64_t skipEntityId = 0,
		const SceneUIEditorView* editorView = nullptr) const;
	bool HasScreenOverlay(const SceneDocument& document) const;
	// 呼出し元で出所を検証したRuntime packetだけを借り、stackの下→上を描く。
	// 初期Panelを再選択せず、各Panel内の既存描画順を維持する。
	bool HasMenu(const SceneDocument& document, const SceneMenuFrame& frame) const;
	void DrawMenu(const SceneDocument& document, const SceneObjectSystem& objects,
		const SceneTextRenderSystem& texts, const SceneMenuFrame& frame) const;
};
