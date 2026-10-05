// 役割: 物理押し戻しと分離されたTrigger衝突から戦闘ダメージを解決する。
#include "SceneCombatSystem.h"

#include "SceneStatSystem.h"
#include "../../../../../engine/collision/Collider.h"
#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../../engine/scene/SceneTransformResolver.h"
#include "../../../../../engine/math/Math.h"
#include "../../../../../engine/math/Quaternion.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {
	struct ComponentOwner {
		SceneEntity* entity = nullptr;
		const SceneComponent* component = nullptr;
	};

	ComponentOwner FindInAncestors(
		SceneDocument& document,
		SceneEntity& start,
		const char* componentType
	) {
		SceneEntity* current = &start;
		while (current) {
			if (const SceneComponent* component =
				SceneEntityQuery::FindEnabledComponent(*current, componentType)) {
				return { current, component };
			}
			current = current->parentId != 0
				? document.FindEntity(current->parentId)
				: nullptr;
		}
		return {};
	}

	SceneEntity* ResolveEntity(
		SceneDocument& document,
		uint64_t id,
		const std::string& name
	) {
		if (id != 0) {
			if (SceneEntity* entity = document.FindEntity(id)) {
				return entity;
			}
		}
		return name.empty() ? nullptr : document.FindEntityByName(name);
	}

	std::string ResolveFaction(SceneDocument& document, SceneEntity& entity) {
		const ComponentOwner faction = FindInAncestors(document, entity, "Faction");
		return faction.component ? faction.component->factionName : std::string{};
	}

	uint64_t MakeContactKey(
		uint64_t hitBoxId,
		uint64_t hurtBoxId,
		uint64_t attackWindowSerial
	) {
		uint64_t value = hitBoxId + 0x9E3779B97F4A7C15ull;
		value ^= hurtBoxId + 0x9E3779B97F4A7C15ull + (value << 6) + (value >> 2);
		value ^= attackWindowSerial + 0x9E3779B97F4A7C15ull + (value << 6) + (value >> 2);
		return value;
	}

	uint64_t MakeHitBoxWindowKey(uint64_t hitBoxId, uint64_t attackWindowSerial) {
		return hitBoxId ^ (attackWindowSerial + 0x9E3779B97F4A7C15ull +
			(hitBoxId << 6) + (hitBoxId >> 2));
	}
}

void SceneCombatSystem::Update(
	SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	SceneStatSystem& statSystem,
	float deltaTime
) {
	elapsedTime_ += (std::max)(deltaTime, 0.0f);
	struct CombatBinding {
		SceneEntity* entity = nullptr;
		Collider* collider = nullptr;
		const SceneComponent* component = nullptr;
	};
	std::vector<CombatBinding> hitBoxes;
	std::vector<CombatBinding> hurtBoxes;
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.collider ||
			!binding.collider->IsTrigger() ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, *binding.entity)
		) {
			continue;
		}
		if (const SceneComponent* hitBox =
			SceneEntityQuery::FindEnabledComponent(*binding.entity, "HitBox")) {
			hitBoxes.push_back({ binding.entity, binding.collider, hitBox });
		}
		if (const SceneComponent* hurtBox =
			SceneEntityQuery::FindEnabledComponent(*binding.entity, "HurtBox")) {
			hurtBoxes.push_back({ binding.entity, binding.collider, hurtBox });
		}
	}

	std::unordered_set<uint64_t> currentHitBoxWindows;
	for (const CombatBinding& hit : hitBoxes) {
		currentHitBoxWindows.insert(MakeHitBoxWindowKey(
			hit.entity->id, hit.component->hitBoxAttackWindowSerial
		));
	}
	std::unordered_set<uint64_t> removeProjectiles;
	for (const CombatBinding& hit : hitBoxes) {
		SceneEntity* explicitOwner = ResolveEntity(
			document,
			hit.component->hitBoxOwnerEntityId,
			hit.component->hitBoxOwnerEntityName
		);
		SceneEntity* hitOwner = explicitOwner;
		if (!hitOwner) {
			const ComponentOwner factionOwner =
				FindInAncestors(document, *hit.entity, "Faction");
			hitOwner = factionOwner.entity ? factionOwner.entity : hit.entity;
		}
		const std::string hitFaction = ResolveFaction(document, *hitOwner);

		for (const CombatBinding& hurt : hurtBoxes) {
			if (hit.entity == hurt.entity || hitOwner == hurt.entity) {
				continue;
			}
			SceneEntity* statsEntity = ResolveEntity(
				document,
				hurt.component->hurtBoxStatsEntityId,
				hurt.component->hurtBoxStatsEntityName
			);
			if (!statsEntity) {
				statsEntity = FindInAncestors(
					document,
					*hurt.entity,
					"StatSet"
				).entity;
			}
			if (!statsEntity || statsEntity == hitOwner) {
				continue;
			}
			if (hit.component->hitBoxIgnoreSameFaction) {
				const std::string hurtFaction = ResolveFaction(
					document,
					*statsEntity
				);
				if (
					!hitFaction.empty() &&
					!hurtFaction.empty() &&
					hitFaction == hurtFaction
				) {
					continue;
				}
			}
			if (!hit.collider->Intersects(*hurt.collider)) {
				continue;
			}

			const uint64_t contactKey = MakeContactKey(
				hit.entity->id,
				hurt.entity->id,
				hit.component->hitBoxAttackWindowSerial
			);
			const uint64_t hitBoxWindowKey = MakeHitBoxWindowKey(
				hit.entity->id, hit.component->hitBoxAttackWindowSerial
			);
			if (hit.component->hitBoxHitPolicy == "TargetCooldown") {
				const auto cooldown = targetCooldownContacts_.find(contactKey);
				if (
					cooldown != targetCooldownContacts_.end() &&
					elapsedTime_ < cooldown->second.nextHitTime
				) {
					continue;
				}
				targetCooldownContacts_[contactKey] = {
					hitBoxWindowKey,
					elapsedTime_ + (std::max)(hit.component->hitBoxTargetCooldown, 0.0f)
				};
			} else {
				if (consumedActivationContacts_.contains(contactKey)) {
					continue;
				}
				consumedActivationContacts_[contactKey] = hitBoxWindowKey;
			}
			const float damage = (std::max)(
				hit.component->hitBoxDamage *
					hurt.component->hurtBoxDamageMultiplier,
				0.0f
			);
			Vector3 knockbackDirection = Math::Subtract(
				SceneTransformResolver::ResolveScene3DTransform(
					document, *statsEntity
				).translate,
				SceneTransformResolver::ResolveScene3DTransform(
					document, *hitOwner
				).translate
			);
			knockbackDirection.y = 0.0f;
			const std::string& directionMode =
				hit.component->hitBoxKnockbackDirectionMode;
			if (directionMode != "RadialFromAttacker") {
				Vector3 local = hit.component->hitBoxKnockbackLocalDirection;
				local.y = 0.0f;
				if (directionMode == "AttackFacingLocal" || directionMode == "HitBoxLocal") {
					const SceneEntity& basisEntity = directionMode == "HitBoxLocal"
						? *hit.entity : *hitOwner;
					const Transform basis = SceneTransformResolver::ResolveScene3DTransform(
						document, basisEntity
					);
					const float yaw = basis.useQuaternionRotation
						? MakeEulerFromQuaternion(basis.quaternionRotate).y
						: basis.rotate.y;
					knockbackDirection = {
						local.x * std::cos(yaw) + local.z * std::sin(yaw),
						0.0f,
						-local.x * std::sin(yaw) + local.z * std::cos(yaw)
					};
				} else if (directionMode == "World") {
					knockbackDirection = local;
				}
			}
			knockbackDirection = Math::Length(knockbackDirection) > 0.0001f
				? Math::Normalize(knockbackDirection)
				: Vector3{ 0.0f, 0.0f, 1.0f };
			// Collider APIは接触点を返さないため、Effect用の暫定位置はHurtBox中心とする。
			const Vector3 hitPosition = SceneTransformResolver::ResolveScene3DTransform(
				document, *hurt.entity
			).translate;
			const Vector3 hitNormal = Math::Multiply(knockbackDirection, -1.0f);
			statSystem.Modify(
				statsEntity->id,
				hurt.component->hurtBoxHealthStatId.empty()
					? hit.component->hitBoxDamageStatId
					: hurt.component->hurtBoxHealthStatId,
				"Subtract",
				damage
			);
			hitEvents_.push_back({
				hitOwner->id,
				statsEntity->id,
				damage,
				hit.component->hitBoxPoiseDamage,
				hit.component->hitBoxKnockback,
				hit.component->hitBoxVerticalKnockback,
				knockbackDirection,
				(std::max)(hit.component->hitBoxHitStopDuration, 0.0f),
				hitPosition,
				hitNormal,
				hurt.component->hurtBoxHealthStatId.empty()
					? hit.component->hitBoxDamageStatId
					: hurt.component->hurtBoxHealthStatId,
				hit.component->hitBoxReactionTag,
				hit.component->hitBoxAttackExecutionId,
				hit.component->hitBoxReactionPriority
			});
			if (hit.component->hitBoxPoiseDamage > 0.0f) {
				statSystem.Modify(
					statsEntity->id,
					hit.component->hitBoxPoiseStatId,
					"Subtract",
					hit.component->hitBoxPoiseDamage
				);
			}
			const ComponentOwner projectile = FindInAncestors(
				document,
				*hit.entity,
				"Projectile"
			);
			if (
				projectile.entity &&
				projectile.component->projectileDestroyOnHit
			) {
				projectile.entity->active = false;
				removeProjectiles.insert(projectile.entity->id);
				break;
			}
		}
	}
	activeHitBoxWindows_ = std::move(currentHitBoxWindows);
	for (auto iterator = consumedActivationContacts_.begin();
		iterator != consumedActivationContacts_.end();) {
		if (!activeHitBoxWindows_.contains(iterator->second)) {
			iterator = consumedActivationContacts_.erase(iterator);
		} else {
			++iterator;
		}
	}
	for (auto iterator = targetCooldownContacts_.begin();
		iterator != targetCooldownContacts_.end();) {
		if (!activeHitBoxWindows_.contains(iterator->second.hitBoxWindowKey)) {
			iterator = targetCooldownContacts_.erase(iterator);
		} else {
			++iterator;
		}
	}

	for (uint64_t entityId : removeProjectiles) {
		pendingRemovals_.insert(entityId);
	}
}

std::vector<SceneCombatHitEvent> SceneCombatSystem::ConsumeHitEvents() {
	std::vector<SceneCombatHitEvent> result = std::move(hitEvents_);
	hitEvents_.clear();
	return result;
}

void SceneCombatSystem::FlushRemovals(SceneDocument& document) {
	for (uint64_t entityId : pendingRemovals_) {
		const SceneEntity* entity = document.FindEntity(entityId);
		const bool runtimeOnly = entity && entity->runtimeOnly;
		const bool wasDirty = document.IsDirty();
		document.RemoveEntity(entityId);
		if (runtimeOnly && !wasDirty) {
			document.MarkClean();
		}
	}
	pendingRemovals_.clear();
	hitEvents_.clear();
}

void SceneCombatSystem::Clear() {
	consumedActivationContacts_.clear();
	targetCooldownContacts_.clear();
	activeHitBoxWindows_.clear();
	pendingRemovals_.clear();
	elapsedTime_ = 0.0f;
}
