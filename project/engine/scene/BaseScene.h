// 役割: すべてのシーンに共通する更新と描画のライフサイクルを定義する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "SceneEntityReference.h"
#include "ScenePreparationDiagnostic.h"
#include "SceneRuntimePreparation.h"
#include "SceneMenuRuntime.h"
#include "SceneReplayHistory.h"
#include "SceneReplayPresentation.h"

class Camera;
class Object3d;
class SceneDocument;
class SceneInstance;
class SceneManager;
struct PreparedSceneTextImage;
struct ScenePostProcessSettings;
struct SceneUIEditorView;
struct SceneEnemyPositioningStatus;

// 既存値を維持して、記録後の3視点再生状態を末尾へ追加する。
enum class SceneDeathReplayPhase { Waiting, LiveSlow, Frozen, Failed, Preparing, ReplayA, ReplayB, ReplayC, FinishedHold };
// Inspector向け値コピー。Sceneや演出状態の所有権を公開しない。
struct SceneDeathReplayStatus {
	SceneDeathReplayPhase phase = SceneDeathReplayPhase::Waiting;
	uint64_t owner = 0;
	float remainingRealSeconds = 0.0f;
	std::string diagnostic;
	double replaySeconds = 0.0, replayDuration = 0.0;
	uint32_t cut = 0;
	bool awaitingPresentation = false, staticClip = false;
	uint64_t gpuBytes = 0, descriptors = 0;
};
// 読取専用の死亡世界時計。Scene参照や一回消費する通知を外へ渡さない。
struct SceneViewEffectClock {
	uint64_t contentGeneration = 0;
	double deathWorldElapsedSeconds = 0.0;
};

// SceneInstanceを介してSceneManagerが所有するSceneの共通契約。派生側は各Passの順序を変えない。
class BaseScene
{
public:
	virtual ~BaseScene() = default;

	// InitializeとFinalizeはScene切り替えごとに一度ずつ対で呼ばれる。
	virtual void Initialize() = 0;
	virtual void Finalize() = 0;
	// mainの候補専用。非対応profileは同期Initializeへフォールバックしない。
	virtual bool BeginPreparedRuntime() { return false; }
	virtual RuntimePrepareStep StepPreparedRuntime(const SceneRuntimePrepareBudget&, std::string&) {
		return RuntimePrepareStep::Unsupported;
	}
	virtual void CancelPreparedRuntime() noexcept {}
	virtual bool TryTakePreparedTextImage(PreparedSceneTextImage&) { return false; }
	virtual bool AcceptPreparedTextImage(uint64_t, const std::string&, std::string&) { return false; }
	virtual void ActivatePreparedRuntime() noexcept {}
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
	// ゲーム内PauseはEditor Debug Cameraを動かさず、UI表示だけ更新する。
	virtual bool SupportsGamePause() const { return false; }
	virtual void UpdateGamePaused(float uiDeltaTime) { (void)uiDeltaTime; }
	// Pause中はSimulationを進めず、Debug Cameraなど必要な表示状態だけを更新する。
	virtual void UpdatePaused() {}
	// 対応Sceneだけが、Simulationや開始Eventを進めずに描画準備できる。
	virtual bool SupportsTransitionHold() const { return false; }
	virtual void UpdateTransitionHold() {}

	// 物理/演出の更新有無に関わらず、各描画フレームの開始を一度通知する。
	virtual void BeginRenderFrame(uint64_t serial) { (void)serial; }
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

	// 全体敗北中だけ累積scaled秒を返す任意口。未対応/不一致ならout不変でfalse。
	virtual bool TryGetViewEffectClock(SceneViewEffectClock& out) const { (void)out; return false; }

	// Editor用の任意の読取口。Runtimeを持たないSceneはfalse、保存値へ反映しない。
	virtual bool TryGetRuntimeStat(uint64_t, const std::string&, float&, float* = nullptr) const { return false; }
	// -1は未対応。値は死亡演出の受付状態で、SceneやRuntimeへの可変参照を返さない。
	virtual int GetDeathResponseStatus(uint64_t) const { return -1; }
	// 値コピーのみ。未対応/対象不一致はfalseでout不変。
	// Active Gameの最初の描画前だけ通知。時計を進めず、CPU履歴を確定する。
    // Gameの全View GPU利用完了通知。Scene破棄前hookは必要時だけ未提出commandをdrainする。
    // Gameだけが呼ぶ。Prepareは前frameの全GPU利用完了後、Shadowより前。
    virtual bool TryGetReplayFraming(uint64_t, SceneReplayFramingBasis&) const { return false; }
    virtual void AdvanceReplayPresentation(float, bool) {}
    virtual bool PrepareReplayGameFrame(uint64_t, float, SceneReplayGameFrame&) { return false; }
    virtual bool DrawReplayGameShadow(const SceneReplayPresentationToken&) { return false; }
    virtual bool DrawReplayGameWorld(const SceneReplayPresentationToken&) { return false; }
    virtual void RejectReplayGameFrame(const SceneReplayPresentationToken&) {}
    virtual void AcceptReplayPresentationReceipt(const SceneReplayPresentationToken&) {}
    // 成功時だけ実際に描いたPanelの出所を返す。GPU完了後に同じ値を返して入力を解禁する。
    virtual bool DrawReplayMenu(const SceneReplayPresentationToken&, SceneMenuSource&) { return false; }
    virtual void AcceptReplayMenuReceipt(const SceneReplayPresentationToken&, const SceneMenuSource&) {}
    virtual void NotifyReplayGpuCompleted() {}
    virtual void PrepareReplayForFinalize() {}
	virtual void CaptureReplayBeforeDraw(const SceneReplayCaptureRequest&) {}
	virtual bool TryGetReplayHistoryStatus(uint64_t, SceneReplayHistoryStatus&) const { return false; }
	virtual bool TryGetDeathReplayStatus(uint64_t, SceneDeathReplayStatus&) const { return false; }
	virtual bool TryGetRuntimeFlag(uint64_t, const std::string&, bool&) const { return false; }
	// Editor向けの個体値コピー。未対応/未初期化はfalse、out不変。
	virtual bool TryGetEnemyPositioningStatus(uint64_t, SceneEnemyPositioningStatus&) const { return false; }

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
