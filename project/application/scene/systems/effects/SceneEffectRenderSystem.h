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

// LightningRendererのみを所有し、共有ParticleManagerとSpriteの描画順を調停する。
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
	void DrawScenePass(
		SceneDocument* document,
		Camera* camera,
		uint64_t skipEntityId,
		SceneEnvironmentSystem& environmentSystem,
		SceneObjectSystem& objectSystem
	);
	void DrawForegroundPass(
		SceneDocument* document,
		Camera* camera,
		uint64_t skipEntityId,
		SceneEnvironmentSystem& environmentSystem,
		SceneObjectSystem& objectSystem
	);
	void Finalize();

private:
	void DrawParticles(
		const SceneDocument* document,
		Camera* camera,
		bool foreground
	) const;

	std::unique_ptr<LightningRenderer> lightningRenderer_;
	std::unique_ptr<GroundCrackRenderer> groundCrackRenderer_;
	bool deferForegroundEffects_ = false;
};
