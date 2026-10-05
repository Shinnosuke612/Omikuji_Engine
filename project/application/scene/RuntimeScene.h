// 役割: SceneDocumentを実行時オブジェクトへ反映し、更新と描画を管理する。
#pragma once
#include "../../engine/scene/BaseScene.h"
#include "SceneRuntimeObjectBinding.h"
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
#include "systems/gameplay/combat/SceneHitReactionSystem.h"
#include "systems/gameplay/combat/SceneHitStopSystem.h"
#include "systems/gameplay/flow/SceneGameFlowSystem.h"
#include "systems/effects/SceneEffectRenderSystem.h"
#include "systems/rendering/SceneEnvironmentSystem.h"
#include "systems/rendering/SceneLightingSystem.h"
#include "systems/rendering/SceneMonitorSystem.h"
#include "systems/rendering/SceneObjectSystem.h"
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
	void UpdatePaused() override;
	void Draw() override;
	Camera* GetRenderCamera() const override;
	void DrawWithCamera(Camera* viewCamera) override;
	void DrawEnvironment(Camera* viewCamera) override;
	void BindLighting() override;
	void DrawSceneContent(Camera* viewCamera) override;
	void PrepareSceneContent(Camera* viewCamera) override;
	void DrawPreparedSceneContent(Camera* viewCamera) override;
	void DrawForegroundEffects() override;
	void DrawForegroundEffectsWithCamera(Camera* viewCamera) override;
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
	bool TryGetRuntimePostProcessSettings(
		ScenePostProcessSettings& settings,
		uint64_t& generation
	) const override;

private:
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

	SceneAgentSystem agentSystem_;
	SceneAudioSystem audioSystem_;
	SceneAttackRunnerSystem attackRunnerSystem_;
	SceneAttachmentSystem attachmentSystem_;
	SceneCameraSystem cameraSystem_;
	SceneCombatSystem combatSystem_;
	SceneDebugSystem debugSystem_;
	SceneEnemySystem enemySystem_;
	SceneEnemySpawnerSystem enemySpawnerSystem_;
	SceneGameFlowSystem gameFlowSystem_;
	SceneEventSystem eventSystem_;
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
	SceneProjectileSystem projectileSystem_;
	SceneRuntimeEffectSystem runtimeEffectSystem_;
	SceneStatSystem statSystem_;
	SceneStateMachineSystem stateMachineSystem_;
	SceneTextMotionSystem textMotionSystem_;
	SceneTextRenderSystem textRenderSystem_;
};

