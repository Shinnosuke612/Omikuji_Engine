// 役割: AttackSetからコピーした定義を評価し、Combatが読むHitBoxの実行時値だけを更新する。
#include "SceneAttackRunnerSystem.h"

#include "../../animation/ScenePrefabAnimationSystem.h"
#include "../../../SceneRuntimeObjectBinding.h"
#include "../../../../../engine/3d/Object3d.h"
#include "../../../../../engine/math/Math.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../../engine/scene/SceneTransformResolver.h"
#include "../../../../player/Player.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
	SceneComponent* FindHitBox(SceneEntity& entity) {
		return SceneEntityQuery::FindComponent(entity, "HitBox");
	}

	void ClampHalfSize(Vector3& halfSize) {
		halfSize.x = (std::max)(halfSize.x, 0.001f);
		halfSize.y = (std::max)(halfSize.y, 0.001f);
		halfSize.z = (std::max)(halfSize.z, 0.001f);
	}
}

bool SceneAttackRunnerSystem::Start(
	SceneDocument& document,
	uint64_t ownerEntityId,
	uint64_t attackSetEntityId,
	const std::string& attackName
) {
	if (ownerEntityId == 0 || attackSetEntityId == 0 || attackName.empty()) {
		return false;
	}
	SceneEntity* attackSetEntity = document.FindEntity(attackSetEntityId);
	const SceneComponent* attackSet = attackSetEntity
		? SceneEntityQuery::FindEnabledComponent(*attackSetEntity, "AttackSet")
		: nullptr;
	if (!attackSet) {
		return false;
	}
	const auto definition = std::find_if(
		attackSet->attackDefinitions.begin(), attackSet->attackDefinitions.end(),
		[&attackName](const SceneAttackDefinition& candidate) {
			return candidate.name == attackName;
		}
	);
	if (definition == attackSet->attackDefinitions.end()) {
		return false;
	}
	if (const auto existing = runtimes_.find(ownerEntityId); existing != runtimes_.end()) {
		DeactivateActiveWindows(document, existing->second);
		runtimes_.erase(existing);
	}
	Runtime runtime{};
	runtime.attackSetEntityId = attackSetEntityId;
	// 実行中にAuthoring Documentが再配置・編集されても攻撃時系列を変えない。
	runtime.definition = *definition;
	SceneEntity* animationTarget = ResolveEntity(
		document,
		runtime.definition.animationTargetEntityId,
		runtime.definition.animationTargetEntityName
	);
	runtime.animationTargetEntityId = animationTarget
		? animationTarget->id
		: attackSetEntityId;
	SceneEntity* facingTarget = ResolveEntity(
		document,
		runtime.definition.facingTargetEntityId,
		runtime.definition.facingTargetEntityName
	);
	runtime.facingTargetEntityId = facingTarget ? facingTarget->id : 0;
	runtime.attackExecutionId = nextAttackExecutionId_++;
	if (nextAttackExecutionId_ == 0) {
		nextAttackExecutionId_ = 1;
	}
	runtime.started = true;
	runtimes_[ownerEntityId] = std::move(runtime);
	return true;
}

float SceneAttackRunnerSystem::GetDuration(const SceneAttackDefinition& definition) {
	return (std::max)(definition.windup + definition.activeTime + definition.recovery, 0.0f);
}

SceneEntity* SceneAttackRunnerSystem::ResolveEntity(
	SceneDocument& document,
	uint64_t entityId,
	const std::string& entityName
) {
	if (entityId != 0) {
		if (SceneEntity* entity = document.FindEntity(entityId)) {
			return entity;
		}
	}
	return entityName.empty() ? nullptr : document.FindEntityByName(entityName);
}

void SceneAttackRunnerSystem::ResolvePlanarAxes(
	const SceneRuntimeObjectBinding* binding,
	Vector3& forward,
	Vector3& right
) {
	if (!binding || !binding->object) {
		forward = { 0.0f, 0.0f, 1.0f };
		right = { 1.0f, 0.0f, 0.0f };
		return;
	}
	const Matrix4x4& world = binding->object->GetWorldMatrix();
	forward = { world.m[2][0], 0.0f, world.m[2][2] };
	const float forwardLength = std::sqrt(forward.x * forward.x + forward.z * forward.z);
	if (forwardLength <= 0.0001f) {
		forward = { 0.0f, 0.0f, 1.0f };
	} else {
		forward = Math::Multiply(forward, 1.0f / forwardLength);
	}
	right = { forward.z, 0.0f, -forward.x };
}

void SceneAttackRunnerSystem::DeactivateWindow(
	SceneDocument& document,
	ActiveWindow& activeWindow
) {
	SceneEntity* entity = document.FindEntity(activeWindow.entityId);
	if (!entity) {
		return;
	}
	SceneComponent* hitBox = FindHitBox(*entity);
	if (activeWindow.usesLegacyPayload) {
		const HitBoxSnapshot& snapshot = activeWindow.legacySnapshot;
		entity->active = snapshot.active;
		if (hitBox) {
			hitBox->hitBoxDamage = snapshot.damage;
			hitBox->hitBoxPoiseDamage = snapshot.poiseDamage;
			hitBox->hitBoxKnockback = snapshot.knockback;
			hitBox->hitBoxVerticalKnockback = snapshot.verticalKnockback;
			hitBox->hitBoxHitStopDuration = snapshot.hitStopDuration;
			hitBox->hitBoxReactionTag = snapshot.reactionTag;
			hitBox->hitBoxKnockbackDirectionMode = snapshot.knockbackDirectionMode;
			hitBox->hitBoxKnockbackLocalDirection = snapshot.knockbackLocalDirection;
			hitBox->hitBoxHitPolicy = snapshot.hitPolicy;
			hitBox->hitBoxTargetCooldown = snapshot.targetCooldown;
		}
		if (snapshot.hasColliderHalfSizeSnapshot) {
			if (SceneComponent* collider = SceneEntityQuery::FindComponent(*entity, "OBBCollider");
				collider && collider->enabled && collider->colliderShape == "Box") {
				collider->colliderSizeMultiplier = snapshot.colliderHalfSize;
			}
		}
	} else {
		// 専用HitBoxは保存時inactiveが契約で、Payload／Collider値は変更しない。
		entity->active = false;
	}
	if (hitBox) {
		hitBox->hitBoxAttackExecutionId = 0;
		hitBox->hitBoxReactionPriority = 0xffffffffu;
	}
}

void SceneAttackRunnerSystem::DeactivateActiveWindows(
	SceneDocument& document,
	Runtime& runtime
) {
	for (ActiveWindow& activeWindow : runtime.activeWindows) {
		DeactivateWindow(document, activeWindow);
	}
	runtime.activeWindows.clear();
}

void SceneAttackRunnerSystem::UpdateHitWindows(
	SceneDocument& document,
	Runtime& runtime
) {
	struct DesiredWindow {
		size_t windowIndex = 0;
		SceneEntity* entity = nullptr;
		SceneComponent* hitBox = nullptr;
	};
	std::vector<DesiredWindow> desiredWindows;
	std::unordered_set<uint64_t> usedHitBoxEntities;
	for (size_t windowIndex = 0;
		windowIndex < runtime.definition.hitWindows.size();
		++windowIndex) {
		const SceneAttackHitWindow& window = runtime.definition.hitWindows[windowIndex];
		if (runtime.time < window.startTime || runtime.time >= window.endTime) {
			continue;
		}
		SceneEntity* entity = ResolveEntity(
			document, window.hitBoxEntityId, window.hitBoxEntityName
		);
		SceneComponent* hitBox = entity ? FindHitBox(*entity) : nullptr;
		if (!entity || !hitBox || !usedHitBoxEntities.insert(entity->id).second) {
			continue;
		}
		desiredWindows.push_back({ windowIndex, entity, hitBox });
	}

	auto remainsActive = [&desiredWindows](const ActiveWindow& activeWindow) {
		return std::any_of(
			desiredWindows.begin(), desiredWindows.end(),
			[&activeWindow](const DesiredWindow& desired) {
				return desired.windowIndex == activeWindow.windowIndex &&
					desired.entity->id == activeWindow.entityId;
			}
		);
	};
	for (auto iterator = runtime.activeWindows.begin();
		iterator != runtime.activeWindows.end();) {
		if (remainsActive(*iterator)) {
			++iterator;
			continue;
		}
		DeactivateWindow(document, *iterator);
		iterator = runtime.activeWindows.erase(iterator);
	}

	for (const DesiredWindow& desired : desiredWindows) {
		const bool alreadyActive = std::any_of(
			runtime.activeWindows.begin(), runtime.activeWindows.end(),
			[&desired](const ActiveWindow& activeWindow) {
				return activeWindow.windowIndex == desired.windowIndex &&
					activeWindow.entityId == desired.entity->id;
			}
		);
		if (alreadyActive) {
			continue;
		}
		const SceneAttackHitWindow& window =
			runtime.definition.hitWindows[desired.windowIndex];
		ActiveWindow activeWindow{};
		activeWindow.windowIndex = desired.windowIndex;
		activeWindow.entityId = desired.entity->id;
		activeWindow.usesLegacyPayload = window.payloadSource != "HitBox";
		if (activeWindow.usesLegacyPayload) {
			SceneComponent* collider = SceneEntityQuery::FindComponent(
				*desired.entity, "OBBCollider"
			);
			const bool overrideColliderHalfSize = window.overrideHitBoxHalfSize &&
				collider && collider->enabled && collider->colliderShape == "Box";
			activeWindow.legacySnapshot = {
				desired.entity->id,
				desired.entity->active,
				desired.hitBox->hitBoxDamage,
				desired.hitBox->hitBoxPoiseDamage,
				desired.hitBox->hitBoxKnockback,
				desired.hitBox->hitBoxVerticalKnockback,
				desired.hitBox->hitBoxHitStopDuration,
				desired.hitBox->hitBoxReactionTag,
				desired.hitBox->hitBoxKnockbackDirectionMode,
				desired.hitBox->hitBoxKnockbackLocalDirection,
				desired.hitBox->hitBoxHitPolicy,
				desired.hitBox->hitBoxTargetCooldown,
				overrideColliderHalfSize,
				overrideColliderHalfSize ? collider->colliderSizeMultiplier : Vector3{}
			};
			if (overrideColliderHalfSize) {
				Vector3 halfSize = window.hitBoxHalfSize;
				ClampHalfSize(halfSize);
				collider->colliderSizeMultiplier = halfSize;
			}
			desired.hitBox->hitBoxDamage = window.damage;
			desired.hitBox->hitBoxPoiseDamage = window.poiseDamage;
			desired.hitBox->hitBoxKnockback = window.knockback;
			desired.hitBox->hitBoxVerticalKnockback = window.verticalKnockback;
			desired.hitBox->hitBoxHitStopDuration = window.hitStopDuration;
			desired.hitBox->hitBoxReactionTag = window.reactionTag;
			desired.hitBox->hitBoxKnockbackDirectionMode = window.knockbackDirectionMode;
			desired.hitBox->hitBoxKnockbackLocalDirection = window.knockbackLocalDirection;
			desired.hitBox->hitBoxHitPolicy = window.hitPolicy;
			desired.hitBox->hitBoxTargetCooldown = window.targetCooldown;
		}
		desired.entity->active = true;
		++desired.hitBox->hitBoxAttackWindowSerial;
		desired.hitBox->hitBoxAttackExecutionId = runtime.attackExecutionId;
		desired.hitBox->hitBoxReactionPriority =
			static_cast<uint32_t>(desired.windowIndex);
		runtime.activeWindows.push_back(std::move(activeWindow));
	}
}

void SceneAttackRunnerSystem::Advance(
	SceneDocument& document,
	ScenePrefabAnimationSystem& prefabAnimationSystem,
	float deltaTime
) {
	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		const uint64_t ownerEntityId = iterator->first;
		Runtime& runtime = iterator->second;
		SceneEntity* owner = document.FindEntity(ownerEntityId);
		if (!owner || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *owner)) {
			DeactivateActiveWindows(document, runtime);
			iterator = runtimes_.erase(iterator);
			continue;
		}
		if (!runtime.animationStarted) {
			SceneEntity* animationTarget =
				document.FindEntity(runtime.animationTargetEntityId);
			if (animationTarget && !runtime.definition.animation.empty()) {
				prefabAnimationSystem.Play(
					document, animationTarget->id, runtime.definition.animation, true, 0.1f
				);
			}
			runtime.animationStarted = true;
		}
		runtime.previousTime = runtime.time;
		const float safeDeltaTime = (std::max)(deltaTime, 0.0f);
		runtime.elapsedTime += safeDeltaTime;
		runtime.time = (std::min)(
			runtime.time + safeDeltaTime,
			GetDuration(runtime.definition)
		);
		for (const SceneAttackEffectEvent& effect : runtime.definition.effectEvents) {
			const bool crossedEffectTime = effect.time <= 0.0f
				? runtime.previousTime <= 0.0f && runtime.time > 0.0f
				: runtime.previousTime < effect.time && effect.time <= runtime.time;
			if (
				!crossedEffectTime ||
				(effect.particleEffectPath.empty() && effect.groundEffectType == "None" && effect.groundPrefabPath.empty())
			) {
				continue;
			}
			SceneEntity* spawnEntity = ResolveEntity(
				document, effect.spawnEntityId, effect.spawnEntityName
			);
			effectRequests_.push_back({
				ownerEntityId,
				spawnEntity ? spawnEntity->id : runtime.attackSetEntityId,
				effect.particleEffectPath,
				effect.localOffset,
				effect.groundPrefabPath,
				effect.groundProbeDistance,
				effect.groundPrefabLifetime,
				effect.groundEffectType,
				effect.groundCrackRadius,
				effect.groundCrackPrimaryBranchCount,
				effect.groundCrackSegmentsPerBranch,
				effect.groundCrackBranchProbability,
				effect.groundCrackWidth,
				effect.groundCrackLifetime,
				effect.groundCrackSurfaceOffset
			});
		}
		UpdateHitWindows(document, runtime);
		if (runtime.time >= GetDuration(runtime.definition)) {
			DeactivateActiveWindows(document, runtime);
			const bool loopLimitReached = runtime.definition.loopMaxCount > 0 &&
				runtime.loopCount >= runtime.definition.loopMaxCount;
			const bool timeoutReached = runtime.definition.loopSafetyTimeout > 0.0f &&
				runtime.elapsedTime >= runtime.definition.loopSafetyTimeout;
			if (runtime.definition.loopEnabled && runtime.loopRequested &&
				!loopLimitReached && !timeoutReached) {
				runtime.time = 0.0f;
				runtime.previousTime = 0.0f;
				runtime.appliedMotionProgress = 0.0f;
				runtime.animationStarted = false;
				++runtime.loopCount;
				++iterator;
				continue;
			}
			runtime.finished = true;
		}
		++iterator;
	}
}

std::vector<SceneAttackEffectRequest>
SceneAttackRunnerSystem::ConsumeEffectRequests() {
	std::vector<SceneAttackEffectRequest> result = std::move(effectRequests_);
	effectRequests_.clear();
	return result;
}

void SceneAttackRunnerSystem::ApplyMotion(
	SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	Player* player,
	const Vector3& playerInputDirection,
	float deltaTime
) {
	if (deltaTime <= 0.0001f) {
		return;
	}
	for (auto& [ownerEntityId, runtime] : runtimes_) {
		if (!runtime.started || runtime.finished) {
			continue;
		}
		const auto binding = std::find_if(
			bindings.begin(), bindings.end(),
			[ownerEntityId](const SceneRuntimeObjectBinding& candidate) {
				return candidate.entity && candidate.entity->id == ownerEntityId;
			}
		);
		if (
			binding == bindings.end() ||
			!binding->entity ||
			!binding->object ||
			!binding->body
		) {
			continue;
		}
		if (!runtime.axesCaptured) {
			ResolvePlanarAxes(&*binding, runtime.forward, runtime.right);
			runtime.startForward = runtime.forward;
			runtime.axesCaptured = true;
		}
		const bool usesPlayerInputFacing =
			runtime.definition.facingMode == "InputDirection" &&
			player &&
			binding->object == player->GetObject() &&
			Math::Length(playerInputDirection) > 0.0001f;
		if (usesPlayerInputFacing) {
			runtime.forward = Math::Normalize(playerInputDirection);
			runtime.right = { runtime.forward.z, 0.0f, -runtime.forward.x };
			// Player::UpdateのCamera向き更新後に上書きし、攻撃中だけ入力方向を向く。
			binding->object->SetRotate({
				0.0f,
				std::atan2(runtime.forward.x, runtime.forward.z),
				0.0f
			});
		}
		if (runtime.definition.facingMode == "TargetDirection") {
			if (SceneEntity* target = document.FindEntity(runtime.facingTargetEntityId)) {
				Vector3 direction = Math::Subtract(
					SceneTransformResolver::ResolveScene3DTransform(document, *target).translate,
					SceneTransformResolver::ResolveScene3DTransform(document, *binding->entity).translate
				);
				direction.y = 0.0f;
				if (Math::Length(direction) > 0.0001f) {
					runtime.forward = Math::Normalize(direction);
					runtime.right = { runtime.forward.z, 0.0f, -runtime.forward.x };
					binding->object->SetRotate({
						0.0f,
						std::atan2(runtime.forward.x, runtime.forward.z),
						0.0f
					});
				}
			}
		} else if (runtime.definition.facingMode == "RotateByAngle") {
			const float duration = (std::max)(GetDuration(runtime.definition), 0.0001f);
			const float angle = runtime.definition.facingRotateAngle *
				std::clamp(runtime.time / duration, 0.0f, 1.0f);
			runtime.forward = {
				runtime.startForward.x * std::cos(angle) + runtime.startForward.z * std::sin(angle),
				0.0f,
				-runtime.startForward.x * std::sin(angle) + runtime.startForward.z * std::cos(angle)
			};
			runtime.right = { runtime.forward.z, 0.0f, -runtime.forward.x };
			binding->object->SetRotate({
				0.0f,
				std::atan2(runtime.forward.x, runtime.forward.z),
				0.0f
			});
		}
		const float activeDuration = (std::max)(runtime.definition.activeTime, 0.0001f);
		const float rawProgress = (runtime.time - runtime.definition.windup) / activeDuration;
		float progress = std::clamp(rawProgress, 0.0f, 1.0f);
		if (runtime.definition.motionEasing == "EaseOut") {
			progress = Math::EaseOutCubic(progress);
		} else if (runtime.definition.motionEasing == "EaseIn") {
			progress = progress * progress * progress;
		} else if (runtime.definition.motionEasing == "EaseInOut") {
			progress = progress < 0.5f
				? 4.0f * progress * progress * progress
				: 1.0f - std::pow(-2.0f * progress + 2.0f, 3.0f) * 0.5f;
		} else if (runtime.definition.motionEasing != "Linear") {
			progress = Math::SmoothStep(progress);
		}
		const float motionDelta = progress - runtime.appliedMotionProgress;
		runtime.appliedMotionProgress = progress;
		if (std::abs(motionDelta) <= 0.000001f) {
			continue;
		}
		const Vector3 displacement = Math::Add(
			Math::Multiply(runtime.forward, runtime.definition.forwardDistance * motionDelta),
			Math::Multiply(runtime.right, runtime.definition.sideDistance * motionDelta)
		);
		binding->body->velocity.x = displacement.x / deltaTime;
		binding->body->velocity.z = displacement.z / deltaTime;
	}
}

bool SceneAttackRunnerSystem::IsRunning(uint64_t ownerEntityId) const {
	const auto found = runtimes_.find(ownerEntityId);
	return found != runtimes_.end() && !found->second.finished;
}

bool SceneAttackRunnerSystem::IsFinished(uint64_t ownerEntityId) const {
	const auto found = runtimes_.find(ownerEntityId);
	return found != runtimes_.end() && found->second.finished;
}

float SceneAttackRunnerSystem::GetTime(uint64_t ownerEntityId) const {
	const auto found = runtimes_.find(ownerEntityId);
	return found == runtimes_.end() ? 0.0f : found->second.time;
}

float SceneAttackRunnerSystem::GetDuration(uint64_t ownerEntityId) const {
	const auto found = runtimes_.find(ownerEntityId);
	return found == runtimes_.end() ? 0.0f : GetDuration(found->second.definition);
}

void SceneAttackRunnerSystem::SetLoopRequested(
	uint64_t ownerEntityId,
	bool requested
) {
	if (const auto found = runtimes_.find(ownerEntityId); found != runtimes_.end()) {
		found->second.loopRequested = requested;
	}
}

void SceneAttackRunnerSystem::Stop(SceneDocument& document, uint64_t ownerEntityId) {
	const auto found = runtimes_.find(ownerEntityId);
	if (found == runtimes_.end()) {
		return;
	}
	DeactivateActiveWindows(document, found->second);
	runtimes_.erase(found);
}

void SceneAttackRunnerSystem::ResetEntity(SceneDocument& document, uint64_t entityId) {
	Stop(document, entityId);
}

void SceneAttackRunnerSystem::Clear(SceneDocument* document) {
	if (document) {
		for (auto& [ownerEntityId, runtime] : runtimes_) {
			DeactivateActiveWindows(*document, runtime);
		}
	}
	runtimes_.clear();
	effectRequests_.clear();
	nextAttackExecutionId_ = 1;
}
