// 役割: パーティクルエフェクトの保存形式とリソース読み込みを定義する。
#pragma once
#include <cstdint>
#include <string>

#include "ParticleCommon.h"
#include "ParticleEmitter.h"
#include "ParticleManager.h"
#include "../math/Vector3.h"

enum class ParticleSimulationType {
	kCPU,
	kGPU
};

// Particleそのものではなく、ParticleGroup + Emitter + Behavior の「レシピ」を保存するための構造体
struct ParticleEmitterDesc {
	Vector3 translate = { 0.0f, 0.0f, 0.0f };
	Vector3 spawnSize = { 1.0f, 1.0f, 1.0f };
	uint32_t count = 1;
	float frequency = 0.1f;
	bool isActive = true;
};

struct ParticleEffectDesc {
	std::string name = "newParticle";
	std::string textureFilePath = "resources/circle.png";
	ParticleSimulationType simulationType = ParticleSimulationType::kCPU;

	ParticleCommon::BlendMode blendMode = ParticleCommon::BlendMode::kBlendModeAdd;

	ParticleEmitterDesc emitter;
	ParticleManager::ParticleBehavior behavior;
	ParticleManager::LightningEmitterDesc lightning;
};

namespace ParticleEffectResource {

bool Load(const std::string& filePath, ParticleEffectDesc& outEffect);
bool Save(const std::string& filePath, const ParticleEffectDesc& effect);

// ParticleManager側にGroupを用意し、BlendModeを反映する
// clearParticlesがtrueなら、既存粒子だけ消してから使う
bool PrepareParticleGroup(const ParticleEffectDesc& effect, bool clearParticles = true);

// 既存Emitterに設定を流し込む
void ApplyToEmitter(ParticleEmitter& emitter, const ParticleEffectDesc& effect);

// new ParticleEmitterして返す。所有権は呼び出し側
ParticleEmitter* CreateEmitter(const ParticleEffectDesc& effect);

} // namespace ParticleEffectResource
