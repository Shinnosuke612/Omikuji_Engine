// 役割: Healthの被弾受付と、物理押し戻しから分離したCombat命中処理を実装する。
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
#include <set>

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
	SceneHealthSystem& healthSystem,
	SceneDeathResponseSystem& deathSystem,
	float deltaTime,
	const std::unordered_set<uint64_t>& blockedEntityIds,
	const std::vector<SceneEnemyContactRequest>& contacts,
	const std::unordered_set<uint64_t>& controlledHitBoxes,
	const std::function<void(const SceneCombatHitEvent&)>& onAcceptedHit
) {
	if (contentGeneration_ != document.GetContentGeneration()) {
		activations_.clear(); controlledConsumed_.clear(); spawnGenerations_.clear();
		contentGeneration_ = document.GetContentGeneration();
	}
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
			blockedEntityIds.contains(binding.entityId) ||
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
	std::set<std::pair<uint64_t, uint64_t>> currentActivations;
	for (const auto& hit : hitBoxes) {
		const auto key = std::make_pair(hit.entity->id, hit.component->hitBoxAttackWindowSerial);
		currentActivations.insert(key);
		if (!activations_.contains(key)) activations_[key] = ++activationSerial_;
	}
	std::erase_if(activations_, [&](const auto& entry) { return !currentActivations.contains(entry.first); });
	struct Candidate { const CombatBinding* hit; const CombatBinding* hurt; const SceneEnemyContactRequest* swept; };
	std::vector<Candidate> candidates;
	for (const auto& hit : hitBoxes) for (const auto& hurt : hurtBoxes) {
		if (!controlledHitBoxes.contains(hit.entity->id) && hit.collider->Intersects(*hurt.collider)) candidates.push_back({&hit, &hurt, nullptr});
	}
	for (const auto& contact : contacts) {
		if (contact.contentGeneration != document.GetContentGeneration() || !std::isfinite(contact.fraction) || contact.fraction < 0 || contact.fraction > 1) continue;
		for (const auto& hit : hitBoxes) if (hit.entity->id == contact.hitBoxId)
			for (const auto& hurt : hurtBoxes) if (hurt.entity->id == contact.hurtBoxId) candidates.push_back({&hit, &hurt, &contact});
	}
	std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
		return (a.swept ? a.swept->fraction : 1.0f) < (b.swept ? b.swept->fraction : 1.0f);
	});
	std::unordered_set<uint64_t> removeProjectiles;
	auto applyContact = [&](const Candidate& candidate) {
		const auto& hit = *candidate.hit; const auto& hurt = *candidate.hurt;
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, *hit.entity) ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, *hurt.entity)) return;
		if (candidate.swept && (hit.component->hitBoxAttackExecutionId != candidate.swept->serial ||
			controlledConsumed_[candidate.swept->ownerId] == candidate.swept->serial)) return;
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
		if (blockedEntityIds.contains(hitOwner->id) || deathSystem.IsBlocked(hitOwner->id) ||
			(candidate.swept && hitOwner->id!=candidate.swept->ownerId)) return;
		const std::string hitFaction = ResolveFaction(document, *hitOwner);

		if (hit.entity == hurt.entity || hitOwner == hurt.entity) {
			return;
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
		if (!statsEntity || statsEntity == hitOwner || blockedEntityIds.contains(statsEntity->id)) {
			return;
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
				return;
			}
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
				return;
			}

		} else {
			if (consumedActivationContacts_.contains(contactKey)) {
				return;
			}

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
		// 従来の重なりはHurtBox中心、Sweepは接触代表位置をEffectへ渡す。
		const Vector3 hitPosition = candidate.swept ? candidate.swept->position : SceneTransformResolver::ResolveScene3DTransform(
			document, *hurt.entity
		).translate;
		const Vector3 hitNormal = candidate.swept ? candidate.swept->normal : Math::Multiply(knockbackDirection, -1.0f);
		const std::string& hpId = hurt.component->hurtBoxHealthStatId.empty()
			? hit.component->hitBoxDamageStatId : hurt.component->hurtBoxHealthStatId;
		if (deathSystem.IsBlocked(hitOwner->id) || deathSystem.IsBlocked(hurt.entity->id)) return;
		float hpBefore = 0.0f, hpAfter = 0.0f;
		const bool hpValid = statSystem.TryGet(statsEntity->id, hpId, hpBefore);
		const uint64_t receiver = healthSystem.FindReceiver(document, statsEntity->id, hpId);
		if (receiver) {
			if (!healthSystem.ApplyDamage(document, statSystem, receiver, damage, blockedEntityIds)) return;
		} else {
			// Health未付与の既存Prefabは従来の減算/反応経路を維持する。
			statSystem.Modify(statsEntity->id, hpId, "Subtract", damage);
		}
		deathSystem.Observe(document, statSystem);
		const bool hpAfterValid = statSystem.TryGet(statsEntity->id, hpId, hpAfter);
		// 拒否された接触はHit Policyも弾の命中回数も消費しない。
		if (hit.component->hitBoxHitPolicy == "TargetCooldown") {
			targetCooldownContacts_[contactKey] = {hitBoxWindowKey,
				elapsedTime_ + (std::max)(hit.component->hitBoxTargetCooldown, 0.0f)};
		} else {
			consumedActivationContacts_[contactKey] = hitBoxWindowKey;
		}
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
		auto& fact = hitEvents_.back();
		fact.acceptedHitSerial = ++acceptedSerial_; fact.contentGeneration = contentGeneration_;
		fact.hitBoxId = hit.entity->id; fact.windowSerial = hit.component->hitBoxAttackWindowSerial;
		fact.activationId = activations_.at({fact.hitBoxId, fact.windowSerial});
		fact.controlledAttack = candidate.swept != nullptr;
		const auto spawn=spawnGenerations_.find(hitOwner->id);
		fact.attackerGeneration=spawn==spawnGenerations_.end()?0:spawn->second;
		fact.candidateTime = candidate.swept ? candidate.swept->fraction : 1.0f;
		fact.actualHpDamage = hpValid && hpAfterValid ? (std::max)(0.0f, hpBefore - hpAfter) : 0.0f;
		fact.poiseStatId = hit.component->hitBoxPoiseStatId;
		fact.poiseValid = statSystem.TryGet(statsEntity->id, hit.component->hitBoxPoiseStatId, fact.poiseBefore, &fact.poiseMinimum);
		if (hit.component->hitBoxPoiseDamage > 0.0f) {
			statSystem.Modify(
				statsEntity->id,
				hit.component->hitBoxPoiseStatId,
				"Subtract",
				hit.component->hitBoxPoiseDamage
			);
		}
		fact.poiseValid = statSystem.TryGet(statsEntity->id, hit.component->hitBoxPoiseStatId, fact.poiseAfter) && fact.poiseValid;
		if (candidate.swept) controlledConsumed_[candidate.swept->ownerId] = candidate.swept->serial;
		// 呼出側は配列追加/削除や再帰Combatを行わない。同じFrameの後続候補に中断を反映する。
		if (onAcceptedHit) onAcceptedHit(fact);
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
			return;
		}
	};
	for (const auto& candidate : candidates) applyContact(candidate);
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

void SceneCombatSystem::ResetEntity(uint64_t entityId) {
	++spawnGenerations_[entityId]; controlledConsumed_.erase(entityId);
	std::unordered_set<uint64_t> windows;
	for(const auto& [key,activation]:activations_) {
		(void)activation;
		if(key.first==entityId) windows.insert(MakeHitBoxWindowKey(key.first,key.second));
	}
	std::erase_if(activations_,[&](const auto& entry){return entry.first.first==entityId;});
	std::erase_if(consumedActivationContacts_,[&](const auto& entry){return windows.contains(entry.second);});
	std::erase_if(targetCooldownContacts_,[&](const auto& entry){return windows.contains(entry.second.hitBoxWindowKey);});
	for(uint64_t window:windows)activeHitBoxWindows_.erase(window);
}

void SceneCombatSystem::Clear() {
	activations_.clear(); spawnGenerations_.clear(); controlledConsumed_.clear(); contentGeneration_ = 0;
	acceptedSerial_ = activationSerial_ = 0;
	consumedActivationContacts_.clear();
	targetCooldownContacts_.clear();
	activeHitBoxWindows_.clear();
	pendingRemovals_.clear();
	elapsedTime_ = 0.0f;
}

namespace {
	// 明示IDの不整合や名前重複を自分/別Entityへのfallbackで隠さない。
	const SceneEntity* ResolveHealthStats(const SceneDocument& document, const SceneEntity& owner, const SceneComponent& health) {
		if (health.healthStatsEntityId) return document.FindEntity(health.healthStatsEntityId);
		if (health.healthStatsEntityName.empty()) return &owner;
		const SceneEntity* result = nullptr;
		for (const auto& candidate : document.GetEntities()) {
			if (candidate.name != health.healthStatsEntityName) continue;
			if (result) return nullptr;
			result = &candidate;
		}
		return result;
	}
}

uint64_t SceneHealthSystem::FindReceiver(const SceneDocument& document, uint64_t statsEntityId, const std::string& statId) const {
	for (const auto& entity : document.GetEntities()) {
		for (const auto& health : entity.components) {
			if (!health.enabled || health.type != "Health" || health.healthStatId != statId) continue;
			const auto* target = ResolveHealthStats(document, entity, health);
			if (target && target->id == statsEntityId) return entity.id;
		}
	}
	return 0;
}

void SceneHealthSystem::Advance(const SceneDocument& document, float gameplayDeltaTime,
	const std::unordered_set<uint64_t>& blockedEntityIds) {
	std::unordered_set<uint64_t> required;
	const float dt = std::isfinite(gameplayDeltaTime) ? (std::max)(gameplayDeltaTime, 0.0f) : 0.0f;
	for (const auto& owner : document.GetEntities()) {
		const auto* health = SceneEntityQuery::FindEnabledComponent(owner, "Health");
		if (!health || !SceneEntityQuery::IsEntityActiveInHierarchy(document, owner)) continue;
		const auto* target = ResolveHealthStats(document, owner, *health);
		if (!target || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *target) ||
			!SceneEntityQuery::FindEnabledComponent(*target, "StatSet")) continue;
		required.insert(owner.id);
		auto& state = runtimes_[owner.id];
		if (state.statsEntityId != target->id || state.statId != health->healthStatId)
			state = {target->id, health->healthStatId, 0.0f, 0};
		if (!blockedEntityIds.contains(owner.id) && !blockedEntityIds.contains(target->id))
			state.remaining = (std::max)(0.0f, state.remaining - dt);
	}
	for (auto it = runtimes_.begin(); it != runtimes_.end();) {
		if (!required.contains(it->first)) it = runtimes_.erase(it);
		else ++it;
	}
}

bool SceneHealthSystem::ApplyDamage(const SceneDocument& document, SceneStatSystem& stats,
	uint64_t receiverId, float amount, const std::unordered_set<uint64_t>& blockedEntityIds) {
	if (!std::isfinite(amount) || amount <= 0.0f || blockedEntityIds.contains(receiverId)) return false;
	const auto* owner = document.FindEntity(receiverId);
	if (!owner || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *owner)) return false;
	const auto* health = SceneEntityQuery::FindEnabledComponent(*owner, "Health");
	if (std::count_if(owner->components.begin(), owner->components.end(), [](const auto& c) { return c.enabled && c.type == "Health"; }) != 1) return false;
	if (!health || !std::isfinite(health->healthInvulnerabilityDuration) || health->healthInvulnerabilityDuration < 0.0f) return false;
	const auto* target = ResolveHealthStats(document, *owner, *health);
	if (!target || blockedEntityIds.contains(target->id) || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *target) ||
		!SceneEntityQuery::FindEnabledComponent(*target, "StatSet")) return false;
	// 同じHPを二つのHealthが所有する設定では、どちらの無敵を使うか選ばず拒否する。
	size_t owners = 0;
	for (const auto& entity : document.GetEntities()) {
		for (const auto& candidate : entity.components) {
			if (!candidate.enabled || candidate.type != "Health" || candidate.healthStatId != health->healthStatId) continue;
			const auto* candidateTarget = ResolveHealthStats(document, entity, candidate);
			if (candidateTarget && candidateTarget->id == target->id) ++owners;
		}
	}
	if (owners != 1) return false;
	float before = 0.0f, minimum = 0.0f, maximum = 0.0f;
	if (!stats.TryGet(target->id, health->healthStatId, before, &minimum, &maximum) ||
		!std::isfinite(before) || !std::isfinite(maximum) || minimum != 0.0f || maximum <= 0.0f || before <= 0.0f) return false;
	auto& state = runtimes_[receiverId];
	if (stats.IsManagedDead(target->id, health->healthStatId)) return false;
	if (state.statsEntityId != target->id || state.statId != health->healthStatId)
		state = {target->id, health->healthStatId, 0.0f, 0};
	if (state.remaining > 0.0f) return false;
	const float after = (std::max)(0.0f, before - amount);
	if (after >= before || !stats.Modify(target->id, health->healthStatId, "Set", after)) return false;
	// 次の攻撃候補/Actionより前に無敵を確定し、全攻撃者で共有する。
	state.remaining = health->healthInvulnerabilityDuration;
	state.revision = ++nextRevision_;
	return true;
}

uint64_t SceneHealthSystem::DamageRevision(uint64_t receiverId) const {
	const auto found = runtimes_.find(receiverId);
	return found == runtimes_.end() ? 0 : found->second.revision;
}

void SceneHealthSystem::ResetEntity(uint64_t entityId) {
	for (auto it = runtimes_.begin(); it != runtimes_.end();) {
		if (it->first == entityId || it->second.statsEntityId == entityId) it = runtimes_.erase(it);
		else ++it;
	}
}

void SceneHealthSystem::Clear() {
	runtimes_.clear();
	nextRevision_ = 0;
}

void SceneDeathResponseSystem::Sync(const SceneDocument& document, SceneStatSystem& stats, uint64_t instance) {
	if (instance_ != instance || generation_ != document.GetContentGeneration()) Clear();
	instance_ = instance; generation_ = document.GetContentGeneration();
	std::unordered_set<uint64_t> required;
	std::vector<SceneStatSystem::DeathBinding> managed;
	for (const auto& owner : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, owner)) continue;
		const auto* death = SceneEntityQuery::FindEnabledComponent(owner, "DeathResponse");
		const auto* health = SceneEntityQuery::FindEnabledComponent(owner, "Health");
		if (!death || !health) continue;
		const auto* target = ResolveHealthStats(document, owner, *health);
		bool flag = false; float hp = 0;
		if (!target || !stats.TryGetFlag(target->id, death->deathResponseFlagId, flag) ||
			!stats.TryGet(target->id, health->healthStatId, hp)) continue;
		required.insert(owner.id);
		// Play中の参照変更で死亡を解除しない。既存の登録をScene再開始まで保持。
		auto [it, added] = bindings_.try_emplace(owner.id, Binding{target->id, 0, health->healthStatId,
			death->deathResponseFlagId, death->deathResponseScope == "Scene", 0});
		const auto& b = it->second;
		managed.push_back({owner.id, b.stats, b.hp, b.flag});
	}
	std::erase_if(bindings_, [&](const auto& p) { return !required.contains(p.first); });
	stats.SetDeathBindings(std::move(managed));
	Observe(document, stats, true);
}
void SceneDeathResponseSystem::Observe(const SceneDocument& document, const SceneStatSystem& stats, bool refreshHierarchy) {
	// 大量Hitのたびに全Hierarchyを走査しない。登録同期時と新たな死亡時だけ再構築。
	if (!refreshHierarchy && observedStatRevision_ == stats.DeathRevision()) return;
	observedStatRevision_ = stats.DeathRevision();
	// Mapの走査順に依存しない。同じ境界での通知順はDocumentのEntity順。
	for (const auto& entity : document.GetEntities()) {
		auto it = bindings_.find(entity.id); if (it == bindings_.end()) continue;
		auto& b = it->second; bool dead = false;
		if (!stats.TryGetFlag(b.stats, b.flag, dead) || !dead) continue;
		if (!b.observed) {
			b.observed = ++serial_; b.status = 2;
			started_.push_back({instance_, generation_, entity.id, b.stats, b.observed, b.whole});
		}
		defeated_ |= b.whole;
	}
	blocked_.clear(); targetBlocked_.clear();
	if (!defeated_ && std::none_of(bindings_.begin(), bindings_.end(), [](const auto& p) { return p.second.observed != 0; })) return;
	for (const auto& entity : document.GetEntities()) {
		const auto* node = &entity;
		for (size_t n = 0; node && n <= document.GetEntities().size(); ++n) {
			auto b = bindings_.find(node->id);
			if (defeated_ || (b != bindings_.end() && b->second.observed)) blocked_.insert(entity.id);
			if (b != bindings_.end() && b->second.observed && !b->second.whole) targetBlocked_.insert(entity.id);
			node = document.FindEntity(node->parentId);
		}
	}
}
std::vector<SceneDeathStarted> SceneDeathResponseSystem::ConsumeStarted() {
	auto result = std::move(started_); started_.clear(); return result;
}
int SceneDeathResponseSystem::Status(uint64_t id) const {
	auto b = bindings_.find(id); return b == bindings_.end() ? 0 : b->second.status;
}
void SceneDeathResponseSystem::SetStatus(uint64_t id, int status) {
	auto b = bindings_.find(id); if (b != bindings_.end()) b->second.status = status;
}
void SceneDeathResponseSystem::ResetEntity(uint64_t id) {
	std::erase_if(bindings_, [id](const auto& b) { return b.first == id || b.second.stats == id; });
	std::erase_if(started_, [id](const auto& b) { return b.owner == id || b.stats == id; });
	blocked_.erase(id); targetBlocked_.erase(id);
	// 全体敗北はPool復元では解除しない。Scene再開始だけが解除する。
}
void SceneDeathResponseSystem::Clear() {
	bindings_.clear(); blocked_.clear(); targetBlocked_.clear(); started_.clear(); instance_ = generation_ = serial_ = observedStatRevision_ = 0; defeated_ = false;
}

// World継続の時計だけを更新する。HP/死亡通知/描画の所有は移さない。
bool SceneDeathWorldClock::Begin(uint64_t instance, uint64_t generation,
	uint64_t owner, uint64_t serial, float scale, float duration) {
	if (phase_ != SceneDeathWorldPhase::Normal || instance == 0 || generation == 0 ||
		owner == 0 || serial == 0 || !std::isfinite(scale) || scale <= 0.0f || scale > 1.0f ||
		!std::isfinite(duration) || duration <= 0.0f) {
		return false;
	}
	instance_ = instance;
	generation_ = generation;
	owner_ = owner;
	serial_ = serial;
	scale_ = scale;
	remaining_ = duration;
	phase_ = SceneDeathWorldPhase::LiveSlow;
	return true;
}

SceneDeathWorldStep SceneDeathWorldClock::Advance(float realDelta) {
	if (phase_ != SceneDeathWorldPhase::LiveSlow || !std::isfinite(realDelta) || realDelta <= 0.0f) {
		return {};
	}
	const float used = (std::min)(realDelta, remaining_);
	SceneDeathWorldStep step{used * scale_, used, false};
	remaining_ -= used;
	if (remaining_ <= 0.0f) {
		phase_ = SceneDeathWorldPhase::Frozen;
		step.freezeAfterStep = true;
	}
	return step;
}

void SceneDeathWorldClock::Freeze() {
	remaining_ = 0.0f;
	phase_ = SceneDeathWorldPhase::Frozen;
}

void SceneDeathWorldClock::Clear() {
	instance_ = generation_ = owner_ = serial_ = 0;
	scale_ = remaining_ = 0.0f;
	phase_ = SceneDeathWorldPhase::Normal;
}

void SceneDeathReplaySystem::AcceptStart(const SceneDeathStarted& pulse, float duration) {
	if (started_.serial || !pulse.serial || !pulse.wholeScene || !std::isfinite(duration) || duration <= 0.0f) return;
	started_ = pulse;
	status_ = { SceneDeathReplayPhase::LiveSlow, pulse.owner, duration, {} };
}
void SceneDeathReplaySystem::FailStart(const SceneDeathStarted& pulse, const std::string& diagnostic) {
	if (!pulse.serial || (started_.serial && (started_.serial != pulse.serial || started_.instance != pulse.instance ||
		started_.generation != pulse.generation || started_.owner != pulse.owner))) return;
	completionPending_ = endpointPresented_ = false;
	started_ = pulse;
	status_ = { SceneDeathReplayPhase::Failed, pulse.owner, 0.0f, diagnostic };
}
void SceneDeathReplaySystem::AdvancePresentation(const SceneDeathWorldStep& step) {
	if (status_.phase == SceneDeathReplayPhase::LiveSlow && std::isfinite(step.elapsedRealDelta) && step.elapsedRealDelta > 0.0f)
		status_.remainingRealSeconds = (std::max)(0.0f, status_.remainingRealSeconds - step.elapsedRealDelta);
}
void SceneDeathReplaySystem::FinishLiveSlow() {
	if (status_.phase == SceneDeathReplayPhase::LiveSlow) {
		status_.phase = SceneDeathReplayPhase::Frozen;
		status_.remainingRealSeconds = 0.0f;
	}
}
bool SceneDeathReplaySystem::BeginReplay(double seconds, uint64_t samples, float speed) {
    if (status_.phase != SceneDeathReplayPhase::Frozen || !samples || !std::isfinite(seconds) || seconds < 0 ||
        !std::isfinite(speed) || speed <= 0 || (samples > 1 && seconds <= 0)) return false;
    status_.phase = SceneDeathReplayPhase::ReplayA;
    status_.staticClip = samples == 1;
    status_.replayDuration = status_.staticClip ? 1.0 : seconds;
    status_.replaySeconds = 0;
    status_.cut = 0;
    status_.awaitingPresentation = true;
    replaySpeed_ = speed;
    poseSerial_ = 1;
    acceptedDrawSerial_ = 0;
    endpointPresented_ = completionPending_ = false;
    return true;
}
bool SceneDeathReplaySystem::IsReplaying() const {
    return status_.phase == SceneDeathReplayPhase::ReplayA || status_.phase == SceneDeathReplayPhase::ReplayB ||
        status_.phase == SceneDeathReplayPhase::ReplayC || status_.phase == SceneDeathReplayPhase::FinishedHold;
}
double SceneDeathReplaySystem::ReplaySampleSeconds() const {
    return status_.staticClip ? 0.0 : status_.replaySeconds;
}
void SceneDeathReplaySystem::AdvanceReplay(float delta, bool allowed) {
    if (!allowed || !IsReplaying() || status_.phase == SceneDeathReplayPhase::FinishedHold ||
        !std::isfinite(delta) || delta < 0) return;
    if (status_.awaitingPresentation) {
        if (!endpointPresented_) return;
        endpointPresented_ = false;
        if (status_.replaySeconds >= status_.replayDuration) {
            if (status_.cut == 2) {
                status_.phase = SceneDeathReplayPhase::FinishedHold;
                status_.awaitingPresentation = false;
                completionPending_ = true;
                return;
            }
            ++status_.cut;
            status_.phase = status_.cut == 1 ? SceneDeathReplayPhase::ReplayB : SceneDeathReplayPhase::ReplayC;
            status_.replaySeconds = 0;
            ++poseSerial_;
            return; // 新しいcutの冒頭を必ず表示し、前cutの余りdeltaを持ち越さない。
        }
        status_.awaitingPresentation = false;
    }
    const double step = static_cast<double>(delta) * (status_.staticClip ? 1.0 : replaySpeed_);
    if (step <= 0) return;
    status_.replaySeconds = (std::min)(status_.replayDuration, status_.replaySeconds + step);
    ++poseSerial_;
    status_.awaitingPresentation = status_.replaySeconds >= status_.replayDuration;
}
SceneReplayPresentationToken SceneDeathReplaySystem::PresentationToken(uint64_t drawSerial) const {
    if (!IsReplaying() || !drawSerial) return {};
    return {started_.instance, started_.generation, started_.owner, started_.serial,
        status_.cut, poseSerial_, drawSerial};
}
void SceneDeathReplaySystem::AcceptPresentationReceipt(const SceneReplayPresentationToken& receipt) {
    if (!receipt.drawSerial || receipt.drawSerial <= acceptedDrawSerial_ ||
        receipt != PresentationToken(receipt.drawSerial)) return;
    acceptedDrawSerial_ = receipt.drawSerial;
    if (status_.awaitingPresentation) endpointPresented_ = true;
}
bool SceneDeathReplaySystem::ConsumeReplayCompleted() {
    const bool result = completionPending_;
    completionPending_ = false;
    return result;
}
void SceneDeathReplaySystem::Clear() {
    started_ = {}; status_ = {}; replaySpeed_ = 1;
    poseSerial_ = acceptedDrawSerial_ = 0;
    endpointPresented_ = completionPending_ = false;
}
bool SceneDeathReplaySystem::TryGetStatus(uint64_t owner, SceneDeathReplayStatus& out) const {
	if (!started_.serial || status_.owner != owner) return false;
	out = status_; return true;
}
