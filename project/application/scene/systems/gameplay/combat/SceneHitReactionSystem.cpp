// 役割: Poise被弾とDeathPresentationをCombat判定から分離して評価する。
#include "SceneHitReactionSystem.h"

#include "../state/SceneStateMachineSystem.h"
#include "SceneStatSystem.h"
#include "../../../SceneRuntimeObjectBinding.h"
#include "../../../../../engine/physics/PhysicsBody.h"
#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <utility>

namespace {
	struct ReactionKey {
		uint64_t targetEntityId = 0;
		uint64_t attackExecutionId = 0;

		bool operator==(const ReactionKey& other) const {
			return targetEntityId == other.targetEntityId &&
				attackExecutionId == other.attackExecutionId;
		}
	};

	struct ReactionKeyHash {
		size_t operator()(const ReactionKey& key) const {
			return std::hash<uint64_t>{}(key.targetEntityId) ^
				(std::hash<uint64_t>{}(key.attackExecutionId) << 1);
		}
	};
}

void SceneHitReactionSystem::AdvanceRecoveries(
	SceneStatSystem& statSystem,
	float deltaTime
) {
	for (auto iterator = poiseRecoveries_.begin();
		iterator != poiseRecoveries_.end();) {
		iterator->second.remainingTime -= (std::max)(deltaTime, 0.0f);
		if (iterator->second.remainingTime > 0.0f) {
			++iterator;
			continue;
		}
		statSystem.Modify(
			iterator->first,
			iterator->second.statId,
			"RestoreToMax",
			0.0f
		);
		iterator = poiseRecoveries_.erase(iterator);
	}
}

void SceneHitReactionSystem::Update(
	SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	SceneStatSystem& statSystem,
	SceneStateMachineSystem& stateMachineSystem,
	const std::vector<SceneCombatHitEvent>& events,
	float deltaTime
) {
	// Bodyへの書込みは次FrameのPhysics直前へ分離した。
	static_cast<void>(bindings);
	for (auto iterator = pendingDeactivateTimes_.begin();
		iterator != pendingDeactivateTimes_.end();) {
		iterator->second -= deltaTime;
		if (iterator->second > 0.0f) {
			++iterator;
			continue;
		}
		if (SceneEntity* entity = document.FindEntity(iterator->first)) {
			entity->active = false;
		}
		iterator = pendingDeactivateTimes_.erase(iterator);
	}
	std::unordered_map<ReactionKey, size_t, ReactionKeyHash> reactionWinners;
	for (size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex) {
		const SceneCombatHitEvent& event = events[eventIndex];
		if (event.attackExecutionId == 0) {
			continue;
		}
		const ReactionKey key{ event.targetEntityId, event.attackExecutionId };
		const auto found = reactionWinners.find(key);
		if (found == reactionWinners.end() ||
			event.reactionPriority < events[found->second].reactionPriority) {
			reactionWinners[key] = eventIndex;
		}
	}

	for (size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex) {
		const SceneCombatHitEvent& event = events[eventIndex];
		const ReactionKey reactionKey{ event.targetEntityId, event.attackExecutionId };
		const bool isReactionWinner = event.attackExecutionId == 0 ||
			reactionWinners.at(reactionKey) == eventIndex;
		SceneEntity* target = document.FindEntity(event.targetEntityId);
		if (!target || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *target)) {
			continue;
		}
		const SceneComponent* reaction =
			SceneEntityQuery::FindEnabledComponent(*target, "HitReaction");
		const SceneComponent* death =
			SceneEntityQuery::FindEnabledComponent(*target, "DeathPresentation");
		const bool hasStateMachine =
			SceneEntityQuery::FindEnabledComponent(*target, "StateMachine") != nullptr;
		if (isReactionWinner && reaction &&
			(event.knockback > 0.0f || event.verticalKnockback > 0.0f)) {
			const float amount = event.knockback *
				reaction->hitReactionKnockbackMultiplier;
			const float verticalAmount = event.verticalKnockback *
				reaction->hitReactionKnockbackMultiplier;
			if (verticalAmount > 0.0f) {
				for (const SceneRuntimeObjectBinding& binding : bindings) {
					if (
						binding.entity != target ||
						!binding.body ||
						binding.body->type != PhysicsBodyType::Dynamic ||
						binding.body->freezePositionY
					) {
						continue;
					}
					// YはこのHit確定時だけ設定する。以後の重力・接地はPhysicsが所有する。
					binding.body->velocity.y = (std::max)(
						binding.body->velocity.y,
						verticalAmount
					);
					break;
				}
			}
			const float duration = (std::max)(
				reaction->hitReactionStateDuration,
				0.0f
			);
			if (duration > 0.0f) {
				// CombatはPhysics後に評価される。ここでは次FrameのPhysics直前に
				// 適用する速度を予約し、EnemyBehaviorの速度書込みで消えないようにする。
				knockbackRuntimes_[target->id] = {
					{
						event.knockbackDirection.x * amount,
						0.0f,
						event.knockbackDirection.z * amount
					},
					duration,
					duration
				};
			}
		}
		if (death && statSystem.IsAtMin(target->id, event.healthStatId)) {
			const bool newlyDead = !pendingDeactivateTimes_.contains(target->id);
			if (!newlyDead) {
				continue;
			}
			poiseRecoveries_.erase(target->id);
			// StateMachine未設定のPrefabへ要求を残さず、演出なしの
			// Pool用敵でもDeactivateだけは確実に完了させる。
			if (hasStateMachine && !death->deathPresentationStateName.empty()) {
				stateMachineSystem.RequestState(
					target->id,
					death->deathPresentationStateName
				);
			}
			pendingDeactivateTimes_[target->id] =
				(std::max)(death->deathPresentationDeactivateDelay, 0.0f);
			if (!death->deathPresentationEffectPath.empty()) {
				deathEffectRequests_.push_back({
					target->id,
					death->deathPresentationEffectPath
				});
			}
		} else if (isReactionWinner && reaction) {
			bool shouldReact = false;
			if (reaction->hitReactionTriggerMode == "PoiseBreak") {
				const bool newlyBroken =
					event.poiseDamage > 0.0f &&
					statSystem.IsAtMin(
						target->id,
						reaction->hitReactionPoiseStatId
					) &&
					!poiseRecoveries_.contains(target->id);
				if (newlyBroken) {
					shouldReact = true;
					const float recoveryDelay = (std::max)(
						reaction->hitReactionPoiseRecoveryDelay,
						0.0f
					);
					if (recoveryDelay <= 0.0f) {
						statSystem.Modify(
							target->id,
							reaction->hitReactionPoiseStatId,
							"RestoreToMax",
							0.0f
						);
					} else {
						poiseRecoveries_[target->id] = {
							reaction->hitReactionPoiseStatId,
							recoveryDelay
						};
					}
				}
			} else {
				shouldReact = event.poiseDamage >=
					reaction->hitReactionMinimumPoiseDamage;
			}
			if (
				shouldReact &&
				hasStateMachine &&
				!reaction->hitReactionStateName.empty()
			) {
				stateMachineSystem.RequestState(
					target->id,
					reaction->hitReactionStateName
				);
			}
		}
	}
}

void SceneHitReactionSystem::ApplyMotionOverrides(
	const SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	float deltaTime
) {
	for (auto iterator = knockbackRuntimes_.begin();
		iterator != knockbackRuntimes_.end();) {
		const SceneEntity* entity = document.FindEntity(iterator->first);
		if (!entity || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity)) {
			iterator = knockbackRuntimes_.erase(iterator);
			continue;
		}

		PhysicsBody* body = nullptr;
		for (const SceneRuntimeObjectBinding& binding : bindings) {
			if (binding.entity == entity) {
				body = binding.body;
				break;
			}
		}
		if (!body) {
			iterator = knockbackRuntimes_.erase(iterator);
			continue;
		}

		const float duration = (std::max)(iterator->second.duration, 0.0001f);
		const float rate = std::clamp(
			iterator->second.remainingTime / duration,
			0.0f,
			1.0f
		);
		// この時点ではEnemy/Agent/StateMachineの速度決定が完了している。
		// 加算では追跡速度に打ち消されるため、被弾中のXZ移動はReactionが所有する。
		body->velocity.x = iterator->second.velocity.x * rate;
		body->velocity.z = iterator->second.velocity.z * rate;

		iterator->second.remainingTime -= (std::max)(deltaTime, 0.0f);
		if (iterator->second.remainingTime <= 0.0f) {
			iterator = knockbackRuntimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

bool SceneHitReactionSystem::IsKnockbackActive(uint64_t entityId) const {
	return knockbackRuntimes_.contains(entityId);
}

std::vector<SceneDeathEffectRequest>
SceneHitReactionSystem::ConsumeDeathEffectRequests() {
	return std::move(deathEffectRequests_);
}

void SceneHitReactionSystem::ResetEntity(uint64_t entityId) {
	pendingDeactivateTimes_.erase(entityId);
	knockbackRuntimes_.erase(entityId);
	poiseRecoveries_.erase(entityId);
	deathEffectRequests_.erase(
		std::remove_if(
			deathEffectRequests_.begin(), deathEffectRequests_.end(),
			[entityId](const SceneDeathEffectRequest& request) {
				return request.entityId == entityId;
			}
		),
		deathEffectRequests_.end()
	);
}

void SceneHitReactionSystem::Clear() {
	pendingDeactivateTimes_.clear();
	knockbackRuntimes_.clear();
	poiseRecoveries_.clear();
	deathEffectRequests_.clear();
}
