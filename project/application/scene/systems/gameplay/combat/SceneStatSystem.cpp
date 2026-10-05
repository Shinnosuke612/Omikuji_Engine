// 役割: Sceneに保存されたStat定義と実行時の現在値を分離して更新する。
#include "SceneStatSystem.h"

#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
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
		for (auto iterator = entityStats.begin(); iterator != entityStats.end();) {
			if (!requiredStats.contains(iterator->first)) {
				iterator = entityStats.erase(iterator);
			} else {
				++iterator;
			}
		}
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
	StatRuntime& runtime = stat->second;
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
	runtime.value = std::clamp(
		runtime.value,
		runtime.minValue,
		runtime.maxValue
	);
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

void SceneStatSystem::Clear() {
	runtimes_.clear();
}
