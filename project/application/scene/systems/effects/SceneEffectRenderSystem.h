// 役割: SceneのLightningと水面前後に分かれるEffect描画を管理する。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "SceneRuntimeEffectSystem.h"

class Camera;
class DirectXCommon;
class LightningRenderer;
class GroundCrackRenderer;
class SceneDocument;
class SceneEnvironmentSystem;
class SceneObjectSystem;

// Lightning/GroundCrack Rendererを所有し、共有ParticleManagerとSpriteの描画順を調停する。
// deferForegroundEffects_は水面屈折の前後へEffectを分けるための一時的な描画契約。
class SceneEffectRenderSystem {
public:
	SceneEffectRenderSystem();
	~SceneEffectRenderSystem();

	void Initialize(DirectXCommon* dxCommon);
	void Update(float deltaTime);
	void SpawnGroundCracks(const std::vector<SceneGroundCrackSpawnRequest>& requests);
	void SetDeferForegroundEffects(bool defer) {
		deferForegroundEffects_ = defer;
	}
	// 保持条件は描画する各Viewから渡す。水面前後のPassでも同じ条件をGPU粒子へ届ける。
	void DrawScenePass(
		SceneDocument* document,
		Camera* camera,
		uint64_t skipEntityId,
		SceneEnvironmentSystem& environmentSystem,
		SceneObjectSystem& objectSystem,
		bool advanceGpuSimulation = true
	);
	void DrawForegroundPass(
		SceneDocument* document,
		Camera* camera,
		uint64_t skipEntityId,
		SceneEnvironmentSystem& environmentSystem,
		SceneObjectSystem& objectSystem,
		bool advanceGpuSimulation = true
	);
	void Finalize();

private:
	void DrawParticles(
		const SceneDocument* document,
		Camera* camera,
		bool foreground,
		bool advanceGpuSimulation
	) const;

	std::unique_ptr<LightningRenderer> lightningRenderer_;
	std::unique_ptr<GroundCrackRenderer> groundCrackRenderer_;
	bool deferForegroundEffects_ = false;
};
