// 役割: 敵AIの判断を移動速度・Animation・HitBox有効時間へ変換する。
#include "SceneEnemySystem.h"

#include "../combat/SceneHitReactionSystem.h"
#include "../../animation/ScenePrefabAnimationSystem.h"
#include "../state/SceneStateMachineSystem.h"
#include "../combat/SceneStatSystem.h"
#include "../../../../../engine/3d/Object3d.h"
#include "../../../../../engine/math/Math.h"
#include "../../../../../engine/physics/PhysicsBody.h"
#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
	SceneEntity* ResolveEnemyTarget(
		SceneDocument& document,
		const SceneComponent& behavior
	) {
		if (behavior.enemyTargetEntityId != 0) {
			if (SceneEntity* entity = document.FindEntity(
				behavior.enemyTargetEntityId)) {
				return entity;
			}
		}
		return behavior.enemyTargetEntityName.empty()
			? nullptr
			: document.FindEntityByName(behavior.enemyTargetEntityName);
	}

	SceneEntity* ResolveAttackHitBox(
		SceneDocument& document,
		const SceneComponent& behavior
	) {
		if (behavior.enemyAttackHitBoxEntityId != 0) {
			if (SceneEntity* entity = document.FindEntity(
				behavior.enemyAttackHitBoxEntityId)) {
				return entity;
			}
		}
		return behavior.enemyAttackHitBoxEntityName.empty()
			? nullptr
			: document.FindEntityByName(behavior.enemyAttackHitBoxEntityName);
	}

	void SetHitBoxActive(SceneEntity* hitBox, bool active) {
		if (hitBox) {
			hitBox->active = active;
		}
	}

	void StopHorizontalMovement(PhysicsBody* body) {
		if (body) {
			body->velocity.x = 0.0f;
			body->velocity.z = 0.0f;
		}
	}
}

void SceneEnemySystem::Update(
	SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	SceneStatSystem& statSystem,
	ScenePrefabAnimationSystem& prefabAnimationSystem,
	const SceneHitReactionSystem& hitReactionSystem,
	const SceneStateMachineSystem& stateMachineSystem,
	float deltaTime
) {
	std::unordered_set<uint64_t> requiredEntities;
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.object ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, *binding.entity)
		) {
			continue;
		}
		const SceneComponent* behavior =
			SceneEntityQuery::FindEnabledComponent(*binding.entity, "EnemyBehavior");
		if (!behavior) {
			continue;
		}
		requiredEntities.insert(binding.entity->id);
		EnemyRuntime& runtime = runtimes_[binding.entity->id];
		runtime.movementSuppressed = false;
		SceneEntity* hitBox = ResolveAttackHitBox(document, *behavior);
		if (!runtime.initialized) {
			runtime.initialized = true;
			SetHitBoxActive(hitBox, false);
		}
		const SceneComponent* hitReaction =
			SceneEntityQuery::FindEnabledComponent(*binding.entity, "HitReaction");
		const SceneComponent* deathPresentation =
			SceneEntityQuery::FindEnabledComponent(*binding.entity, "DeathPresentation");
		const std::string* currentState =
			stateMachineSystem.GetCurrentState(binding.entity->id);
		const bool inHitState =
			currentState && hitReaction &&
			*currentState == hitReaction->hitReactionStateName;
		const bool inDeadState =
			currentState && deathPresentation &&
			*currentState == deathPresentation->deathPresentationStateName;
		if (
			inDeadState ||
			statSystem.IsAtMin(
				binding.entity->id,
				behavior->enemyHealthStatId
			)
		) {
			runtime.phase = AttackPhase::Dead;
			runtime.movementSuppressed = true;
			SetHitBoxActive(hitBox, false);
			StopHorizontalMovement(binding.body);
			continue;
		}
		if (
			inHitState ||
			hitReactionSystem.IsKnockbackActive(binding.entity->id)
		) {
			// 被弾中にAttack Phaseを進めると、吹き飛ばされながら攻撃判定だけが
			// 残る。移動はStateまたはReactionが所有するため、ここではAIと
			// 攻撃判定を止め、被弾終了後に同じPhaseから復帰させる。
			runtime.hitBoxSuppressedByReaction =
				runtime.phase == AttackPhase::Active;
			runtime.movementSuppressed = true;
			SetHitBoxActive(hitBox, false);
			if (inHitState) {
				StopHorizontalMovement(binding.body);
			}
			continue;
		}
		if (runtime.hitBoxSuppressedByReaction) {
			// Active中に被弾した攻撃は、残り時間だけKnockback後に再開する。
			SetHitBoxActive(hitBox, runtime.phase == AttackPhase::Active);
			runtime.hitBoxSuppressedByReaction = false;
		}
		SceneEntity* target = ResolveEnemyTarget(document, *behavior);
		if (!target || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *target)) {
			runtime.hasTarget = false;
			runtime.phase = AttackPhase::None;
			runtime.phaseTimer = 0.0f;
			SetHitBoxActive(hitBox, false);
			StopHorizontalMovement(binding.body);
			continue;
		}
		const Transform targetTransform =
			SceneTransformResolver::ResolveScene3DTransform(document, *target);
		Transform& transform = binding.object->GetTransform();
		Vector3 toTarget = Math::Subtract(
			targetTransform.translate,
			transform.translate
		);
		toTarget.y = 0.0f;
		const float distance = Math::Length(toTarget);
		if (!runtime.hasTarget && distance <= behavior->enemyDetectionRange) {
			runtime.hasTarget = true;
		} else if (runtime.hasTarget && distance > behavior->enemyLoseRange) {
			runtime.hasTarget = false;
		}
		if (!runtime.hasTarget) {
			runtime.phase = AttackPhase::None;
			runtime.phaseTimer = 0.0f;
			SetHitBoxActive(hitBox, false);
			StopHorizontalMovement(binding.body);
			continue;
		}

		const Vector3 direction = distance > 0.0001f
			? Math::Normalize(toTarget)
			: Vector3{ 0.0f, 0.0f, 1.0f };
		const float yaw = std::atan2(direction.x, direction.z);
		const Quaternion targetRotation =
			MakeQuaternionFromEuler({ 0.0f, yaw, 0.0f });
		const Quaternion currentRotation = transform.useQuaternionRotation
			? transform.quaternionRotate
			: MakeQuaternionFromEuler(transform.rotate);
		const float rotationAmount = std::clamp(
			1.0f - std::exp(-behavior->enemyTurnSpeed * deltaTime),
			0.0f,
			1.0f
		);
		transform.useQuaternionRotation = true;
		transform.quaternionRotate = Slerp(
			currentRotation,
			targetRotation,
			rotationAmount
		);

		runtime.cooldown = (std::max)(runtime.cooldown - deltaTime, 0.0f);
		if (runtime.phase == AttackPhase::Windup) {
			runtime.phaseTimer -= deltaTime;
			if (runtime.phaseTimer <= 0.0f) {
				runtime.phase = AttackPhase::Active;
				runtime.phaseTimer = (std::max)(
					behavior->enemyAttackActiveTime,
					0.0f
				);
				SetHitBoxActive(hitBox, true);
			}
		} else if (runtime.phase == AttackPhase::Active) {
			runtime.phaseTimer -= deltaTime;
			if (runtime.phaseTimer <= 0.0f) {
				runtime.phase = AttackPhase::Recovery;
				runtime.phaseTimer = (std::max)(
					behavior->enemyAttackRecovery,
					0.0f
				);
				SetHitBoxActive(hitBox, false);
			}
		} else if (runtime.phase == AttackPhase::Recovery) {
			runtime.phaseTimer -= deltaTime;
			if (runtime.phaseTimer <= 0.0f) {
				runtime.phase = AttackPhase::None;
				runtime.cooldown = (std::max)(
					behavior->enemyAttackCooldown,
					0.0f
				);
			}
		} else if (
			distance <= behavior->enemyAttackRange &&
			runtime.cooldown <= 0.0f
		) {
			runtime.phase = AttackPhase::Windup;
			runtime.phaseTimer = (std::max)(behavior->enemyAttackWindup, 0.0f);
			SetHitBoxActive(hitBox, false);
			binding.object->PlayAnimation(
				static_cast<size_t>((std::max)(behavior->enemyAttackAnimationClip, 0)),
				0.1f,
				true
			);
			if (!behavior->enemyAttackPrefabAnimationClip.empty()) {
				prefabAnimationSystem.Play(
					document,
					binding.entity->id,
					behavior->enemyAttackPrefabAnimationClip,
					true
				);
			}
		}

		if (runtime.phase == AttackPhase::None && distance > behavior->enemyAttackRange) {
			if (binding.body) {
				binding.body->velocity.x = direction.x * behavior->enemyMoveSpeed;
				binding.body->velocity.z = direction.z * behavior->enemyMoveSpeed;
			} else {
				transform.translate = Math::Add(
					transform.translate,
					Math::Multiply(
						direction,
						behavior->enemyMoveSpeed * deltaTime
					)
				);
			}
		} else if (binding.body) {
			binding.body->velocity.x = 0.0f;
			binding.body->velocity.z = 0.0f;
		}
		binding.object->Update();
		binding.entity->transform.translate = transform.translate;
		binding.entity->transform.scale = transform.scale;
		binding.entity->transform.rotate = transform.quaternionRotate;
	}

	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredEntities.contains(iterator->first)) {
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void SceneEnemySystem::ApplyMovementStops(
	const std::vector<SceneRuntimeObjectBinding>& bindings
) const {
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (!binding.entity || !binding.body) {
			continue;
		}
		const auto runtime = runtimes_.find(binding.entity->id);
		if (runtime == runtimes_.end() || !runtime->second.movementSuppressed) {
			continue;
		}
		StopHorizontalMovement(binding.body);
	}
}

void SceneEnemySystem::ResetEntity(uint64_t entityId) {
	runtimes_.erase(entityId);
}

void SceneEnemySystem::Clear() {
	runtimes_.clear();
}
