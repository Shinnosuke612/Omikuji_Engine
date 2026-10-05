// 役割: Combat Hit EventをPoise被弾、ノックバック、死亡Presentationへ変換する。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "SceneCombatSystem.h"
#include "../../../../../engine/math/Vector3.h"

class SceneDocument;
class SceneStateMachineSystem;
class SceneStatSystem;
struct SceneRuntimeObjectBinding;

struct SceneDeathEffectRequest {
	uint64_t entityId = 0;
	std::string particleEffectPath;
};

class SceneHitReactionSystem {
public:
	// Stat同期後・Combat前に呼び、そのFrameのPoise Damageを上書きしない。
	void AdvanceRecoveries(SceneStatSystem& statSystem, float deltaTime);
	void Update(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		SceneStatSystem& statSystem,
		SceneStateMachineSystem& stateMachineSystem,
		const std::vector<SceneCombatHitEvent>& events,
		float deltaTime
	);
	// Enemy/Agent/StateMachineの速度決定後、Physics直前に呼び出す。
	void ApplyMotionOverrides(
		const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime
	);
	// Knockback中はEnemyBehaviorが攻撃・追跡を進めないために参照する。
	bool IsKnockbackActive(uint64_t entityId) const;
	std::vector<SceneDeathEffectRequest> ConsumeDeathEffectRequests();
	void ResetEntity(uint64_t entityId);
	void Clear();

private:
	struct KnockbackRuntime {
		Vector3 velocity{};
		float remainingTime = 0.0f;
		float duration = 0.0f;
	};
	struct PoiseRecoveryRuntime {
		std::string statId;
		float remainingTime = 0.0f;
	};

	std::unordered_map<uint64_t, float> pendingDeactivateTimes_;
	std::unordered_map<uint64_t, KnockbackRuntime> knockbackRuntimes_;
	std::unordered_map<uint64_t, PoiseRecoveryRuntime> poiseRecoveries_;
	std::vector<SceneDeathEffectRequest> deathEffectRequests_;
};
