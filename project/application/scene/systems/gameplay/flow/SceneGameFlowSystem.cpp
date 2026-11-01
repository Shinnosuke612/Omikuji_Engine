// 役割: GameFlowの保存値を読取り、既存Systemへ渡す値型Requestだけを生成する。
#include "SceneGameFlowSystem.h"

#include "../ai/SceneEnemySpawnerSystem.h"
#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../../engine/scene/SceneValidator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace {
	const SceneComponent* FindDirector(const SceneDocument& document, uint64_t& entityId) {
		const SceneComponent* found = nullptr;
		entityId = 0;
		for (const SceneEntity& entity : document.GetEntities()) {
			const SceneComponent* component = SceneEntityQuery::FindEnabledComponent(entity, "GameFlowDirector");
			if (!component) continue;
			if (found) return nullptr;
			found = component;
			entityId = entity.id;
		}
		return found;
	}

	int CalculateRemainingEnemies(
		const SceneComponent& director,
		const SceneEnemySpawnerSystem& spawnerSystem,
		int phaseIndex,
		const std::vector<uint64_t>& activeWaveGenerations
	) {
		if (phaseIndex < 0 || phaseIndex >= static_cast<int>(director.gameFlowPhases.size())) return 0;
		const SceneGameFlowPhase& phase = director.gameFlowPhases[phaseIndex];
		long long remaining = 0;
		for (size_t index = 0; index < phase.waves.size(); ++index) {
			const SceneGameFlowWave& wave = phase.waves[index];
			int waveRemaining = wave.count;
			if (index < activeWaveGenerations.size()) {
				const SceneEnemySpawnerSystem::FiniteWaveStatus status = spawnerSystem.GetFiniteWaveStatus(wave.spawnerEntityId);
				if (status.generation == activeWaveGenerations[index]) {
					const int defeated = (std::clamp)(status.activatedCount - status.activeCount, 0, (std::max)(status.requestedCount, 0));
					waveRemaining = (std::max)(status.requestedCount - defeated, 0);
				}
			}
			remaining += waveRemaining;
		}
		return static_cast<int>((std::min)(remaining, static_cast<long long>((std::numeric_limits<int>::max)())));
	}
}

void SceneGameFlowSystem::Fail(ScenePreparationDiagnosticCode code, const std::string& detail) {
	state_ = SceneGameFlowState::Faulted;
	timerRunning_ = false;
	gameplayAllowed_ = false;
	if (diagnostic_.code == ScenePreparationDiagnosticCode::None)
		diagnostic_ = {code, documentGeneration_, directorEntityId_, ++diagnosticSerial_, detail};
}

SceneGameFlowResult SceneGameFlowSystem::Update(SceneDocument& document, SceneEnemySpawnerSystem& spawnerSystem, float deltaTime, bool finalize) {
	if (documentGeneration_ != 0 && documentGeneration_ != document.GetContentGeneration()) Clear();
	documentGeneration_ = document.GetContentGeneration();
	SceneGameFlowResult result;
	uint64_t foundDirectorId = 0;
	const SceneComponent* director = FindDirector(document, foundDirectorId);
	if (!director) {
		if (foundDirectorId || directorEntityId_) {
			if (!directorEntityId_) directorEntityId_ = foundDirectorId;
			Fail(ScenePreparationDiagnosticCode::InvalidReference, "FlowPreparation:InvalidReference");
			for (uint64_t id : activeSpawnerIds_) spawnerSystem.StopFiniteWave(id);
			result.gameplayAllowed = false;
			result.diagnostic = diagnostic_;
		} else Clear();
		return result;
	}
	result.hasDirector = true;
	if (directorEntityId_ != foundDirectorId) {
		Clear();
		documentGeneration_ = document.GetContentGeneration();
		directorEntityId_ = foundDirectorId;
	}
	if (!director->gameFlowAutoStart && state_ == SceneGameFlowState::Inactive) {
		result.gameplayAllowed = gameplayAllowed_ = false;
		return result;
	}
	const bool briefingFirst = director->gameFlowStartSequence == "BriefingSpawnCountdown";
	const uint64_t introId = director->gameFlowIntroTextEntityId ? director->gameFlowIntroTextEntityId : director->gameFlowCountdownTextEntityId;
	const float elapsed = std::isfinite(deltaTime) ? (std::max)(deltaTime, 0.0f) : 0.0f;
	bool enteredState = false;
	if (state_ == SceneGameFlowState::Inactive) state_ = SceneGameFlowState::Preflight;
	if (state_ == SceneGameFlowState::Preflight) {
		ScenePreparationDiagnostic diagnostic;
		if (!Preflight(document, foundDirectorId, diagnostic)) Fail(diagnostic.code, diagnostic.detail);
		else {
			stateElapsedSeconds_ = 0.0f;
			enteredState = true;
			if (briefingFirst) {
				state_ = SceneGameFlowState::Briefing;
				result.motionRequests.push_back({introId, {}, SceneGameFlowMotionRequestType::Reset});
				if (!director->gameFlowIntroMotionClipId.empty()) result.motionRequests.push_back({introId, director->gameFlowIntroMotionClipId});
			} else {
				state_ = SceneGameFlowState::Countdown;
				countdownValue_ = director->gameFlowCountdownStart;
				result.motionRequests.push_back({director->gameFlowCountdownTextEntityId, director->gameFlowCountdownMotionClipId});
			}
		}
	}
	// 新状態へ入ったFrameは時刻0を表示し、長いdeltaを次状態へ繰り越さない。
	if (!finalize && !enteredState) stateElapsedSeconds_ = static_cast<float>((std::min)(
		static_cast<double>(stateElapsedSeconds_) + elapsed, static_cast<double>((std::numeric_limits<float>::max)())));
	if (!finalize && timerRunning_) elapsedSeconds_ += elapsed;

	// 待機中の世代/個体破損はCountdown中も検知する。準備済みを再生成で補わない。
	if (preparedFirstWave_ && phaseIndex_ == 0 && (state_ == SceneGameFlowState::PreparingFirstWave ||
		state_ == SceneGameFlowState::Countdown || state_ == SceneGameFlowState::StartCue)) {
		if (director->gameFlowPhases.empty() || activeSpawnerIds_.size() != director->gameFlowPhases.front().waves.size())
			Fail(ScenePreparationDiagnosticCode::WaveMismatch, "FlowPreparation:WaveMismatch");
		else for (size_t index = 0; index < activeSpawnerIds_.size(); ++index) {
			const auto& wave = director->gameFlowPhases.front().waves[index];
			const auto status = spawnerSystem.GetFiniteWaveStatus(activeSpawnerIds_[index]);
			if (status.state == SceneEnemySpawnerSystem::FiniteWaveState::Failed)
				Fail(ScenePreparationDiagnosticCode::SpawnFailed, "FlowPreparation:SpawnFailed " + status.diagnostic);
			else if (wave.spawnerEntityId != activeSpawnerIds_[index] || status.generation != activeWaveGenerations_[index] ||
				status.requestedCount != wave.count || status.state != SceneEnemySpawnerSystem::FiniteWaveState::Running)
				Fail(ScenePreparationDiagnosticCode::WaveMismatch, "FlowPreparation:WaveMismatch");
		}
	}
	if (!enteredState && state_ == SceneGameFlowState::Briefing && stateElapsedSeconds_ >= director->gameFlowIntroSeconds) {
		result.motionRequests.push_back({introId, {}, SceneGameFlowMotionRequestType::Reset});
		state_ = SceneGameFlowState::PreparingFirstWave;
		stateElapsedSeconds_ = 0.0f;
		BeginPhaseWaves(*director, result, true);
		preparedFirstWave_ = true;
	} else if (!enteredState && state_ == SceneGameFlowState::PreparingFirstWave) {
		bool ready = !activeSpawnerIds_.empty();
		for (uint64_t id : activeSpawnerIds_) ready &= spawnerSystem.GetFiniteWaveStatus(id).preparationReady;
		if (ready) {
			state_ = SceneGameFlowState::Countdown;
			stateElapsedSeconds_ = 0.0f;
			countdownValue_ = director->gameFlowCountdownStart;
			result.motionRequests.push_back({director->gameFlowCountdownTextEntityId, director->gameFlowCountdownMotionClipId});
		}
	} else if (!enteredState && state_ == SceneGameFlowState::Countdown) {
		const double steps = static_cast<double>(stateElapsedSeconds_) / director->gameFlowCountdownStepSeconds;
		const int expected = steps >= director->gameFlowCountdownStart ? 0 : director->gameFlowCountdownStart - static_cast<int>(steps);
		while (countdownValue_ > expected) {
			--countdownValue_;
			if (countdownValue_ > 0) result.motionRequests.push_back({director->gameFlowCountdownTextEntityId, director->gameFlowCountdownMotionClipId});
		}
		if (countdownValue_ <= 0) {
			state_ = SceneGameFlowState::StartCue;
			stateElapsedSeconds_ = 0.0f;
			result.motionRequests.push_back({director->gameFlowCountdownTextEntityId, director->gameFlowCountdownMotionClipId});
		}
	} else if (!enteredState && state_ == SceneGameFlowState::StartCue && stateElapsedSeconds_ >= director->gameFlowStartCueSeconds) {
		state_ = SceneGameFlowState::PhaseActive;
		stateElapsedSeconds_ = 0.0f;
		timerRunning_ = true;
		EnterPhase(document, *director, result);
		if (!finalize && state_ == SceneGameFlowState::PhaseActive) {
			result.startedDirectorEntityId = directorEntityId_;
			result.startedDocumentGeneration = documentGeneration_;
		}
	} else if (finalize && state_ == SceneGameFlowState::PhaseActive) {
		bool complete = !activeWaveGenerations_.empty();
		for (size_t index = 0; index < activeWaveGenerations_.size(); ++index) {
			const auto status = spawnerSystem.GetFiniteWaveStatus(activeSpawnerIds_[index]);
			if (status.state == SceneEnemySpawnerSystem::FiniteWaveState::Failed)
				Fail(ScenePreparationDiagnosticCode::SpawnFailed, "FlowPreparation:SpawnFailed " + status.diagnostic);
			else if (status.generation != activeWaveGenerations_[index] || status.state == SceneEnemySpawnerSystem::FiniteWaveState::Idle)
				Fail(ScenePreparationDiagnosticCode::WaveMismatch, "FlowPreparation:WaveMismatch");
			complete &= status.state == SceneEnemySpawnerSystem::FiniteWaveState::Complete;
		}
		if (state_ != SceneGameFlowState::Faulted && complete) {
			++phaseIndex_;
			stateElapsedSeconds_ = 0.0f;
			if (phaseIndex_ >= static_cast<int>(director->gameFlowPhases.size())) { state_ = SceneGameFlowState::ResultDelay; timerRunning_ = false; }
			else state_ = SceneGameFlowState::PhaseGap;
		}
	} else if (finalize && state_ == SceneGameFlowState::PhaseGap && stateElapsedSeconds_ >= director->gameFlowInterPhaseDelaySeconds) {
		state_ = SceneGameFlowState::PhaseActive;
		stateElapsedSeconds_ = 0.0f;
		EnterPhase(document, *director, result);
	} else if (finalize && state_ == SceneGameFlowState::ResultDelay && stateElapsedSeconds_ >= director->gameFlowResultRevealDelaySeconds) {
		state_ = SceneGameFlowState::Result;
		result.entityRequests.push_back({director->gameFlowResultRootEntityId, true});
		result.motionRequests.push_back({director->gameFlowResultTimeTextEntityId, director->gameFlowResultMotionClipId});
	}
	gameplayAllowed_ = state_ == SceneGameFlowState::PhaseActive || state_ == SceneGameFlowState::PhaseGap;
	result.gameplayAllowed = gameplayAllowed_;
	if (state_ == SceneGameFlowState::PreparingFirstWave) result.preparationSpawnerIds = activeSpawnerIds_;
	if (state_ == SceneGameFlowState::Faulted) {
		for (uint64_t id : activeSpawnerIds_) spawnerSystem.StopFiniteWave(id);
		result.diagnostic = diagnostic_;
	}
	const auto addText = [&result](uint64_t entityId, std::string text) {
		if (entityId) result.textRequests.push_back({entityId, std::move(text)});
	};
	if (briefingFirst && introId != director->gameFlowCountdownTextEntityId)
		addText(introId, state_ == SceneGameFlowState::Briefing ? director->gameFlowIntroText : std::string{});
	if (state_ == SceneGameFlowState::Briefing) addText(director->gameFlowCountdownTextEntityId, introId == director->gameFlowCountdownTextEntityId ? director->gameFlowIntroText : std::string{});
	else if (state_ == SceneGameFlowState::Countdown) addText(director->gameFlowCountdownTextEntityId, std::to_string(countdownValue_));
	else if (state_ == SceneGameFlowState::StartCue) addText(director->gameFlowCountdownTextEntityId, director->gameFlowStartCueText);
	else addText(director->gameFlowCountdownTextEntityId, {});
	if (state_ == SceneGameFlowState::PhaseActive || state_ == SceneGameFlowState::PhaseGap || state_ == SceneGameFlowState::ResultDelay) {
		const int labelIndex = state_ == SceneGameFlowState::PhaseActive ? phaseIndex_ : phaseIndex_ - 1;
		addText(director->gameFlowPhaseTextEntityId, labelIndex >= 0 && labelIndex < static_cast<int>(director->gameFlowPhases.size()) ? director->gameFlowPhases[labelIndex].label : std::string{});
		const double displayedSeconds = std::floor(elapsedSeconds_ / director->gameFlowTimerDisplayStepSeconds) * director->gameFlowTimerDisplayStepSeconds;
		addText(director->gameFlowTimerTextEntityId, director->gameFlowTimerPrefix + FormatTimer(displayedSeconds, false));
	} else {
		addText(director->gameFlowPhaseTextEntityId, {});
		addText(director->gameFlowTimerTextEntityId, {});
	}
	if (state_ == SceneGameFlowState::PhaseActive)
		addText(director->gameFlowRemainingTextEntityId, director->gameFlowRemainingPrefix + std::to_string(CalculateRemainingEnemies(*director, spawnerSystem, phaseIndex_, activeWaveGenerations_)));
	else if (state_ == SceneGameFlowState::PhaseGap || state_ == SceneGameFlowState::ResultDelay)
		addText(director->gameFlowRemainingTextEntityId, director->gameFlowRemainingPrefix + "0");
	else addText(director->gameFlowRemainingTextEntityId, {});
	if (state_ == SceneGameFlowState::Result) addText(director->gameFlowResultTimeTextEntityId, director->gameFlowResultPrefix + FormatTimer(elapsedSeconds_, true));
	return result;
}

bool SceneGameFlowSystem::Preflight(const SceneDocument& document, uint64_t directorEntityId, ScenePreparationDiagnostic& diagnostic) const {
	const auto* entity = document.FindEntity(directorEntityId);
	const auto* director = entity ? SceneEntityQuery::FindEnabledComponent(*entity, "GameFlowDirector") : nullptr;
	if (!director) { diagnostic.code = ScenePreparationDiagnosticCode::InvalidReference; diagnostic.detail = "FlowPreparation:InvalidReference"; return false; }
	std::unordered_set<uint64_t> spawners;
	for (const auto& phase : director->gameFlowPhases) for (const auto& wave : phase.waves) spawners.insert(wave.spawnerEntityId);
	std::vector<SceneValidationIssue> issues;
	SceneValidator::ValidateDocument(document, nullptr, {}, {}, issues);
	for (const auto& issue : issues) {
		const bool flowIssue = issue.entityId == directorEntityId && issue.message.starts_with("FlowPreparation:");
		const bool spawnIssue = spawners.contains(issue.entityId) && issue.message.starts_with("SpawnPreparation:");
		if (!flowIssue && !spawnIssue) continue;
		diagnostic.code = ScenePreparationDiagnosticCode::InvalidReference;
		if (issue.message.find("InvalidSettings") != std::string::npos) diagnostic.code = ScenePreparationDiagnosticCode::InvalidSettings;
		else if (issue.message.find("CapacityExceeded") != std::string::npos) diagnostic.code = ScenePreparationDiagnosticCode::CapacityExceeded;
		else if (issue.message.find("UnsupportedPrefab") != std::string::npos) diagnostic.code = ScenePreparationDiagnosticCode::UnsupportedPrefab;
		diagnostic.detail = flowIssue ? issue.message : "FlowPreparation:SpawnFailed " + issue.message;
		return false;
	}
	return true;
}

void SceneGameFlowSystem::BeginPhaseWaves(const SceneComponent& director, SceneGameFlowResult& result, bool holdUntilGameplay) {
	if (phaseIndex_ < 0 || phaseIndex_ >= static_cast<int>(director.gameFlowPhases.size())) {
		Fail(ScenePreparationDiagnosticCode::InvalidReference, "FlowPreparation:InvalidPhase");
		return;
	}
	activeWaveGenerations_.clear();
	activeSpawnerIds_.clear();
	for (const auto& wave : director.gameFlowPhases[phaseIndex_].waves) {
		const uint64_t generation = nextGeneration_++;
		activeWaveGenerations_.push_back(generation);
		activeSpawnerIds_.push_back(wave.spawnerEntityId);
		result.waveRequests.push_back({wave.spawnerEntityId, generation, wave.count, holdUntilGameplay});
	}
}

void SceneGameFlowSystem::EnterPhase(const SceneDocument&, const SceneComponent& director, SceneGameFlowResult& result) {
	result.motionRequests.push_back({director.gameFlowPhaseTextEntityId, director.gameFlowPhaseMotionClipId});
	// 初回準備済みの世代を引継ぐ。後続Phaseと旧方式だけがここでWaveを開始する。
	if (!preparedFirstWave_ || phaseIndex_ != 0) BeginPhaseWaves(director, result, false);
}

std::string SceneGameFlowSystem::FormatTimer(double seconds, bool centiseconds) {
	const double clamped = (std::min)(seconds, 5999.99);
	const int minutes = static_cast<int>(clamped / 60.0);
	const double remainder = clamped - minutes * 60.0;
	char buffer[32]{};
	if (centiseconds) std::snprintf(buffer, sizeof(buffer), "%02d:%05.2f", minutes, remainder);
	else std::snprintf(buffer, sizeof(buffer), "%02d:%04.1f", minutes, remainder);
	return buffer;
}

void SceneGameFlowSystem::Clear() {
	state_ = SceneGameFlowState::Inactive;
	directorEntityId_ = 0;
	phaseIndex_ = 0;
	countdownValue_ = 0;
	nextGeneration_ = 1;
	activeWaveGenerations_.clear();
	activeSpawnerIds_.clear();
	preparedFirstWave_ = false;
	documentGeneration_ = 0;
	diagnostic_ = {};
	elapsedSeconds_ = 0.0;
	stateElapsedSeconds_ = 0.0f;
	timerRunning_ = false;
	gameplayAllowed_ = true;
	// 診断serialは同じRuntimeScene内の再開始でも単調に保つ。
}
