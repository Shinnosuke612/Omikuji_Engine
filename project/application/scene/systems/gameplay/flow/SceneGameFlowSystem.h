// 役割: GameFlowDirectorのRuntime state、phase進行、gameplay許可と値型Requestを所有する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "../../../../../engine/scene/ScenePreparationDiagnostic.h"

class SceneDocument;
class SceneEnemySpawnerSystem;

enum class SceneGameFlowState { Inactive, Preflight, Briefing, PreparingFirstWave, Countdown, StartCue, PhaseActive, PhaseGap, ResultDelay, Result, Faulted };

struct SceneGameFlowTextRequest { uint64_t entityId = 0; std::string text; };
enum class SceneGameFlowMotionRequestType { Play, Reset };
struct SceneGameFlowMotionRequest { uint64_t entityId = 0; std::string clipId; SceneGameFlowMotionRequestType type = SceneGameFlowMotionRequestType::Play; };
struct SceneGameFlowWaveRequest { uint64_t spawnerEntityId = 0; uint64_t generation = 0; int count = 0; bool holdUntilGameplay = false; };
struct SceneGameFlowEntityRequest { uint64_t entityId = 0; bool active = false; };

struct SceneGameFlowResult {
	bool hasDirector = false;
	bool gameplayAllowed = true;
	std::vector<SceneGameFlowTextRequest> textRequests;
	std::vector<SceneGameFlowMotionRequest> motionRequests;
	std::vector<SceneGameFlowWaveRequest> waveRequests;
	std::vector<SceneGameFlowEntityRequest> entityRequests;
	// 開始前は、この有限Waveだけをpresentation時計で進める。
	std::vector<uint64_t> preparationSpawnerIds;
	ScenePreparationDiagnostic diagnostic;
};

class SceneGameFlowSystem {
public:
	// 秒単位で進行し、前FrameのSpawner状態から要求を返す。要求適用とSystem更新順はcallerが所有する。
	SceneGameFlowResult Update(SceneDocument& document, SceneEnemySpawnerSystem& spawnerSystem, float deltaTime);
	bool IsGameplayAllowed() const { return gameplayAllowed_; }
	// Runtime状態だけを初期化する。Spawner/文字の破棄や復元はcaller側のScene終了処理で行う。
	void Clear();

private:
	bool Preflight(const SceneDocument& document, uint64_t directorEntityId, ScenePreparationDiagnostic& diagnostic) const;
	void BeginPhaseWaves(const struct SceneComponent& director, SceneGameFlowResult& result, bool holdUntilGameplay);
	void EnterPhase(const SceneDocument& document, const struct SceneComponent& director, SceneGameFlowResult& result);
	void Fail(ScenePreparationDiagnosticCode code, const std::string& detail);
	static std::string FormatTimer(double seconds, bool centiseconds);

	SceneGameFlowState state_ = SceneGameFlowState::Inactive;
	uint64_t directorEntityId_ = 0;
	int phaseIndex_ = 0;
	int countdownValue_ = 0;
	uint64_t nextGeneration_ = 1;
	std::vector<uint64_t> activeWaveGenerations_;
	std::vector<uint64_t> activeSpawnerIds_;
	bool preparedFirstWave_ = false;
	uint64_t documentGeneration_ = 0;
	uint64_t diagnosticSerial_ = 0;
	ScenePreparationDiagnostic diagnostic_;
	double elapsedSeconds_ = 0.0;
	float stateElapsedSeconds_ = 0.0f;
	bool timerRunning_ = false;
	bool gameplayAllowed_ = true;
};
