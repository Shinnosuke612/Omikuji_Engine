// 役割: GameFlowDirectorのRuntime state、phase進行、gameplay許可と値型Requestを所有する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "../../../../../engine/scene/ScenePreparationDiagnostic.h"
#include "../../../../../engine/scene/SceneRunUpgradeRuntime.h"

class SceneDocument;
class SceneEnemySpawnerSystem;

enum class SceneGameFlowState { Inactive, Preflight, Briefing, PreparingFirstWave, Countdown, StartCue, PhaseActive, UpgradeSelection, PhaseGap, ResultDelay, Result, Faulted };

struct SceneGameFlowTextRequest { uint64_t entityId = 0; std::string text; };
enum class SceneGameFlowMotionRequestType { Play, Reset };
struct SceneGameFlowMotionRequest { uint64_t entityId = 0; std::string clipId; SceneGameFlowMotionRequestType type = SceneGameFlowMotionRequestType::Play; };
struct SceneGameFlowWaveRequest { uint64_t spawnerEntityId = 0; uint64_t generation = 0; int count = 0; bool holdUntilGameplay = false; };
struct SceneGameFlowEntityRequest { uint64_t entityId = 0; bool active = false; };

struct SceneGameFlowResult {
	// 初回StartCue→PhaseActiveだけのpulse。callerは保持用結果へコピーする前に取り出す。
	uint64_t startedDirectorEntityId = 0;
	uint64_t startedDocumentGeneration = 0;
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
	// 前段で秒時計/開始準備を更新し、finalize=trueでは時計を進めずPhase以降の結果を確定する。
	// callerは死亡判定後に必要なFrameだけ後段を一回呼ぶ。要求の適用もcallerが所有する。
	SceneGameFlowResult Update(SceneDocument& document, SceneEnemySpawnerSystem& spawnerSystem, float deltaTime, bool finalize = false, uint64_t instance = 0);
	// 待ちtokenとpoolはFlow所有。callerは同じDocument/設定を毎prepass照合する。
	bool IsAwaitingUpgrade() const { return state_ == SceneGameFlowState::UpgradeSelection; }
	const SceneUpgradeToken& UpgradeToken() const { return upgradeToken_; }
	uint64_t UpgradePanel() const { return upgradePanel_; }
	const std::vector<SceneUpgradePoolEntry>& UpgradePool() const { return upgradePool_; }
	bool MatchesUpgrade(const SceneDocument& document, const SceneUpgradeToken& token) const;
	// commitしたFrameの次prepassだけで呼ぶ。一致する待ち一件をPhaseGapへ進める。
	bool ResumeAfterUpgrade(const SceneUpgradeToken& token);
	void FailUpgrade(const std::string& issue) { Fail(ScenePreparationDiagnosticCode::InvalidReference, issue); }
	// Phase以降だけ戦闘/Event後に確定。前段は時計だけを進める。
	bool NeedsFinalize() const { return state_ == SceneGameFlowState::PhaseActive || state_ == SceneGameFlowState::PhaseGap || state_ == SceneGameFlowState::ResultDelay || state_ == SceneGameFlowState::Result; }
	bool IsGameplayAllowed() const { return gameplayAllowed_; }
	// Result表示へ到達した時点で、RuntimeSceneがプレイ内の取得状態を破棄する。
	bool IsFinished() const { return state_ == SceneGameFlowState::Result; }
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
	SceneUpgradeToken upgradeToken_{};
	uint64_t upgradePanel_ = 0, nextSelectionSerial_ = 0;
	std::vector<SceneUpgradePoolEntry> upgradePool_;
};
