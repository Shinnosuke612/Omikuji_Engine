// 役割: EnemySpawnerのRuntime専用Prefab生成と非アクティブInstance再利用を管理する。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../../../engine/scene/SceneDocument.h"

class SceneEnemySpawnerSystem {
public:
	enum class FiniteWaveState {
		Idle,
		Running,
		Complete,
		Failed
	};

	struct FiniteWaveStatus {
		FiniteWaveState state = FiniteWaveState::Idle;
		uint64_t generation = 0;
		int requestedCount = 0;
		int activatedCount = 0;
		int activeCount = 0;
		std::string diagnostic;
	};

	void Update(SceneDocument& document, float deltaTime);
	void BeginFiniteWave(
		uint64_t spawnerEntityId,
		uint64_t generation,
		int requestedCount
	);
	FiniteWaveStatus GetFiniteWaveStatus(uint64_t spawnerEntityId) const;
	void StopFiniteWave(uint64_t spawnerEntityId);
	std::vector<uint64_t> ConsumeResetEntityIds();
	void Clear();

private:
	struct Instance {
		uint64_t rootEntityId = 0;
		// 生成直後のPrefab枝を保持し、再利用時に子HitBoxを含むAuthoring値へ戻す。
		std::unordered_map<uint64_t, SceneEntity> baselineEntities;
	};

	struct Runtime {
		float timer = 0.0f;
		int spawnedInitial = 0;
		uint32_t spawnIndex = 0;
		std::vector<Instance> instances;
		FiniteWaveStatus finiteWave;
	};

	void UpdateFiniteWave(
		SceneDocument& document,
		const SceneEntity& spawnerEntity,
		const SceneComponent& spawner,
		Runtime& runtime,
		float deltaTime
	);

	bool RestoreInstance(
		SceneDocument& document,
		const Instance& instance,
		const Vector3& spawnPosition
	);
	Instance CaptureInstance(
		const SceneDocument& document,
		uint64_t rootEntityId
	) const;

	std::unordered_map<uint64_t, Runtime> runtimes_;
	std::vector<uint64_t> resetEntityIds_;
};
