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
#include "../../../../../engine/scene/SceneEnemyChargeEvaluator.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <unordered_set>
#include <limits>
#include "../../../../../engine/scene/ScenePrefabAnimationEvaluator.h"

namespace {
	uint64_t MixIndividualSeed(uint64_t value) {
		value += 0x9e3779b97f4a7c15ULL;
		value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
		value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
		return value ^ (value >> 31);
	}
	float IndividualSigned(uint64_t seed, uint64_t channel) {
		return static_cast<float>(MixIndividualSeed(seed ^ channel) >> 40) / 16777215.0f * 2.0f - 1.0f;
	}
	// 検証済みの軸から目標回転を作る。補間時計と現在回転はEnemy側だけが所有する。
	Quaternion ChargeRotation(const Vector3& axis, float degrees) {
		const double length = std::sqrt(double(axis.x) * axis.x + double(axis.y) * axis.y + double(axis.z) * axis.z);
		if (!std::isfinite(length) || length <= 0 || !std::isfinite(degrees)) return MakeIdentityQuaternion();
		const double half = double(degrees) * 0.008726646259971648;
		const float factor = static_cast<float>(std::sin(half) / length);
		return {axis.x * factor, axis.y * factor, axis.z * factor, static_cast<float>(std::cos(half))};
	}
	// Healthと同じID優先・一意名・省略時は自身の規則。HPを読めないだけでは死亡とみなさない。
	bool IsTargetDead(const SceneDocument& document, const SceneEntity& target, const SceneStatSystem& stats) {
		const auto* health = SceneEntityQuery::FindEnabledComponent(target, "Health");
		if (!health) return false;
		const SceneEntity* source = nullptr;
		if (health->healthStatsEntityId) source = document.FindEntity(health->healthStatsEntityId);
		else if (health->healthStatsEntityName.empty()) source = &target;
		else for (const auto& entity : document.GetEntities()) if (entity.name == health->healthStatsEntityName) {
			if (source) return false;
			source = &entity;
		}
		if (!source) return false;
		if (stats.IsManagedDead(source->id, health->healthStatId)) return true;
		float value = 0, minimum = 0;
		return stats.TryGet(source->id, health->healthStatId, value, &minimum) && value <= minimum;
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

	// 新方式は自分の子HitBoxだけを所有する。壊れた明示IDを名前で付け替えて無関係なEntityを閉じない。
	SceneEntity* ResolveChargeHitBox(SceneDocument& document, uint64_t ownerId, const SceneComponent& behavior) {
		auto* hit=behavior.enemyAttackHitBoxEntityId?document.FindEntity(behavior.enemyAttackHitBoxEntityId):
			document.FindEntityByName(behavior.enemyAttackHitBoxEntityName);
		return hit && hit->id!=ownerId && document.IsDescendantOf(hit->id,ownerId) && SceneEntityQuery::FindEnabledComponent(*hit,"HitBox")?hit:nullptr;
	}

	void SetHitBoxActive(SceneEntity* hitBox, bool active) {
		if (hitBox) {
			hitBox->active = active;
		}
	}

	const char* PositioningConfigurationIssue(const SceneComponent& behavior) {
		if (!behavior.enemyPositioning.IsValid(behavior.enemyMoveSpeed) ||
			!std::isfinite(behavior.enemyDetectionRange) || behavior.enemyDetectionRange < 0 ||
			!std::isfinite(behavior.enemyLoseRange) || behavior.enemyLoseRange < behavior.enemyDetectionRange ||
			!std::isfinite(behavior.enemyTurnSpeed) || behavior.enemyTurnSpeed < 0) return "EnemyPositioning:Settings";
		return "";
	}

	SceneEnemyAttackWaitReason ApproachWaitReason(const SceneMovementProbeResult& approach, float limit) {
		switch (approach.issue) {
		case SceneMovementProbeIssue::OwnerShape: return SceneEnemyAttackWaitReason::OwnerShape;
		case SceneMovementProbeIssue::TargetShape: return SceneEnemyAttackWaitReason::TargetShape;
		case SceneMovementProbeIssue::InvalidShape: return SceneEnemyAttackWaitReason::InvalidShape;
		case SceneMovementProbeIssue::CollisionMask: return SceneEnemyAttackWaitReason::CollisionMask;
		case SceneMovementProbeIssue::NoHorizontalContact: return SceneEnemyAttackWaitReason::HorizontalPath;
		case SceneMovementProbeIssue::None: break;
		}
		if (approach.contactDistance > limit) return SceneEnemyAttackWaitReason::StartDistance;
		if (approach.blocked) return approach.wall ? SceneEnemyAttackWaitReason::FrontObstacle : SceneEnemyAttackWaitReason::FrontEnemy;
		return SceneEnemyAttackWaitReason::Ready;
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
	float deltaTime,
	uint64_t runtimePlayerEntityId
) {
	positioningRequests_.clear();
	if (++positioningFrameToken_ == 0) ++positioningFrameToken_;
	// 一時無効な個体も、前Frameの許可や遮蔽物を次の開始判定へ持ち越さない。
	for (auto& [id, runtime] : runtimes_) {
		runtime.approach = {}; runtime.approachValid = runtime.canStartAttack = false; runtime.blockerName.clear();
		runtime.individual.positioningIntent = SceneEnemyPositioningIntent::KeepSpacing;
		runtime.individual.noSafePositioningCandidate = false;
	}
	if (playSeed_ == 0) playSeed_ = MixIndividualSeed(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) | 1;
	std::unordered_set<uint64_t> requiredEntities;
	// Update内だけの値cache。同名の解決を共有し、自分の枝かどうかは敵ごとに検査する。
	std::unordered_map<std::string, SceneEnemyTargetResolution> namedTargets;
	std::unordered_map<uint64_t, bool> deadTargets;
	SceneComponent namedReference;
	namedReference.enemyTargetMode = SceneEnemyTargetMode::Entity;
	auto resolveTarget = [&](const SceneEntity& owner, const SceneComponent& behavior) {
		if (behavior.enemyTargetMode != SceneEnemyTargetMode::Name || behavior.enemyTargetEntityName.empty())
			return ResolveEnemyTarget(document, owner, behavior, runtimePlayerEntityId);
		auto found = namedTargets.find(behavior.enemyTargetEntityName);
		if (found == namedTargets.end()) {
			auto result = ResolveEnemyTarget(document, owner, behavior, runtimePlayerEntityId);
			found = namedTargets.emplace(behavior.enemyTargetEntityName, result).first;
		}
		if (!found->second.entityId) return found->second;
		namedReference.enemyTargetEntityId = found->second.entityId;
		namedReference.enemyMovementMode = behavior.enemyMovementMode;
		return ResolveEnemyTarget(document, owner, namedReference, runtimePlayerEntityId);
	};
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
			const auto* disabled = SceneEntityQuery::FindComponent(*binding.entity, "EnemyBehavior");
			if (disabled && disabled->enemyMovementMode == "PositioningCharge") {
				const auto found = runtimes_.find(binding.entity->id);
				SceneEnemyPositioningStatus status;
				if (found != runtimes_.end()) status = found->second.individual;
				status.phase = SceneEnemyPositioningPhase::Suppressed;
				positioningRequests_.push_back({binding.entity->id, 0, disabled->enemyPositioning, status, {}, 0.0f, false});
				StopHorizontalMovement(binding.body);
				SetHitBoxActive(ResolveChargeHitBox(document,binding.entityId,*disabled), false);
			}
			continue;
		}
		requiredEntities.insert(binding.entity->id);
		EnemyRuntime& runtime = runtimes_[binding.entity->id];
		if (runtime.contentGeneration && runtime.contentGeneration != document.GetContentGeneration()) runtime = {};
		runtime.contentGeneration = document.GetContentGeneration();
		runtime.positioning = behavior->enemyMovementMode == "PositioningCharge";
		runtime.movementSuppressed = false;
		SceneEntity* hitBox = runtime.positioning ? ResolveChargeHitBox(document,binding.entityId,*behavior) : ResolveAttackHitBox(document, *behavior);
		if (runtime.positioning) {
			const auto& settings = behavior->enemyPositioning;
			if (!runtime.individualInitialized) {
				const uint64_t generation = ++generations_[binding.entity->id];
				const uint64_t seed = MixIndividualSeed(binding.entity->id) ^ MixIndividualSeed(generation) ^
					MixIndividualSeed(settings.seed) ^ (settings.randomizeOnPlay ? playSeed_ : 0);
				runtime.individualSeed = seed;
				const float amount = settings.variation ? 1.0f : 0.0f;
				runtime.individual.distance = settings.distance + amount * settings.distanceVariation * IndividualSigned(seed, 1);
				runtime.individual.spacing = settings.spacing + amount * settings.spacingVariation * IndividualSigned(seed, 2);
				runtime.individual.speed = behavior->enemyMoveSpeed * (1.0f + amount * settings.speedVariation * IndividualSigned(seed, 3));
				runtime.individual.orbitSide = settings.variation && IndividualSigned(seed, 4) < 0.0f ? -1 : 1;
				runtime.individual.generation = generation;
				runtime.individualInitialized = true;
			}
			if (runtime.charge.phase != SceneEnemyChargePhase::Charge) SetHitBoxActive(hitBox, false);
			positioningRequests_.push_back({binding.entity->id, 0, settings, runtime.individual,
				runtime.positioningVelocity, runtime.orbitHold, false});
		}
		if (!runtime.initialized) {
			runtime.initialized = true;
			SetHitBoxActive(hitBox, false);
		}
		const uint64_t previousTarget = runtime.target.entityId;
		runtime.target = resolveTarget(*binding.entity, *behavior);
		auto* resolvedTarget = document.FindEntity(runtime.target.entityId);
		runtime.targetName = resolvedTarget ? resolvedTarget->name : std::string{};
		if (resolvedTarget && runtime.target.issue == SceneEnemyTargetIssue::None) {
			if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, *resolvedTarget)) runtime.target.issue = SceneEnemyTargetIssue::Inactive;
			else {
				auto [dead, inserted] = deadTargets.try_emplace(resolvedTarget->id, false);
				if (inserted) dead->second = IsTargetDead(document, *resolvedTarget, statSystem);
				if (dead->second) runtime.target.issue = SceneEnemyTargetIssue::Dead;
			}
			// 旧参照のID優先は維持する。突進は従来どおりPlayerのBodyを必要とする。
			if (runtime.positioning && runtime.target.issue == SceneEnemyTargetIssue::None &&
				!SceneEntityQuery::FindEnabledComponent(*resolvedTarget, "PlayerBehavior")) runtime.target.issue = SceneEnemyTargetIssue::UnsupportedPlayer;
		}
		const bool targetChanged = previousTarget && previousTarget != runtime.target.entityId;
		if (targetChanged || runtime.target.issue != SceneEnemyTargetIssue::None) {
			runtime.hasTarget = false;
			SetHitBoxActive(hitBox, false);
			if (runtime.positioning && runtime.charge.phase != SceneEnemyChargePhase::Idle) EndCharge(runtime.charge, SceneEnemyChargeEnd::TargetLost, true);
			runtime.charge.targetId = 0;
			if (!runtime.positioning) { runtime.phase = AttackPhase::None; runtime.phaseTimer = 0; }
		}
		runtime.targetInRange = runtime.hasTarget;
		runtime.targetRestricted = false;
		SceneEntity* target = runtime.target.issue == SceneEnemyTargetIssue::None && !targetChanged ? resolvedTarget : nullptr;
		if (runtime.positioning) {
			auto& request = positioningRequests_.back();
			const auto* reaction = SceneEntityQuery::FindEnabledComponent(*binding.entity, "HitReaction");
			const auto* state = stateMachineSystem.GetCurrentState(binding.entityId);
			const bool suppressed = statSystem.IsAtMin(binding.entityId, behavior->enemyHealthStatId) ||
				hitReactionSystem.IsKnockbackActive(binding.entityId) || (state && reaction && *state == reaction->hitReactionStateName);
			if (target && SceneEntityQuery::IsEntityActiveInHierarchy(document, *target)) {
				auto direction = Math::Subtract(SceneTransformResolver::ResolveScene3DTransform(document, *target).translate,
					SceneTransformResolver::ResolveScene3DTransform(document, *binding.entity).translate);
				direction.y = 0;
				const float distance = Math::Length(direction);
				if (!runtime.hasTarget && distance <= behavior->enemyDetectionRange) runtime.hasTarget = true;
				if (runtime.hasTarget && distance > behavior->enemyLoseRange) runtime.hasTarget = false;
				if (runtime.hasTarget) {
					request.targetId = target->id; runtime.charge.targetId = target->id;
					if (!suppressed && (runtime.charge.phase == SceneEnemyChargePhase::Idle || runtime.charge.phase == SceneEnemyChargePhase::Aim)) {
						auto desired = MakeQuaternionFromEuler({0, std::atan2(direction.x, direction.z), 0});
						if(const auto* parent=document.FindEntity(binding.entity->parentId)) {
							const auto parentTransform=SceneTransformResolver::ResolveScene3DTransform(document,*parent);
							const auto parentRotation=MakeRotateMatrix(parentTransform.useQuaternionRotation?parentTransform.quaternionRotate:MakeQuaternionFromEuler(parentTransform.rotate));
							desired=Normalize(MakeQuaternionFromRotationMatrix(Multiply(MakeRotateMatrix(desired),Inverse(parentRotation))));
						}
						auto& transform = binding.object->GetTransform();
						const auto current = transform.useQuaternionRotation ? transform.quaternionRotate : MakeQuaternionFromEuler(transform.rotate);
						transform.useQuaternionRotation = true;
						transform.quaternionRotate = Slerp(current, desired, std::clamp(1.0f - std::exp(-behavior->enemyTurnSpeed * PhysicsWorld::EffectiveStepDelta(deltaTime)), 0.0f, 1.0f));
						binding.entity->transform.rotate = transform.quaternionRotate; binding.object->Update();
					}
				}
			} else runtime.hasTarget = false;
			if (!runtime.hasTarget) {
				if (runtime.charge.phase != SceneEnemyChargePhase::Idle) EndCharge(runtime.charge, SceneEnemyChargeEnd::TargetLost, true);
				runtime.charge.targetId = 0; SetHitBoxActive(hitBox, false);
			}
			runtime.targetInRange = runtime.hasTarget;
			runtime.targetRestricted = suppressed;
			request.canMove = runtime.hasTarget && !suppressed && runtime.charge.phase == SceneEnemyChargePhase::Idle;
			if (!request.canMove) request.status.phase = SceneEnemyPositioningPhase::Suppressed;
			// Knockbackの速度は後段Reactionに任せ、攻撃中の自発速度だけ止める。
			StopHorizontalMovement(binding.body);
			continue;
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
			if (runtime.positioning) positioningRequests_.back().status.phase = SceneEnemyPositioningPhase::Suppressed;
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
			if (runtime.positioning) positioningRequests_.back().status.phase = SceneEnemyPositioningPhase::Suppressed;
			continue;
		}
		if (runtime.hitBoxSuppressedByReaction) {
			// Active中に被弾した攻撃は、残り時間だけKnockback後に再開する。
			SetHitBoxActive(hitBox, runtime.phase == AttackPhase::Active);
			runtime.hitBoxSuppressedByReaction = false;
		}

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
		runtime.targetInRange = runtime.hasTarget;
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
		// 一時無効化では個体値を失わない。Pool復元は明示ResetEntityが世代を進める。
		const auto* retained = document.FindEntity(iterator->first);
		if (!requiredEntities.contains(iterator->first) &&
			(!iterator->second.positioning || !retained || !SceneEntityQuery::HasComponent(*retained, "EnemyBehavior"))) {
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
	generations_.clear();
	positioningRequests_.clear();
	playSeed_ = 0;
	if (++positioningFrameToken_ == 0) ++positioningFrameToken_;
	chargeMotions_.clear(); chargeHitBoxes_.clear(); nextChargeSerial_ = 1;
}

void SceneEnemySystem::RefreshAttackApproach(const SceneDocument& document, uint64_t ownerId, EnemyRuntime& runtime,
	const SceneMovementProbeSnapshot* snapshot) {
	runtime.approach = {}; runtime.approachValid = runtime.canStartAttack = false; runtime.blockerName.clear();
	if (!snapshot || !positioningFrameToken_ || snapshot->frameToken != positioningFrameToken_ ||
		snapshot->contentGeneration != document.GetContentGeneration() || runtime.contentGeneration != document.GetContentGeneration()) return;
	runtime.approach = ScenePhysicsSystem::ProbeTargetApproach(*snapshot, ownerId, runtime.charge.targetId);
	runtime.approachValid = true;
	if (const auto* blocker = document.FindEntity(runtime.approach.blockerId)) runtime.blockerName = blocker->name;
}

void SceneEnemySystem::PrepareAttackApproaches(const SceneDocument& document,
	const SceneMovementProbeSnapshot& snapshot, float deltaTime) {
	const float dt = PhysicsWorld::EffectiveStepDelta(deltaTime);
	for (auto& request : positioningRequests_) {
		request.status.positioningIntent = SceneEnemyPositioningIntent::KeepSpacing;
		request.status.noSafePositioningCandidate = false;
		request.probeFrameToken = positioningFrameToken_;
		const auto found = runtimes_.find(request.entityId);
		if (found == runtimes_.end()) continue;
		auto& runtime = found->second;
		const auto* owner = document.FindEntity(request.entityId);
		const auto* behavior = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "EnemyBehavior") : nullptr;
		if (!behavior || !request.canMove || !std::isfinite(dt) || dt <= 0) continue;
		runtime.startDistanceLimit = behavior->enemyCharge.startDistance == 0 ? behavior->enemyCharge.distance : behavior->enemyCharge.startDistance;
		request.status.startDistanceLimit = runtime.startDistanceLimit;
		RefreshAttackApproach(document, request.entityId, runtime, &snapshot);
		request.status.contactDistanceValid = runtime.approachValid && runtime.approach.issue == SceneMovementProbeIssue::None;
		request.status.contactDistance = request.status.contactDistanceValid ? runtime.approach.contactDistance : 0.0f;
		// 初回設定は最終準備が所有する。ここでは次の機会を見積もるだけで待ち時間を消費しない。
		if (!runtime.charge.initialized || !runtime.charge.ready || !runtime.approachValid ||
			*PositioningConfigurationIssue(*behavior) || !behavior->enemyCharge.IsValid(behavior->enemyPositioning.variation) ||
			(std::max)(0.0f, runtime.charge.cooldown - dt) > 0 || runtime.approach.issue != SceneMovementProbeIssue::None) continue;
		if (runtime.approach.contactDistance > runtime.startDistanceLimit)
			request.status.positioningIntent = SceneEnemyPositioningIntent::ApproachForAttack;
		else if (runtime.approach.blocked)
			request.status.positioningIntent = SceneEnemyPositioningIntent::FindClearLane;
	}
}

void SceneEnemySystem::AcceptPositioningResults(const std::vector<SceneEnemyPositioningResult>& results) {
	for (const auto& result : results) {
		const auto found = runtimes_.find(result.entityId);
		if (found == runtimes_.end() || !found->second.positioning) continue;
		found->second.individual = result.status;
		found->second.positioningVelocity = result.velocity;
		found->second.orbitHold = result.orbitHold;
	}
}

bool SceneEnemySystem::TryGetPositioningStatus(uint64_t entityId, SceneEnemyPositioningStatus& out) const {
	const auto found = runtimes_.find(entityId);
	if (found == runtimes_.end()) return false;
	const auto& runtime = found->second;
	out = runtime.individual;
	out.positioningAvailable = runtime.positioning && runtime.individualInitialized;
	out.resolvedTargetEntityId = runtime.target.entityId;
	out.targetDisplayName = runtime.targetName;
	out.targetIssue = runtime.target.issue;
	out.configurationIssue = runtime.configurationIssue;
	const auto& charge = runtime.charge;
	if (runtime.positioning) {
		const auto geometryReason = runtime.approachValid ? ApproachWaitReason(runtime.approach, runtime.startDistanceLimit) : SceneEnemyAttackWaitReason::ProbePending;
		const bool shapeInvalid = runtime.approachValid && (runtime.approach.issue == SceneMovementProbeIssue::OwnerShape ||
			runtime.approach.issue == SceneMovementProbeIssue::TargetShape || runtime.approach.issue == SceneMovementProbeIssue::InvalidShape);
		out.attackWaitReason = !runtime.configurationIssue.empty() ? SceneEnemyAttackWaitReason::Configuration :
			shapeInvalid ? geometryReason :
			runtime.target.issue != SceneEnemyTargetIssue::None ? SceneEnemyAttackWaitReason::Target :
			(runtime.targetRestricted || runtime.movementSuppressed || charge.hitState || charge.reacting ||
				(charge.continuing && charge.phase == SceneEnemyChargePhase::Idle)) ? SceneEnemyAttackWaitReason::Suppressed :
			charge.phase != SceneEnemyChargePhase::Idle ? SceneEnemyAttackWaitReason::Attacking :
			!runtime.targetInRange ? SceneEnemyAttackWaitReason::Range :
			charge.cooldown > 0 ? SceneEnemyAttackWaitReason::Cooldown :
			geometryReason;
		out.canStartAttack = runtime.canStartAttack && out.attackWaitReason == SceneEnemyAttackWaitReason::Ready;
		out.contactDistanceValid = runtime.approachValid && runtime.approach.issue == SceneMovementProbeIssue::None;
		out.contactDistance = out.contactDistanceValid ? runtime.approach.contactDistance : 0.0f;
		out.startDistanceLimit = runtime.startDistanceLimit;
		out.attackWaitSeconds = charge.cooldown; out.firstAttackWait = charge.firstWait;
		out.blockerEntityId = runtime.approachValid ? runtime.approach.blockerId : 0;
		out.blockerDisplayName = runtime.blockerName;
	} else {
		out.attackWaitReason = !runtime.configurationIssue.empty() ? SceneEnemyAttackWaitReason::Configuration :
			runtime.target.issue != SceneEnemyTargetIssue::None ? SceneEnemyAttackWaitReason::Target :
			!runtime.targetInRange ? SceneEnemyAttackWaitReason::Range :
			(runtime.targetRestricted || runtime.movementSuppressed || charge.hitState || charge.reacting) ? SceneEnemyAttackWaitReason::Suppressed :
			runtime.cooldown > 0 ? SceneEnemyAttackWaitReason::Cooldown :
			runtime.phase != AttackPhase::None ? SceneEnemyAttackWaitReason::Attacking : SceneEnemyAttackWaitReason::Ready;
	}
	out.chargePhase = charge.phase; out.chargeEnd = charge.end; out.attackReady = charge.ready;
	out.accumulatedDamage = charge.damage; out.hitCount = charge.hitCount; out.achievedConditions = charge.achieved;
	return true;
}



void SceneEnemySystem::EndCharge(ChargeRuntime& c, SceneEnemyChargeEnd reason, bool restore) {
	if(c.phase!=SceneEnemyChargePhase::Idle)c.cooldown=c.cooldownDuration;
	c.end = reason; c.phase = SceneEnemyChargePhase::Idle; c.elapsed = 0;
	 c.restorePose = restore && c.ownsPose;
	// 集計の終端はCombatが時刻順に消費した後で確定する。
}

void SceneEnemySystem::SelectInterruptRule(const SceneEnemyInterruptSettings& settings, ChargeRuntime& c, SceneEnemyChargePhase currentPhase) {
	const int phase = static_cast<int>(currentPhase) - static_cast<int>(SceneEnemyChargePhase::Aim);
	const int scope = settings.enabled && phase >= 0 && phase < 3 && (settings.phases & (1 << phase))
		? (settings.overrideEnabled[phase] ? phase : -1) : -2;
	if (scope != c.ruleScope) {
		c.damage = 0; c.hitCount = 0; c.achieved = 0; c.idleSinceHit = 0; c.counted.clear(); c.ruleScope = scope;
	}
}

// 同時刻の境界では次の段階を選び、対象外/overrideへの出入りを命中がない区間でも適用する。
void SceneEnemySystem::AdvanceInterruptLedger(const SceneEnemyInterruptSettings& settings, ChargeRuntime& c, float time) {
	time=std::clamp(time,c.ledgerTime,1.0f);
	while(c.ledgerInterval<c.framePhases.size()) {
		const auto& interval=c.framePhases[c.ledgerInterval];
		SelectInterruptRule(settings,c,interval.phase);
		const float end=(std::min)(time,interval.end);
		c.idleSinceHit+=(std::max)(0.0f,end-c.ledgerTime)*c.frameDelta;c.ledgerTime=end;
		if(time<interval.end)return;
		++c.ledgerInterval;
	}
	SelectInterruptRule(settings,c,c.phase);
	c.idleSinceHit+=(std::max)(0.0f,time-c.ledgerTime)*c.frameDelta;c.ledgerTime=time;
}

void SceneEnemySystem::AdvanceChargeClock(SceneDocument& document, uint64_t ownerId, EnemyRuntime& runtime, float seconds) {
	auto& c = runtime.charge;
	auto* owner = document.FindEntity(ownerId);
	const auto* behavior = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "EnemyBehavior") : nullptr;
	if (!behavior) return;
	const auto& setting = behavior->enemyCharge;
	const bool nonlinear = setting.movementEasing != "Linear";
	for (unsigned transition = 0; transition < 8; ++transition) {
		if (c.phase == SceneEnemyChargePhase::Idle) break;
		if (c.continuing && (c.phase == SceneEnemyChargePhase::Aim || c.phase == SceneEnemyChargePhase::Locked)) break;
		if (c.phase == SceneEnemyChargePhase::Interrupted && (c.reacting || c.hitState)) {
			c.elapsed += seconds; c.cursor += seconds;
			if(c.frameDelta>0)c.framePhases.push_back({c.phase,std::clamp(c.cursor/c.frameDelta,0.0f,1.0f)});
			break;
		}
		float duration = 0;
		switch (c.phase) {
		case SceneEnemyChargePhase::Aim: duration = c.aim; break;
		case SceneEnemyChargePhase::Locked: duration = c.locked; break;
		case SceneEnemyChargePhase::Charge:
			if (nonlinear) {
				if (!SceneEnemyChargeEvaluator::TryGetDuration(setting, duration)) {
					runtime.configurationIssue = "EnemyCharge:Path";
					EndCharge(c, SceneEnemyChargeEnd::Invalid, true); SetHitBoxActive(document.FindEntity(c.hitBoxId), false); return;
				}
			} else duration = (std::max)(0.0f, setting.distance - c.travelled) / setting.speed;
			break;
		case SceneEnemyChargePhase::Recovery: duration = setting.recoverySeconds; break;
		case SceneEnemyChargePhase::Returning: duration = setting.returnSeconds; break;
		case SceneEnemyChargePhase::Interrupted: duration = c.reactionRemaining; break;
		default: break;
		}
		const float available = c.phase == SceneEnemyChargePhase::Charge && !nonlinear ? duration : (std::max)(0.0f, duration - c.elapsed);
		const float spent = (std::min)(seconds, available);
		if (c.phase == SceneEnemyChargePhase::Charge && spent > 0 && c.frameDelta > 0) {
			c.pendingSeconds = spent;
			c.pendingReachesEnd = nonlinear && spent >= available;
			c.pendingElapsed = c.pendingReachesEnd ? duration : c.elapsed + spent;
			SceneControlledMotion motion{ownerId, c.serial, c.targetId, c.hitBoxId, Math::Multiply(c.direction, setting.speed * spent),
				std::clamp(c.cursor / c.frameDelta, 0.0f, 1.0f), std::clamp((c.cursor + spent) / c.frameDelta, 0.0f, 1.0f)};
			if (nonlinear) {
				std::vector<SceneEnemyChargeEvaluator::DistanceSample> samples;
				if (!SceneEnemyChargeEvaluator::SampleInterval(setting, c.elapsed, c.pendingElapsed, samples)) {
					runtime.configurationIssue = "EnemyCharge:Path";
					EndCharge(c, SceneEnemyChargeEnd::Invalid, true); SetHitBoxActive(document.FindEntity(c.hitBoxId), false); return;
				}
				const float origin = samples.front().distance;
				motion.displacement = Math::Multiply(c.direction, samples.back().distance - origin);
				motion.samples.reserve(samples.size());
				for (const auto& sample : samples) {
					const float time = static_cast<float>((double(c.cursor) + double(sample.seconds) - c.elapsed) / c.frameDelta);
					motion.samples.push_back({time, Math::Multiply(c.direction, sample.distance - origin)});
				}
				// 要求と列の端点は同じ値を渡す。中間時刻の丸めによる重複はPhysicsのInvalid検査で停止する。
				motion.samples.front() = {motion.activeBegin, {}};
				motion.samples.back() = {motion.activeEnd, motion.displacement};
			}
			chargeMotions_.push_back(std::move(motion));
			SetHitBoxActive(document.FindEntity(c.hitBoxId), true);
			return; // Physicsが距離/停止時刻を確定してから残りの時計を進める。
		}
		c.elapsed += spent; c.cursor += spent; seconds -= spent;
		if(c.frameDelta>0)c.framePhases.push_back({c.phase,std::clamp(c.cursor/c.frameDelta,0.0f,1.0f)});
		const float rate = duration > 0 ? Math::SmoothStep(std::clamp(c.elapsed / duration, 0.0f, 1.0f)) : 1.0f;
		if (setting.poseEnabled && c.phase == SceneEnemyChargePhase::Aim) {
			c.rotation = Slerp(MakeIdentityQuaternion(), ChargeRotation(setting.axis, -setting.backAngle), rate);
			c.offset = Math::Multiply(setting.aimOffset, rate);
		}
		if (setting.poseEnabled && c.phase == SceneEnemyChargePhase::Returning) {
			c.rotation = Slerp(c.returnRotation, MakeIdentityQuaternion(), rate);
			c.offset = Math::Multiply(c.returnOffset, 1.0f - rate);
		}
		if (spent < available) break;
		c.elapsed = 0;
		if (c.phase == SceneEnemyChargePhase::Aim) {
			c.phase = SceneEnemyChargePhase::Locked;
			// 最後に描いたrootの向きが固定方向。狙い位置が動いても以後追尾しない。
			const auto world = SceneTransformResolver::ResolveScene3DTransform(document, *owner);
			const auto rotation = MakeRotateMatrix(world.useQuaternionRotation ? world.quaternionRotate : MakeQuaternionFromEuler(world.rotate));
			c.direction = {rotation.m[2][0], 0, rotation.m[2][2]};
			const float length=Math::Length(c.direction);
			if(!std::isfinite(length)||length<=0.0001f){EndCharge(c,SceneEnemyChargeEnd::Invalid,true);break;}
			c.direction = Math::Normalize(c.direction);
		} else if (c.phase == SceneEnemyChargePhase::Locked) c.phase = SceneEnemyChargePhase::Charge;
		else if (c.phase == SceneEnemyChargePhase::Charge) { c.phase = SceneEnemyChargePhase::Recovery; c.end = SceneEnemyChargeEnd::Distance; }
		else if (c.phase == SceneEnemyChargePhase::Recovery || c.phase == SceneEnemyChargePhase::Interrupted) {
			c.phase = SceneEnemyChargePhase::Returning; c.returnRotation = c.rotation; c.returnOffset = c.offset;
		} else if (c.phase == SceneEnemyChargePhase::Returning) {
			EndCharge(c, c.end, true); break;
		}
	}
}

void SceneEnemySystem::PrepareChargeFrame(SceneDocument& document, const std::vector<SceneRuntimeObjectBinding>& bindings,
	SceneStatSystem& stats, const ScenePrefabAnimationSystem& animation, const SceneHitReactionSystem& reaction,
	const SceneStateMachineSystem& states, float deltaTime, bool continuation, const SceneMovementProbeSnapshot* snapshot) {
	chargeMotions_.clear(); chargeHitBoxes_.clear();
	const float dt = PhysicsWorld::EffectiveStepDelta(deltaTime);
	for (auto& [id, runtime] : runtimes_) {
		if (!runtime.positioning) continue;
		auto& c = runtime.charge;
		runtime.canStartAttack = false;
		if (runtime.contentGeneration != document.GetContentGeneration() ||
			(c.contentGeneration && c.contentGeneration != document.GetContentGeneration())) {
			runtime = {}; // 内容世代が変われば、対象・診断も旧Documentから持ち越さない。
			continue;
		}
		c.interruptedThisFrame = false; c.frameDelta = dt; c.cursor = 0; c.continuing = continuation;
		c.framePhases.clear(); c.ledgerTime=0; c.ledgerInterval=0;
		auto* owner = document.FindEntity(id);
		const auto* behavior = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "EnemyBehavior") : nullptr;
		if (!owner || !behavior || behavior->enemyMovementMode!="PositioningCharge" || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *owner)) {
			runtime.target = {0, SceneEnemyTargetIssue::Inactive}; runtime.targetName.clear();
			runtime.hasTarget = runtime.targetInRange = false; runtime.targetRestricted = true;
			c.targetId = 0;
			SetHitBoxActive(document.FindEntity(c.hitBoxId), false); EndCharge(c, SceneEnemyChargeEnd::Invalid, true); continue;
		}
		runtime.startDistanceLimit = behavior->enemyCharge.startDistance == 0 ? behavior->enemyCharge.distance : behavior->enemyCharge.startDistance;
		c.contentGeneration = document.GetContentGeneration();
		auto* hit = ResolveChargeHitBox(document,id,*behavior);
		if (hit) { c.hitBoxId = hit->id; chargeHitBoxes_.insert(hit->id); }
		PhysicsBody* body = nullptr;
		for (const auto& binding : bindings) if (binding.entityId == id) body = binding.body;
		// Authoring値はPlay中固定。構成全探索は出現時だけ行い、毎Frameは借用先の生存条件だけ照合する。
		if (!c.initialized) {
			runtime.configurationIssue = ValidateEnemyChargeConfiguration(document, *owner, *behavior);
			c.configurationValid = runtime.configurationIssue.empty();
		}
		const auto* target=document.FindEntity(c.targetId);
		if (c.configurationValid) {
			runtime.configurationIssue.clear();
			if (const char* issue = PositioningConfigurationIssue(*behavior); *issue) runtime.configurationIssue = issue;
			else if (!body || body->type != PhysicsBodyType::Dynamic || body->freezePositionX || body->freezePositionZ) runtime.configurationIssue = "EnemyPositioning:Body";
			else if (behavior->enemyCharge.poseEnabled && !document.FindEntity(behavior->enemyCharge.poseEntityId)) runtime.configurationIssue = "EnemyCharge:Pose";
			else if (!hit) runtime.configurationIssue = "EnemyCharge:HitBox";
		}
		c.ready = c.configurationValid && runtime.configurationIssue.empty() && body && body->type == PhysicsBodyType::Dynamic && !body->freezePositionX && !body->freezePositionZ &&
			(!behavior->enemyCharge.poseEnabled || document.FindEntity(behavior->enemyCharge.poseEntityId)) && hit &&
			(!target || SceneEntityQuery::FindEnabledComponent(*target,"PlayerBehavior"));
		if (stats.IsAtMin(id, behavior->enemyHealthStatId)) {
			runtime.targetRestricted = true;
			SetHitBoxActive(hit, false); EndCharge(c, SceneEnemyChargeEnd::Interrupted, true); continue;
		}
		if (!c.initialized) {
			const auto& setting = behavior->enemyCharge;
			const float variation = behavior->enemyPositioning.variation ? 1.0f : 0.0f;
			c.aim = setting.aimSeconds + setting.aimVariation * variation * IndividualSigned(runtime.individualSeed, 5);
			c.locked = setting.lockedSeconds + setting.lockedVariation * variation * IndividualSigned(runtime.individualSeed, 6);
			c.cooldownDuration = setting.cooldownSeconds + setting.cooldownVariation * variation * IndividualSigned(runtime.individualSeed, 7);
			c.cooldown = setting.firstWaitSeconds + setting.firstWaitVariation * variation * IndividualSigned(runtime.individualSeed, 8);
			c.initialized = true;
		}
		const auto* hitReaction = SceneEntityQuery::FindEnabledComponent(*owner, "HitReaction");
		const auto* state = states.GetCurrentState(id);
		c.hitState = state && hitReaction && *state == hitReaction->hitReactionStateName;
		c.reacting = reaction.IsKnockbackActive(id);
		if ((c.hitState || c.reacting) && c.phase != SceneEnemyChargePhase::Idle && c.phase != SceneEnemyChargePhase::Interrupted && !continuation) {
			c.phase = SceneEnemyChargePhase::Interrupted; c.elapsed = 0; c.reactionRemaining = hitReaction ? hitReaction->hitReactionStateDuration : 0;
			c.end = SceneEnemyChargeEnd::Interrupted; SetHitBoxActive(hit, false);
		}
		if (!c.ready || (!runtime.hasTarget && !continuation)) {
			SetHitBoxActive(hit, false); EndCharge(c, c.ready ? SceneEnemyChargeEnd::TargetLost : SceneEnemyChargeEnd::Invalid, true); continue;
		}
		// 他のTransform演出が動的に所有した時点では、古い姿勢を後から上書きしない。
		if(c.ownsPose) {
			if(animation.HasPresentationTargetsIn(c.poseBranch)){runtime.configurationIssue="EnemyCharge:PoseConflict";c.ownsPose=c.restorePose=false;c.ready=false;EndCharge(c,SceneEnemyChargeEnd::Invalid,false);SetHitBoxActive(hit,false);continue;}
		}
		if (!std::isfinite(dt) || dt <= 0) continue;
		float remaining = dt;
		if (c.phase == SceneEnemyChargePhase::Idle) {
			if (continuation || c.hitState || c.reacting || runtime.targetRestricted) continue;
			// Update後のState等で対象が失われていないか、開始直前にも確認する。
			if (!target || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *target) || IsTargetDead(document, *target, stats)) {
				runtime.target.issue = !target ? SceneEnemyTargetIssue::Missing :
					!SceneEntityQuery::IsEntityActiveInHierarchy(document, *target) ? SceneEnemyTargetIssue::Inactive : SceneEnemyTargetIssue::Dead;
				runtime.hasTarget = runtime.targetInRange = false; runtime.approachValid = false; runtime.blockerName.clear();
				c.targetId = 0; continue;
			}
			const auto ownerPosition = SceneTransformResolver::ResolveScene3DTransform(document, *owner).translate;
			const auto targetPosition = SceneTransformResolver::ResolveScene3DTransform(document, *target).translate;
			const double targetDistance = std::hypot(double(targetPosition.x) - ownerPosition.x, double(targetPosition.z) - ownerPosition.z);
			if (!std::isfinite(targetDistance) || targetDistance > behavior->enemyLoseRange) {
				runtime.hasTarget = runtime.targetInRange = false; runtime.approachValid = false; runtime.blockerName.clear();
				c.targetId = 0; continue;
			}
			const float wait = (std::min)(remaining, c.cooldown); c.cooldown -= wait; remaining -= wait; c.cursor += wait;
			if(wait>0)c.framePhases.push_back({SceneEnemyChargePhase::Idle,std::clamp(c.cursor/dt,0.0f,1.0f)});
			// Agent用の結果を許可として使わず、入力/被弾制御後の同Frame形状で再判定する。
			RefreshAttackApproach(document, id, runtime, snapshot);
			if (c.cooldown > 0 || !runtime.approachValid ||
				ApproachWaitReason(runtime.approach, runtime.startDistanceLimit) != SceneEnemyAttackWaitReason::Ready) continue;
			runtime.canStartAttack = true;
			if (behavior->enemyCharge.poseEnabled) {
				auto* pose = document.FindEntity(behavior->enemyCharge.poseEntityId);
				std::unordered_set<uint64_t> branch;
				for (const auto& entity : document.GetEntities()) if (entity.id == pose->id || document.IsDescendantOf(entity.id, pose->id)) branch.insert(entity.id);
				if (animation.HasTransformTargetsIn(document, branch)) { runtime.configurationIssue = "EnemyCharge:PoseConflict"; c.ready = false; c.end = SceneEnemyChargeEnd::Invalid; continue; }
				c.poseBranch=std::move(branch);
				c.poseId = pose->id; c.base = pose->transform; c.ownsPose = true; c.restorePose = false;
			}
			c.serial = nextChargeSerial_++; c.travelled = c.elapsed = 0; c.offset = {};
			c.rotation = MakeIdentityQuaternion();
			c.phase = SceneEnemyChargePhase::Aim; c.end = SceneEnemyChargeEnd::None; c.firstWait = false;
			c.damage = 0; c.hitCount = 0; c.achieved = 0; c.counted.clear(); c.ruleScope = -2;
			if (auto* hitComponent = SceneEntityQuery::FindComponent(*hit, "HitBox")) {
				hitComponent->hitBoxAttackExecutionId = c.serial; ++hitComponent->hitBoxAttackWindowSerial;
			}
		}
		if (c.phase != SceneEnemyChargePhase::Interrupted) StopHorizontalMovement(body);
		AdvanceChargeClock(document, id, runtime, remaining);
	}
}

void SceneEnemySystem::AcceptChargePhysics(SceneDocument& document, const std::vector<SceneControlledMotionResult>& results) {
	for (const auto& result : results) {
		const auto found = runtimes_.find(result.ownerId); if (found == runtimes_.end()) continue;
		auto& runtime = found->second; auto& c = runtime.charge;
		if (c.serial != result.serial || c.phase != SceneEnemyChargePhase::Charge) continue;
		// 移動後は開始前の距離/許可を失効させる。後隙と再攻撃待ちが0でも再照会を待つ。
		runtime.approachValid = runtime.canStartAttack = false; runtime.blockerName.clear();
		const auto* owner = document.FindEntity(result.ownerId);
		const auto* behavior = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "EnemyBehavior") : nullptr;
		if (!behavior) continue;
		const auto& setting = behavior->enemyCharge;
		const float spent = std::clamp(result.fraction * c.frameDelta - c.cursor, 0.0f, c.pendingSeconds);
		// 無衝突時は要求の端点を採用し、fractionの丸めによる経過時刻の取りこぼしを防ぐ。
		const bool nonlinear = setting.movementEasing != "Linear";
		c.elapsed = nonlinear && !result.stopped ? c.pendingElapsed : c.elapsed + spent;
		c.cursor += spent; c.travelled += result.distance;
		if (result.invalid) runtime.configurationIssue = "EnemyCharge:Path";
		if(spent>0 && c.frameDelta>0)c.framePhases.push_back({SceneEnemyChargePhase::Charge,std::clamp(c.cursor/c.frameDelta,0.0f,1.0f)});
		if (setting.poseEnabled) {
			const float rate = setting.leanSeconds > 0 ? Math::SmoothStep(std::clamp(c.elapsed / setting.leanSeconds, 0.0f, 1.0f)) : 1.0f;
			c.rotation = Slerp(ChargeRotation(setting.axis, -setting.backAngle),
				ChargeRotation(setting.axis, setting.forwardAngle), rate);
			c.offset = Math::Add(Math::Multiply(setting.aimOffset, 1.0f - rate), Math::Multiply(setting.chargeOffset, rate));
		}
		if (result.stopped || (nonlinear ? c.pendingReachesEnd : c.travelled + 0.0001f >= setting.distance)) {
			c.phase = SceneEnemyChargePhase::Recovery; c.elapsed = 0;
			c.end = result.invalid ? SceneEnemyChargeEnd::Invalid : result.wall ? SceneEnemyChargeEnd::Wall :
				result.stopped ? (result.obstacleId == c.targetId ? SceneEnemyChargeEnd::Player : SceneEnemyChargeEnd::Enemy) : SceneEnemyChargeEnd::Distance;
			AdvanceChargeClock(document, result.ownerId, runtime, (std::max)(0.0f, c.frameDelta - c.cursor));
		}
	}
}

SceneHitReactionDecision SceneEnemySystem::EvaluateAcceptedHit(SceneDocument& document, const SceneCombatHitEvent& hit) {
	SceneHitReactionDecision decision{hit.acceptedHitSerial};
	const auto found = runtimes_.find(hit.targetEntityId);
	if (found == runtimes_.end() || !found->second.positioning || hit.contentGeneration != document.GetContentGeneration()) return decision;
	auto& c = found->second.charge;
	const auto* owner = document.FindEntity(hit.targetEntityId);
	const auto* behavior = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "EnemyBehavior") : nullptr;
	if (!behavior || !c.configurationValid || !behavior->enemyInterruption.enabled || hit.healthStatId!=behavior->enemyHealthStatId ||
		!hit.acceptedHitSerial || hit.acceptedHitSerial<=c.lastAcceptedHitSerial) return decision;
	c.lastAcceptedHitSerial=hit.acceptedHitSerial;
	if (c.interruptedThisFrame) { decision.policy = SceneHitReactionPolicy::Suppress; return decision; }
	AdvanceInterruptLedger(behavior->enemyInterruption,c,hit.candidateTime);
	if (c.ruleScope == -2 || hit.actualHpDamage <= 0 || !std::isfinite(hit.actualHpDamage)) return decision;
	decision.managePoiseRecovery = true;
	decision.policy = SceneHitReactionPolicy::Suppress;
	const auto& settings = behavior->enemyInterruption;
	if (settings.idleResetSeconds > 0 && c.idleSinceHit >= settings.idleResetSeconds) { c.damage = 0; c.hitCount = 0; c.achieved = 0; }
	c.idleSinceHit = 0;
	const auto key = ChargeRuntime::CountKey{hit.attackerEntityId, hit.attackExecutionId ? hit.attackExecutionId : hit.activationId,
		hit.controlledAttack, hit.attackExecutionId ? 0 : hit.hitBoxId,
		settings.countWindows ? hit.windowSerial : 0, settings.countWindows ? hit.reactionPriority : 0,hit.attackerGeneration};
	if (c.counted.insert(key).second && c.hitCount < (std::numeric_limits<uint32_t>::max)()) ++c.hitCount;
	c.damage = static_cast<float>((std::min)(double((std::numeric_limits<float>::max)()), double(c.damage) + hit.actualHpDamage));
	const auto& rule = c.ruleScope == -1 ? settings.common : settings.overrides[c.ruleScope];
	for (size_t i = 0; i < rule.conditions.size(); ++i) {
		const auto& row = rule.conditions[i]; bool achieved = false;
		switch (row.type) {
		case SceneEnemyInterruptConditionType::SingleHpDamage: achieved = hit.actualHpDamage >= row.threshold; break;
		case SceneEnemyInterruptConditionType::AccumulatedHpDamage: achieved = c.damage >= row.threshold; break;
		case SceneEnemyInterruptConditionType::HitCount: achieved = double(c.hitCount) >= row.threshold; break;
		case SceneEnemyInterruptConditionType::HorizontalKnockback: achieved = hit.knockback >= row.threshold; break;
		case SceneEnemyInterruptConditionType::VerticalKnockback: achieved = hit.verticalKnockback >= row.threshold; break;
		case SceneEnemyInterruptConditionType::PoiseDepleted: achieved = hit.poiseValid && hit.poiseDamage > 0 && hit.poiseAfter <= hit.poiseMinimum &&
			SceneEntityQuery::FindEnabledComponent(*owner,"HitReaction") && hit.poiseStatId==SceneEntityQuery::FindEnabledComponent(*owner,"HitReaction")->hitReactionPoiseStatId; break;
		}
		if (achieved) c.achieved |= static_cast<uint8_t>(1u << i);
	}
	const auto all = static_cast<uint8_t>((1u << rule.conditions.size()) - 1u);
	if (!(rule.all ? c.achieved == all : c.achieved != 0)) return decision;
	decision.policy = SceneHitReactionPolicy::Interrupt;
	c.interruptedThisFrame = true; c.phase = SceneEnemyChargePhase::Interrupted; c.end = SceneEnemyChargeEnd::Interrupted; c.elapsed = 0;
	const auto* reaction = SceneEntityQuery::FindEnabledComponent(*owner, "HitReaction");
	c.reactionRemaining = reaction ? (std::max)(0.0f, reaction->hitReactionStateDuration) : 0.0f;
	SetHitBoxActive(document.FindEntity(c.hitBoxId), false);
	return decision;
}

std::vector<uint64_t> SceneEnemySystem::FinishChargeCombat(SceneDocument& document, const SceneStatSystem& stats,
	const std::vector<SceneRuntimeObjectBinding>& bindings) {
	std::vector<uint64_t> changed;
	for(auto& [id,runtime]:runtimes_) {
		auto& c=runtime.charge; if(!runtime.positioning || c.contentGeneration!=document.GetContentGeneration())continue;
		const auto* owner=document.FindEntity(id); const auto* behavior=owner?SceneEntityQuery::FindEnabledComponent(*owner,"EnemyBehavior"):nullptr;
		const bool dead=behavior&&stats.IsAtMin(id,behavior->enemyHealthStatId);
		if(c.interruptedThisFrame || dead) for(const auto& binding:bindings)if(binding.entityId==id)StopHorizontalMovement(binding.body);
		if(dead || !behavior || !SceneEntityQuery::IsEntityActiveInHierarchy(document,*owner)) {
			runtime.targetRestricted = true;
			runtime.approachValid = runtime.canStartAttack = false; runtime.blockerName.clear();
			SetHitBoxActive(document.FindEntity(c.hitBoxId),false);EndCharge(c,dead?SceneEnemyChargeEnd::Interrupted:SceneEnemyChargeEnd::Invalid,true);
			if (c.ownsPose) {
				if (auto* pose = document.FindEntity(c.poseId)) { pose->transform = c.base; changed.push_back(pose->id); }
				c.ownsPose = c.restorePose = false;
			}
		}
		if(behavior) {
			if(c.interruptedThisFrame) {c.framePhases.clear();c.ledgerInterval=0;}
			AdvanceInterruptLedger(behavior->enemyInterruption,c,1.0f);
		}
	}
	return changed;
}

std::vector<uint64_t> SceneEnemySystem::ApplyChargePoses(SceneDocument& document) {
	std::vector<uint64_t> changed;
	for (auto& [id, runtime] : runtimes_) {
		auto& c = runtime.charge;
		if (!runtime.positioning || c.contentGeneration != document.GetContentGeneration()) continue;
		if (c.phase != SceneEnemyChargePhase::Charge) SetHitBoxActive(document.FindEntity(c.hitBoxId), false);
		if (!c.ownsPose) continue;
		auto* pose = document.FindEntity(c.poseId);
		const auto* owner = document.FindEntity(id);
		const auto* behavior = owner ? SceneEntityQuery::FindComponent(*owner, "EnemyBehavior") : nullptr;
		if (!pose || !c.ownsPose) continue;
		if (c.restorePose || !behavior) { pose->transform = c.base; c.ownsPose = c.restorePose = false; }
		else pose->transform = ScenePrefabAnimationEvaluator::EvaluateAttackPose(c.base, c.rotation,
			behavior->enemyCharge.pivot, c.offset);
		changed.push_back(pose->id);
	}
	return changed;
}
