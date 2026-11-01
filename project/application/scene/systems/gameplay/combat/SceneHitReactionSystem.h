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
	// 受付serialごとの方針だけを消費する。SuppressでもPoise回復を維持し、HP/命中Eventは変更しない。
	void Update(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		SceneStatSystem& statSystem,
		SceneStateMachineSystem& stateMachineSystem,
		const std::vector<SceneCombatHitEvent>& events,
		float deltaTime,
		const std::vector<SceneHitReactionDecision>& decisions = {}
	);
	// Enemy/Agent/StateMachineの速度決定後、Physics直前に呼び出す。
	void ApplyMotionOverrides(
		const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime
	);
	// Knockback中はEnemyBehaviorが攻撃・追跡を進めないために参照する。
	bool IsKnockbackActive(uint64_t entityId) const;
	// 既存Knockbackだけを残す。終了ownerの速度解除はPhysics後にcallerが行う。
	void BeginDeathContinuation(const std::unordered_set<uint64_t>& participants);
	std::vector<uint64_t> ApplyDeathContinuationMotion(const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings, float worldDelta);
	void ClearDeathContinuation();
	std::vector<SceneDeathEffectRequest> ConsumeDeathEffectRequests();
	void ResetEntity(uint64_t entityId);
	void Clear();
	// 死亡したbranchの反応だけ破棄し、最終Poseは保持する。
	void DiscardBlocked(const std::unordered_set<uint64_t>& ids);

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
	std::unordered_set<uint64_t> deathParticipants_;
};
