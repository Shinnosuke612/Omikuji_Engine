// 役割: Scene保存対象外の敵Prefabを必要数だけSpawnし、Deactivate済みInstanceを優先再利用する。
#include "SceneEnemySpawnerSystem.h"

#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <utility>

void SceneEnemySpawnerSystem::Update(SceneDocument& document, float deltaTime) {
	std::vector<uint64_t> spawnerIds;
	for (const SceneEntity& entity : document.GetEntities()) {
		const SceneComponent* spawner = SceneEntityQuery::FindEnabledComponent(
			entity, "EnemySpawner"
		);
		if (!spawner || spawner->enemySpawnerPrefabPath.empty()) {
			continue;
		}
		if (!spawner->enemySpawnerAutoStart) {
			const auto runtime = runtimes_.find(entity.id);
			if (runtime == runtimes_.end() ||
				runtime->second.finiteWave.state != FiniteWaveState::Running) {
				continue;
			}
		}
		spawnerIds.push_back(entity.id);
	}

	for (uint64_t spawnerId : spawnerIds) {
		const SceneEntity* spawnerEntity = document.FindEntity(spawnerId);
		const SceneComponent* spawner = spawnerEntity
			? SceneEntityQuery::FindEnabledComponent(*spawnerEntity, "EnemySpawner")
			: nullptr;
		if (!spawner || spawner->enemySpawnerPrefabPath.empty()) {
			continue;
		}
		Runtime& runtime = runtimes_[spawnerId];
		if (!spawner->enemySpawnerAutoStart) {
			if (runtime.finiteWave.state == FiniteWaveState::Running) {
				UpdateFiniteWave(
					document,
					*spawnerEntity,
					*spawner,
					runtime,
					deltaTime
				);
			}
			continue;
		}
		const std::string prefabPath = spawner->enemySpawnerPrefabPath;
		const int initial = (std::max)(spawner->enemySpawnerInitialCount, 0);
		const int maxAlive = (std::max)(spawner->enemySpawnerMaxAlive, initial);
		const float interval = (std::max)(spawner->enemySpawnerInterval, 0.0f);
		const float radius = (std::max)(spawner->enemySpawnerRadius, 0.0f);
		const Vector3 origin = spawnerEntity->transform.translate;
		runtime.instances.erase(
			std::remove_if(
				runtime.instances.begin(),
				runtime.instances.end(),
				[&document](const Instance& instance) {
					return !document.FindEntity(instance.rootEntityId);
				}
			),
			runtime.instances.end()
		);
		int alive = 0;
		Instance* reusable = nullptr;
		for (Instance& instance : runtime.instances) {
			SceneEntity* root = document.FindEntity(instance.rootEntityId);
			if (!root) {
				continue;
			}
			if (root->active) {
				++alive;
			} else if (!reusable) {
				reusable = &instance;
			}
		}
		const bool needsInitial = runtime.spawnedInitial < initial;
		runtime.timer = (std::max)(runtime.timer - deltaTime, 0.0f);
		if (alive >= maxAlive || (!needsInitial && runtime.timer > 0.0f)) { continue; }
		const float angle = static_cast<float>(runtime.spawnIndex) * 2.39996323f;
		const Vector3 spawnPosition = {
			origin.x + std::cos(angle) * radius,
			origin.y,
			origin.z + std::sin(angle) * radius
		};
		if (reusable) {
			if (!RestoreInstance(document, *reusable, spawnPosition)) {
				continue;
			}
		} else {
			const uint64_t id = document.InstantiatePrefab(
				prefabPath, 0, true
			);
			if (SceneEntity* instance = document.FindEntity(id)) {
				runtime.instances.push_back(CaptureInstance(document, id));
				instance->transform.translate = spawnPosition;
			} else {
				continue;
			}
		}
		if (needsInitial) { ++runtime.spawnedInitial; }
		++runtime.spawnIndex;
		runtime.timer = interval;
	}
}

void SceneEnemySpawnerSystem::BeginFiniteWave(
	uint64_t spawnerEntityId,
	uint64_t generation,
	int requestedCount
) {
	Runtime& runtime = runtimes_[spawnerEntityId];
	FiniteWaveStatus& status = runtime.finiteWave;
	if (status.generation == generation &&
		status.state != FiniteWaveState::Idle) {
		return;
	}
	if (status.state == FiniteWaveState::Running) {
		status.state = FiniteWaveState::Failed;
		status.diagnostic = "A different finite wave generation is already running";
		return;
	}
	status = {};
	status.generation = generation;
	status.requestedCount = requestedCount;
	if (generation == 0 || requestedCount <= 0) {
		status.state = FiniteWaveState::Failed;
		status.diagnostic = "Finite wave generation and requested count must be positive";
		return;
	}
	status.state = FiniteWaveState::Running;
	runtime.timer = 0.0f;
	runtime.spawnIndex = 0;
}

SceneEnemySpawnerSystem::FiniteWaveStatus
SceneEnemySpawnerSystem::GetFiniteWaveStatus(uint64_t spawnerEntityId) const {
	const auto runtime = runtimes_.find(spawnerEntityId);
	return runtime == runtimes_.end()
		? FiniteWaveStatus{}
		: runtime->second.finiteWave;
}

void SceneEnemySpawnerSystem::StopFiniteWave(uint64_t spawnerEntityId) {
	const auto runtime = runtimes_.find(spawnerEntityId);
	if (runtime == runtimes_.end()) {
		return;
	}
	runtime->second.finiteWave.state = FiniteWaveState::Idle;
	runtime->second.finiteWave.activeCount = 0;
	runtime->second.finiteWave.diagnostic.clear();
	runtime->second.timer = 0.0f;
}

void SceneEnemySpawnerSystem::UpdateFiniteWave(
	SceneDocument& document,
	const SceneEntity& spawnerEntity,
	const SceneComponent& spawner,
	Runtime& runtime,
	float deltaTime
) {
	FiniteWaveStatus& status = runtime.finiteWave;
	runtime.instances.erase(
		std::remove_if(
			runtime.instances.begin(),
			runtime.instances.end(),
			[&document](const Instance& instance) {
				return !document.FindEntity(instance.rootEntityId);
			}
		),
		runtime.instances.end()
	);
	int alive = 0;
	Instance* reusable = nullptr;
	for (Instance& instance : runtime.instances) {
		SceneEntity* root = document.FindEntity(instance.rootEntityId);
		if (!root) {
			continue;
		}
		if (root->active) {
			++alive;
		} else if (!reusable) {
			reusable = &instance;
		}
	}
	status.activeCount = alive;
	if (status.activatedCount >= status.requestedCount && alive == 0) {
		status.state = FiniteWaveState::Complete;
		return;
	}
	const int maxAlive = (std::max)(spawner.enemySpawnerMaxAlive, 1);
	runtime.timer = (std::max)(runtime.timer - deltaTime, 0.0f);
	if (status.activatedCount >= status.requestedCount ||
		alive >= maxAlive || runtime.timer > 0.0f) {
		return;
	}
	const float angle = static_cast<float>(runtime.spawnIndex) * 2.39996323f;
	const float radius = (std::max)(spawner.enemySpawnerRadius, 0.0f);
	const Vector3 origin = spawnerEntity.transform.translate;
	const Vector3 spawnPosition = {
		origin.x + std::cos(angle) * radius,
		origin.y,
		origin.z + std::sin(angle) * radius
	};
	bool activated = false;
	if (reusable) {
		activated = RestoreInstance(document, *reusable, spawnPosition);
	} else {
		const uint64_t id = document.InstantiatePrefab(
			spawner.enemySpawnerPrefabPath, 0, true
		);
		if (SceneEntity* instance = document.FindEntity(id)) {
			runtime.instances.push_back(CaptureInstance(document, id));
			instance->transform.translate = spawnPosition;
			activated = true;
		}
	}
	if (!activated) {
		status.state = FiniteWaveState::Failed;
		status.diagnostic = "Finite wave prefab activation failed";
		return;
	}
	++status.activatedCount;
	++runtime.spawnIndex;
	runtime.timer = (std::max)(spawner.enemySpawnerInterval, 0.0f);
	status.activeCount = alive + 1;
}

std::vector<uint64_t> SceneEnemySpawnerSystem::ConsumeResetEntityIds() {
	return std::move(resetEntityIds_);
}

void SceneEnemySpawnerSystem::Clear() {
	runtimes_.clear();
	resetEntityIds_.clear();
}

SceneEnemySpawnerSystem::Instance SceneEnemySpawnerSystem::CaptureInstance(
	const SceneDocument& document,
	uint64_t rootEntityId
) const {
	Instance result{};
	result.rootEntityId = rootEntityId;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (entity.id == rootEntityId ||
			document.IsDescendantOf(entity.id, rootEntityId)) {
			result.baselineEntities.emplace(entity.id, entity);
		}
	}
	return result;
}

bool SceneEnemySpawnerSystem::RestoreInstance(
	SceneDocument& document,
	const Instance& instance,
	const Vector3& spawnPosition
) {
	SceneEntity* root = document.FindEntity(instance.rootEntityId);
	if (!root || instance.baselineEntities.empty()) {
		return false;
	}
	for (const auto& entry : instance.baselineEntities) {
		if (!document.FindEntity(entry.first)) {
			return false;
		}
	}
	for (const auto& [entityId, baseline] : instance.baselineEntities) {
		SceneEntity* entity = document.FindEntity(entityId);
		*entity = baseline;
		resetEntityIds_.push_back(entityId);
	}
	root = document.FindEntity(instance.rootEntityId);
	if (!root) {
		return false;
	}
	root->active = true;
	root->transform.translate = spawnPosition;
	return true;
}
