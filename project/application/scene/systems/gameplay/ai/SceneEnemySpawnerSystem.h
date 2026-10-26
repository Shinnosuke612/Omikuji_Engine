// 役割: EnemySpawnerのRuntime専用Prefab生成と非アクティブInstance再利用を管理する。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/ScenePreparationDiagnostic.h"
#include "../../../SceneSpawnPresentation.h"

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
		// 全滅Completeと区別する。completedは世代内累積、presentingは現在の生存個体数。
		int appearanceCompletedCount = 0;
		int presentingCount = 0;
		bool preparationReady = false;
		std::string diagnostic;
	};

	void Update(SceneDocument& document, float deltaTime);
	// 秒単位。開始前は明示された準備Waveだけ進め、他のautoStartは動かさない。
	void Update(SceneDocument& document, const SceneSpawnUpdateContext& context);
	// 同一generationは冪等。hold=trueは全個体が100%でも戦闘許可まで参加させない。
	void BeginFiniteWave(
		uint64_t spawnerEntityId,
		uint64_t generation,
		int requestedCount,
		bool holdUntilGameplay = false
	);
	FiniteWaveStatus GetFiniteWaveStatus(uint64_t spawnerEntityId) const;
	// root/子孫を値で返す。描画倍率は保存Transformへ書き込まない。
	SceneSpawnFrame BuildPresentationFrame(const SceneDocument& document, bool gameplayAllowed) const;
	ScenePreparationDiagnostic GetPreparationDiagnostic() const { return diagnostic_; }
	// 次回Updateで未完成/開始待ち個体を取消。戦闘参加済みの個体は維持する。
	void StopFiniteWave(uint64_t spawnerEntityId);
	std::vector<uint64_t> ConsumeResetEntityIds();
	void Clear();

private:
	enum class AppearanceState { Growing, FullSizePending, Ready, Retired };
	struct Instance {
		uint64_t rootEntityId = 0;
		// 生成直後のPrefab枝を保持し、再利用時に子HitBoxを含むAuthoring値へ戻す。
		std::unordered_map<uint64_t, SceneEntity> baselineEntities;
		uint64_t activationSerial = 0;
		uint64_t waveGeneration = 0;
		float elapsed = 0.0f;
		float scale = 1.0f;
		float startScale = 0.0f;
		float duration = 0.5f;
		bool easeOut = true;
		bool scaleUp = false;
		bool holdUntilGameplay = false;
		AppearanceState appearance = AppearanceState::Ready;
	};

	struct Runtime {
		float timer = 0.0f;
		int spawnedInitial = 0;
		uint32_t spawnIndex = 0;
		std::vector<Instance> instances;
		FiniteWaveStatus finiteWave;
		bool holdUntilGameplay = false;
		bool cancelPending = false;
		bool stopped = false;
		std::string validatedPrefabPath;
	};
	void Fail(uint64_t spawnerId, Runtime& runtime, ScenePreparationDiagnosticCode code, const std::string& detail);
	void AdvanceInstances(SceneDocument& document, uint64_t spawnerId, Runtime& runtime, float deltaTime, bool gameplayAllowed);
	void RefreshStatus(const SceneDocument& document, Runtime& runtime);
	void CancelPending(SceneDocument& document, Runtime& runtime);
	bool ActivateInstance(SceneDocument& document, uint64_t spawnerId, const SceneComponent& settings,
		Runtime& runtime, Instance* reusable, const Vector3& position, bool finite);

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
	uint64_t documentGeneration_ = 0;
	uint64_t activationSerial_ = 0;
	uint64_t diagnosticSerial_ = 0;
	ScenePreparationDiagnostic diagnostic_;
};
