// 役割: Sceneに保存されたStat定義と実行時の現在値を分離して更新する。
#include "SceneStatSystem.h"

#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <utility>
#include <cmath>
#include <unordered_set>

void SceneStatSystem::Update(const SceneDocument& document) {
	std::unordered_set<uint64_t> requiredEntities;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* statSet =
			SceneEntityQuery::FindEnabledComponent(entity, "StatSet");
		if (!statSet) {
			continue;
		}
		requiredEntities.insert(entity.id);
		auto& entityStats = runtimes_[entity.id];
		std::unordered_set<std::string> requiredStats;
		for (const SceneStatDefinition& definition : statSet->stats) {
			if (definition.id.empty()) {
				continue;
			}
			requiredStats.insert(definition.id);
			const float minValue = definition.minValue;
			const float maxValue = (std::max)(definition.maxValue, minValue);
			auto found = entityStats.find(definition.id);
			if (found == entityStats.end()) {
				entityStats.emplace(
					definition.id,
					StatRuntime{
						std::clamp(definition.initialValue, minValue, maxValue),
						minValue,
						maxValue
					}
				);
				continue;
			}
			found->second.minValue = minValue;
			found->second.maxValue = maxValue;
			found->second.value = std::clamp(
				found->second.value,
				minValue,
				maxValue
			);
		}
		auto& entityFlags = flags_[entity.id];
		std::unordered_set<std::string> requiredFlags;
		for (const auto& flag : statSet->flags) {
			if (flag.id.empty() || requiredStats.contains(flag.id) || !requiredFlags.insert(flag.id).second) continue;
			entityFlags.try_emplace(flag.id, FlagRuntime{flag.initialValue, 0});
		}
		for (auto it = entityFlags.begin(); it != entityFlags.end();) {
			if (!requiredFlags.contains(it->first)) it = entityFlags.erase(it);
			else ++it;
		}
		for (auto iterator = entityStats.begin(); iterator != entityStats.end();) {
			if (!requiredStats.contains(iterator->first)) {
				iterator = entityStats.erase(iterator);
			} else {
				++iterator;
			}
		}
	}

	for (auto it = flags_.begin(); it != flags_.end();) {
		if (!requiredEntities.contains(it->first)) it = flags_.erase(it);
		else ++it;
	}
	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredEntities.contains(iterator->first)) {
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

bool SceneStatSystem::Modify(
	uint64_t entityId,
	const std::string& statId,
	const std::string& operation,
	float value
) {
	auto entity = runtimes_.find(entityId);
	if (entity == runtimes_.end()) {
		return false;
	}
	auto stat = entity->second.find(statId);
	if (stat == entity->second.end()) {
		return false;
	}
	if (!std::isfinite(value)) return false;
	// 管理HPの下限0/正の上限を外部操作で壊さない。
	for (const auto& binding : deathBindings_) if (binding.stats == entityId && binding.hp == statId) {
		if ((operation == "SetMin" && value != 0.0f) || (operation == "SetMax" && value <= 0.0f)) return false;
	}
	StatRuntime runtime = stat->second;
	if (operation == "Add") {
		runtime.value += value;
	} else if (operation == "Subtract") {
		runtime.value -= value;
	} else if (operation == "Set") {
		runtime.value = value;
	} else if (operation == "Multiply") {
		runtime.value *= value;
	} else if (operation == "SetMin") {
		runtime.minValue = (std::min)(value, runtime.maxValue);
	} else if (operation == "SetMax") {
		runtime.maxValue = (std::max)(value, runtime.minValue);
	} else if (operation == "RestoreToMax") {
		runtime.value = runtime.maxValue;
	} else {
		return false;
	}
	if (!std::isfinite(runtime.value)) return false;
	runtime.value = std::clamp(
		runtime.value,
		runtime.minValue,
		runtime.maxValue
	);
	stat->second = runtime;
	if (runtime.value <= 0.0f) for (const auto& binding : deathBindings_)
		if (binding.stats == entityId && binding.hp == statId) SetFlag(entityId, binding.flag, true);
	return true;
}

bool SceneStatSystem::TryGet(
	uint64_t entityId,
	const std::string& statId,
	float& value,
	float* minValue,
	float* maxValue
) const {
	const auto entity = runtimes_.find(entityId);
	if (entity == runtimes_.end()) {
		return false;
	}
	const auto stat = entity->second.find(statId);
	if (stat == entity->second.end()) {
		return false;
	}
	value = stat->second.value;
	if (minValue) {
		*minValue = stat->second.minValue;
	}
	if (maxValue) {
		*maxValue = stat->second.maxValue;
	}
	return true;
}

bool SceneStatSystem::IsAtMin(
	uint64_t entityId,
	const std::string& statId
) const {
	float value = 0.0f;
	float minValue = 0.0f;
	return TryGet(entityId, statId, value, &minValue) &&
		value <= minValue + 0.0001f;
}

bool SceneStatSystem::TryGetFlag(uint64_t entityId, const std::string& flagId, bool& value, uint64_t* revision) const {
	const auto entity = flags_.find(entityId);
	if (entity == flags_.end()) return false;
	const auto flag = entity->second.find(flagId);
	if (flag == entity->second.end()) return false;
	value = flag->second.value;
	if (revision) *revision = flag->second.revision;
	return true;
}

bool SceneStatSystem::SetFlag(uint64_t entityId, const std::string& flagId, bool value) {
	const auto entity = flags_.find(entityId);
	if (entity == flags_.end()) return false;
	const auto flag = entity->second.find(flagId);
	if (flag == entity->second.end()) return false;
	for (const auto& binding : deathBindings_) if (binding.stats == entityId && binding.flag == flagId) {
		if (!value && flag->second.value) return false;
		if (value) {
			auto stats = runtimes_.find(entityId);
			if (stats == runtimes_.end()) return false;
			auto hp = stats->second.find(binding.hp);
			if (hp == stats->second.end()) return false;
			// 同値trueは通知もHPも再変更しない。死亡後のHP回復は復活ではない。
			if (!flag->second.value) hp->second.value = hp->second.minValue;
		}
	}
	if (flag->second.value != value) {
		if (value && IsManagedFlag(entityId, flagId)) ++deathRevision_;
		flag->second.value = value;
		flag->second.revision = ++nextFlagRevision_;
	}
	return true;
}

void SceneStatSystem::SetDeathBindings(std::vector<DeathBinding> bindings) { deathBindings_ = std::move(bindings); }
bool SceneStatSystem::IsManagedFlag(uint64_t stats, const std::string& flag) const {
	return std::any_of(deathBindings_.begin(), deathBindings_.end(), [&](const auto& b) { return b.stats == stats && b.flag == flag; });
}
bool SceneStatSystem::IsManagedDead(uint64_t stats, const std::string& hp) const {
	for (const auto& binding : deathBindings_) if (binding.stats == stats && binding.hp == hp) {
		bool dead = false;
		if (TryGetFlag(stats, binding.flag, dead) && dead) return true;
	}
	return false;
}
void SceneStatSystem::ResetEntity(uint64_t id) {
	runtimes_.erase(id); flags_.erase(id);
	std::erase_if(deathBindings_, [id](const auto& b) { return b.owner == id || b.stats == id; });
}
void SceneStatSystem::Clear() {
	deathBindings_.clear(); deathRevision_ = 0;
	flags_.clear();
	nextFlagRevision_ = 0;
	runtimes_.clear();
}
