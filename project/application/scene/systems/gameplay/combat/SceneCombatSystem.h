// 役割: HitBox/HurtBoxの重なりをHit Policyに従ってDamageへ変換する。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../SceneRuntimeObjectBinding.h"
#include "../../../../../engine/math/Vector3.h"

class SceneDocument;
class SceneStatSystem;

struct SceneCombatHitEvent {
	uint64_t attackerEntityId = 0;
	uint64_t targetEntityId = 0;
	float damage = 0.0f;
	float poiseDamage = 0.0f;
	float knockback = 0.0f;
	float verticalKnockback = 0.0f;
	Vector3 knockbackDirection{};
	float hitStopDuration = 0.0f;
	Vector3 hitPosition{};
	Vector3 hitNormal{};
	std::string healthStatId = "hp";
	std::string reactionTag;
	uint64_t attackExecutionId = 0;
	uint32_t reactionPriority = 0xffffffffu;
};

class SceneCombatSystem {
public:
	void Update(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		SceneStatSystem& statSystem,
		float deltaTime
	);
	void FlushRemovals(SceneDocument& document);
	std::vector<SceneCombatHitEvent> ConsumeHitEvents();
	void Clear();

private:
	struct TargetCooldownContact {
		uint64_t hitBoxWindowKey = 0;
		float nextHitTime = 0.0f;
	};
	std::unordered_map<uint64_t, uint64_t> consumedActivationContacts_;
	std::unordered_map<uint64_t, TargetCooldownContact> targetCooldownContacts_;
	std::unordered_set<uint64_t> activeHitBoxWindows_;
	std::unordered_set<uint64_t> pendingRemovals_;
	std::vector<SceneCombatHitEvent> hitEvents_;
	float elapsedTime_ = 0.0f;
};
