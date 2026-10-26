// 役割: すべてのシーンに共通する更新と描画のライフサイクルを定義する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "SceneEntityReference.h"
#include "ScenePreparationDiagnostic.h"
#include "SceneMenuRuntime.h"

class Camera;
class Object3d;
class SceneDocument;
class SceneInstance;
class SceneManager;
struct ScenePostProcessSettings;
struct SceneUIEditorView;

// SceneInstanceを介してSceneManagerが所有するSceneの共通契約。派生側は各Passの順序を変えない。
class BaseScene
{
public:
	virtual ~BaseScene() = default;

	// InitializeとFinalizeはScene切り替えごとに一度ずつ対で呼ばれる。
	virtual void Initialize() = 0;
	virtual void Finalize() = 0;
	// Single Scene切替で旧Sceneを破棄する直前だけ呼ばれる任意hook。
	virtual void PrepareForSceneTransition() {}

	virtual void Update(float deltaTime) = 0;
	// 診断は値のsnapshot。既定のSceneには準備失敗なし。
	virtual ScenePreparationDiagnostic GetPreparationDiagnostic() const { return {}; }
	// Active確定後・通常更新前のUI入力段階。非許可時も離し待ちと旧Instance破棄を処理する。
	virtual void PrepareMenuInput(float deltaTime, bool allowed) { (void)deltaTime; (void)allowed; }
	virtual SceneUiControlFrame GetUiControlFrame() const { return {}; }
	// 入力だけを遮断する。Simulation/Editor Pauseの意味は変えない。
	virtual void SetUiGameplayInputBlocked(bool blocked) { (void)blocked; }
	// Pause中はSimulationを進めず、Debug Cameraなど必要な表示状態だけを更新する。
	virtual void UpdatePaused() {}
	// 対応Sceneだけが、Simulationや開始Eventを進めずに描画準備できる。
	virtual bool SupportsTransitionHold() const { return false; }
	virtual void UpdateTransitionHold() {}

	virtual void Draw() = 0;
	// Additive描画ではActive SceneのCameraを全Instanceへ共有する。
	virtual Camera* GetRenderCamera() const { return nullptr; }
	virtual void DrawWithCamera(Camera* viewCamera) {
		(void)viewCamera;
		Draw();
	}
	// Main ViewではActive SceneがEnvironmentとLightingを提供し、全Sceneが内容を描画する。
	virtual void DrawEnvironment(Camera* viewCamera) { (void)viewCamera; }
	virtual void BindLighting() {}
	virtual void DrawSceneContent(Camera* viewCamera) {
		DrawWithCamera(viewCamera);
	}
	virtual void PrepareSceneContent(Camera* viewCamera) { (void)viewCamera; }
	virtual void DrawPreparedSceneContent(Camera* viewCamera) {
		DrawSceneContent(viewCamera);
	}
	// 以下は対応する機能を持つSceneだけが上書きする任意Pass。
	virtual void DrawForegroundEffects() {}
	virtual void DrawForegroundEffectsWithCamera(Camera* viewCamera) {
		(void)viewCamera;
		DrawForegroundEffects();
	}
	// Scene Viewの単独編集表示だけが呼ぶ。非対応Sceneは何も描かない。
	virtual void DrawEditorUI(const SceneUIEditorView& view, const char* renderSpace) { (void)view; (void)renderSpace; }
	// Active SceneのMenuは、全Sceneの通常Overlayが終わってから描く。
	virtual bool HasMenuOverlay() const { return false; }
	virtual void DrawMenuOverlay() {}
	virtual bool HasScreenOverlay() const { return false; }
	virtual void DrawScreenOverlay(uint32_t width, uint32_t height) {
		(void)width;
		(void)height;
	}
	virtual void DrawShadow() {}
	virtual void CollectShadowCasters(std::vector<Object3d*>& shadowCasters) {
		(void)shadowCasters;
	}
	virtual void RenderShadowCasters(
		const std::vector<Object3d*>& shadowCasters
	) {
		(void)shadowCasters;
		DrawShadow();
	}
	virtual void DrawOffscreenViews() {}
	virtual void SetDeferForegroundEffects(bool defer) { (void)defer; }
	// Runtime-only Profileなど、Scene更新後に有効になる描画設定を返す任意口。
	// 非Runtime Sceneはfalseのままで既存のDocument Baselineを使う。
	virtual bool TryGetRuntimePostProcessSettings(
		ScenePostProcessSettings& settings,
		uint64_t& generation
	) const {
		(void)settings;
		(void)generation;
		return false;
	}

	virtual void SetSceneManager(SceneManager* sceneManager) { sceneManager_ = sceneManager; }
	virtual void SetSceneInstance(SceneInstance* sceneInstance) {
		sceneInstance_ = sceneInstance;
	}


protected:
	SceneDocument* GetSceneDocument() const;
	const std::string& GetSceneAssetId() const;
	SceneInstanceId GetSceneInstanceId() const;

	SceneManager* sceneManager_ = nullptr;
	SceneInstance* sceneInstance_ = nullptr;
};
