// 役割: SceneDocumentを実行時オブジェクトへ反映し、更新と描画を管理する。
#pragma once
#include <array>
#include <unordered_map>
#include <unordered_set>
#include "../../engine/scene/BaseScene.h"
#include "SceneRuntimeObjectBinding.h"
#include "SceneSpawnPresentation.h"
#include "systems/gameplay/ai/SceneAgentSystem.h"
#include "systems/audio/SceneAudioSystem.h"
#include "systems/gameplay/combat/SceneAttackRunnerSystem.h"
#include "systems/animation/SceneAttachmentSystem.h"
#include "systems/camera/SceneCameraSystem.h"
#include "systems/gameplay/combat/SceneCombatSystem.h"
#include "systems/debug/SceneDebugSystem.h"
#include "systems/gameplay/ai/SceneEnemySystem.h"
#include "systems/gameplay/ai/SceneEnemySpawnerSystem.h"
#include "systems/events/SceneEventSystem.h"
#include "systems/menu/SceneMenuSystem.h"
#include "systems/gameplay/combat/SceneHitReactionSystem.h"
#include "systems/gameplay/combat/SceneHitStopSystem.h"
#include "systems/gameplay/flow/SceneGameFlowSystem.h"
#include "systems/effects/SceneEffectRenderSystem.h"
#include "systems/rendering/SceneEnvironmentSystem.h"
#include "systems/rendering/SceneLightingSystem.h"
#include "systems/rendering/SceneMonitorSystem.h"
#include "systems/rendering/SceneObjectSystem.h"
#include "systems/replay/SceneReplayHistorySystem.h"
#include "systems/replay/SceneReplayRenderSystem.h"
#include "systems/rendering/SceneUIRenderSystem.h"
#include "systems/effects/SceneParticleSystem.h"
#include "systems/effects/ScenePostProcessProfileSystem.h"
#include "systems/physics/ScenePhysicsSystem.h"
#include "systems/animation/ScenePrefabAnimationSystem.h"
#include "systems/gameplay/combat/SceneProjectileSystem.h"
#include "systems/effects/SceneRuntimeEffectSystem.h"
#include "systems/gameplay/combat/SceneStatSystem.h"
#include "systems/gameplay/state/SceneStateMachineSystem.h"
#include "systems/text/SceneTextRenderSystem.h"
#include "systems/text/SceneTextMotionSystem.h"
#include "systems/events/SceneTransitionSystem.h"

#include <cstdint>
#include <vector>

class Camera;
class Object3d;
class Player;

// 各SystemとCameraを所有し、Component同期から描画までの実行順だけを決定する。
class RuntimeScene : public BaseScene
{
public:
	// BaseSceneのライフサイクル入口。個別機能は対応するSystemへ委譲する。
	void Initialize() override;
	void Finalize() override;
	void PrepareForSceneTransition() override;
	void Update(float deltaTime) override;
	ScenePreparationDiagnostic GetPreparationDiagnostic() const override { return preparationDiagnostic_; }
	// 通常更新前にUI Eventを評価。非Active/Editでは状態を破棄し、入力非許可では時計を凍結する。
	void PrepareMenuInput(float deltaTime, bool allowed) override;
	SceneUiControlFrame GetUiControlFrame() const override { return uiControl_; }
	void SetUiGameplayInputBlocked(bool blocked) override { uiGameplayInputBlocked_ = blocked; }
	void UpdatePaused() override;
	bool SupportsGamePause() const override { return true; }
	// Simulation/Camera/Audio autoplayは呼ばず、最上位Menuの文字時計と描画同期だけ進める。
	void UpdateGamePaused(float uiDeltaTime) override;
	bool SupportsTransitionHold() const override { return true; }
	// 保持中は描画同期だけ。Event/Audio autoplay/SimulationのRuntime stateを変更しない。
	void UpdateTransitionHold() override;
	void Draw() override;
	Camera* GetRenderCamera() const override;
	// 現Document世代の全体敗北だけを公開。Pause/描画で時計を進めない。
	bool TryGetViewEffectClock(SceneViewEffectClock& out) const override;
	void NotifyReplayGpuCompleted() override;
	void PrepareReplayForFinalize() override;
	void CaptureReplayBeforeDraw(const SceneReplayCaptureRequest& request) override;
    bool TryGetReplayFraming(uint64_t owner, SceneReplayFramingBasis& out) const override;
    void AdvanceReplayPresentation(float delta, bool allowed) override;
    bool PrepareReplayGameFrame(uint64_t drawSerial, float aspect, SceneReplayGameFrame& out) override;
    bool DrawReplayGameShadow(const SceneReplayPresentationToken&) override;
    bool DrawReplayGameWorld(const SceneReplayPresentationToken&) override;
    void RejectReplayGameFrame(const SceneReplayPresentationToken&) override;
    void AcceptReplayPresentationReceipt(const SceneReplayPresentationToken&) override;
    bool DrawReplayMenu(const SceneReplayPresentationToken&, SceneMenuSource&) override;
    void AcceptReplayMenuReceipt(const SceneReplayPresentationToken&, const SceneMenuSource&) override;
	bool TryGetReplayHistoryStatus(uint64_t owner, SceneReplayHistoryStatus& out) const override;
	bool TryGetDeathReplayStatus(uint64_t owner, SceneDeathReplayStatus& out) const override;
	void DrawWithCamera(Camera* viewCamera) override;
	void DrawEnvironment(Camera* viewCamera) override;
	void BindLighting() override;
	void DrawSceneContent(Camera* viewCamera) override;
	void PrepareSceneContent(Camera* viewCamera) override;
	void DrawPreparedSceneContent(Camera* viewCamera) override;
	void DrawForegroundEffects() override;
	void DrawForegroundEffectsWithCamera(Camera* viewCamera) override;
	// Scene View専用。出所一致を検証し、既存資源を借りて基本配置を描く。
	void DrawEditorUI(const SceneUIEditorView& view, const char* renderSpace) override;
	// ゲーム実行中のActive Instanceだけ。通常Overlayの後へ検証済みMenu packetを渡す。
	bool HasMenuOverlay() const override;
	void DrawMenuOverlay() override;
	bool HasScreenOverlay() const override;
	void DrawScreenOverlay(uint32_t width, uint32_t height) override;
	void DrawOffscreenViews() override;
	void DrawShadow() override;
	void CollectShadowCasters(std::vector<Object3d*>& shadowCasters) override;
	void RenderShadowCasters(
		const std::vector<Object3d*>& shadowCasters
	) override;
	void SetDeferForegroundEffects(bool defer) override {
		effectRenderSystem_.SetDeferForegroundEffects(defer);
	}
	bool TryGetRuntimeStat(uint64_t id, const std::string& key, float& value, float* maxValue = nullptr) const override { return statSystem_.TryGet(id, key, value, nullptr, maxValue); }
	int GetDeathResponseStatus(uint64_t id) const override { return deathSystem_.Status(id); }
	bool TryGetRuntimeFlag(uint64_t id, const std::string& key, bool& value) const override { return statSystem_.TryGetFlag(id, key, value); }
	bool TryGetEnemyPositioningStatus(uint64_t id, SceneEnemyPositioningStatus& out) const override {
		return enemySystem_.TryGetPositioningStatus(id, out);
	}
	bool TryGetRuntimePostProcessSettings(
		ScenePostProcessSettings& settings,
		uint64_t& generation
	) const override;

private:
	const SceneUIEditorView* GetEditorUIView() const;
	void SyncMenuPresentation();
	void ClearUiRuntime();
	void ApplyUiRequests(const SceneDocument& document, const std::vector<SceneUiActionRequest>& requests);
	void ProcessMenuPulses(const SceneDocument& document, const std::vector<SceneMenuPulse>& pulses);
	void ApplyDeathActions(SceneDocument& document, bool transformOnly);
	void DispatchDeaths(SceneDocument& document);
	void FailDeathReplay(const SceneDeathStarted& pulse, const std::string& issue);
	// 最初のScene敗北pulseでWorld動作とCameraを同じ境界に固定する。
	// ClearはSequenceを含む継続状態を破棄し、完了通知を出さない。
	bool BeginDeathWorldContinuation(const SceneDeathStarted& started, float scale, float duration);
	void ClearDeathWorldContinuation();
	void ReportMenuFailure(const std::string& detail);
	void SyncGamePause(const SceneDocument& document);
	// UI prepassでは検証/予約だけ行い、GameのAudio停止理由反映後にVoiceを操作する。
	void ApplyPendingUiAudio();
	// 初回/内容置換時だけ空表示を準備する。reset=trueは通常Frameのoverride再構築用。
	// 同世代の遷移hold/Pauseでは、進行済みの文字を保持する。
	void PrepareGameFlowTextOverrides(bool reset);
	// 発生元のserialを独立に照合し、Editor向けにはScene内で一意のserialへ束ねる。
	void ReportPreparationDiagnostic(const ScenePreparationDiagnostic& diagnostic, uint64_t& sourceSerial);
	// 再読込拒否/失敗だけを既存の言語対応Popup/ログ経路へ渡す。Scene進行は停止しない。
	void ReportSceneRestartFailure(const std::string& detail);
	void DrawSceneView(Camera* viewCamera, uint64_t skipEntityId = 0);
	void DrawPreparedSceneContentForView(
		Camera* viewCamera,
		uint64_t skipEntityId
	);
	bool ShouldHidePlayerModelForCamera(Camera* viewCamera) const;
	void ApplyRenderCamera(Camera* viewCamera);
	Camera* GetSceneViewCamera() const;

	Camera* camera_ = nullptr;
	Camera* debugCamera_ = nullptr;
	Player* player_ = nullptr;
	std::vector<SceneRuntimeObjectBinding> runtimeObjectBindings_;
	// Objectの所有は変えず、出現中branchだけをSimulationから除外する。
	std::vector<SceneRuntimeObjectBinding> gameplayObjectBindings_;
	SceneSpawnFrame spawnFrame_;
	ScenePreparationDiagnostic preparationDiagnostic_;
	uint64_t flowDiagnosticSerial_ = 0, spawnerDiagnosticSerial_ = 0;
	uint64_t preparationDiagnosticSerial_ = 0;
	uint64_t gameFlowTextDocumentGeneration_ = 0;
	SceneInstanceId pendingRestartSceneInstanceId_ = kInvalidSceneInstanceId;
	SceneUiControlFrame uiControl_;
	bool uiGameplayInputBlocked_ = false;
	std::vector<SceneUiActionRequest> pendingMenuRequests_;
	std::vector<SceneAudioRequest> pendingUiAudioRequests_;
	std::vector<SceneMenuPulse> deferredMenuPulses_;
	SceneMenuSource uiTransitionSource_;
	// 拒否された終端要求も、そのprepassでは再試行やCancelの既定Closeへ進めない。
	bool uiTerminalAttempted_ = false;
	uint64_t nextUiTransitionSerial_ = 1;
	SceneInstanceId uiLoadReservation_ = kInvalidSceneInstanceId;

	SceneAgentSystem agentSystem_;
	SceneAudioSystem audioSystem_;
	SceneAttackRunnerSystem attackRunnerSystem_;
	SceneAttachmentSystem attachmentSystem_;
	SceneCameraSystem cameraSystem_;
	SceneCombatSystem combatSystem_;
	SceneHealthSystem healthSystem_;
	SceneDeathResponseSystem deathSystem_;
	SceneDeathWorldClock deathWorldClock_;
	SceneDeathReplaySystem deathReplaySystem_;
	SceneReplayHistorySystem replayHistory_;
	SceneReplayRenderSystem replayRender_;
    SceneReplayGameFrame replayGameFrame_{};
    SceneReplayPresentationToken replayCompletion_{};
    SceneMenuSource replayMenuSource_{};
    SceneReplayPresentationToken replayMenuDrawToken_{};
    bool replayMenuPresented_ = false;
    bool replayPresentationAllowed_ = false;
    std::vector<SceneReplayViewSettings> replayViews_;
    std::array<Camera,3> replayCameras_;
    bool replayGameFrameReady_ = false;
	SceneReplayOwnerBasis replayOwnerBasis_;
	double replayRealSeconds_ = 0, replayNextDue_ = 0, replayInterval_ = 0;
	uint64_t replayUpdateSerial_ = 0, replayCapturedUpdate_ = 0, replayCapturedDraw_ = 0;
	bool replayBeginPending_ = false, replayFinalPending_ = false;
	SceneDeathStarted deathReplayStarted_{};
	double deathWorldElapsedSeconds_ = 0.0;
	uint64_t deathWorldInstance_ = 0, deathWorldGeneration_ = 0;
	std::unordered_set<uint64_t> deathWorldParticipants_;
	std::unordered_map<uint64_t, Vector3> deathWorldBaseVelocities_;
	std::vector<SceneDeathAction> pendingDeathActions_;
	uint64_t pendingDeathGeneration_ = 0, deathDocumentGeneration_ = 0;
	std::unordered_set<uint64_t> deathTextEntities_, deathTransformEntities_;
	SceneGameFlowResult heldDeathFlow_;
	SceneDebugSystem debugSystem_;
	SceneEnemySystem enemySystem_;
	SceneEnemySpawnerSystem enemySpawnerSystem_;
	SceneGameFlowSystem gameFlowSystem_;
	SceneEventSystem eventSystem_;
	SceneMenuSystem menuSystem_;
	SceneGamePauseSystem gamePauseSystem_;
	SceneHitReactionSystem hitReactionSystem_;
	SceneHitStopSystem hitStopSystem_;
	SceneEffectRenderSystem effectRenderSystem_;
	SceneEnvironmentSystem environmentSystem_;
	SceneLightingSystem lightingSystem_;
	SceneMonitorSystem monitorSystem_;
	SceneObjectSystem objectSystem_;
	SceneTransitionSystem transitionSystem_;
	SceneParticleSystem particleSystem_;
	ScenePostProcessProfileSystem postProcessProfileSystem_;
	ScenePhysicsSystem physicsSystem_;
	ScenePrefabAnimationSystem prefabAnimationSystem_;
	// Eventの開始/停止/Resetは次Frameで適用し、同期後のbase Pose表示を避ける。
	std::vector<ScenePresentationRequest> pendingPresentationRequests_;
	// Event要求は発行時のDocument内容だけへ適用する（0は未束縛）。
	uint64_t pendingPresentationDocumentGeneration_ = 0;
	SceneProjectileSystem projectileSystem_;
	SceneRuntimeEffectSystem runtimeEffectSystem_;
	SceneStatSystem statSystem_;
	SceneStateMachineSystem stateMachineSystem_;
	SceneTextMotionSystem textMotionSystem_;
	SceneTextRenderSystem textRenderSystem_;
	SceneUIRenderSystem uiRenderSystem_;
};

