// 役割: EventTriggerの条件を評価し、Stat変更・Entity制御・Scene遷移を実行する。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../text/SceneTextMotionSystem.h"
#include "../animation/ScenePrefabAnimationSystem.h"

class SceneDocument;
class SceneStatSystem;
class SceneStateMachineSystem;

enum class ScenePostProcessRequestType {
	None,
	SetProfile,
	NextProfile,
	ResetToSceneDefault
};

struct ScenePostProcessRequest {
	ScenePostProcessRequestType type = ScenePostProcessRequestType::None;
	uint64_t managerEntityId = 0;
	std::string managerEntityName;
	std::string profileId;
};

enum class SceneCameraRequestType {
	PlayPath,
	StopPath,
	SelectCamera
};

struct SceneCameraRequest {
	SceneCameraRequestType type = SceneCameraRequestType::PlayPath;
	uint64_t entityId = 0;
	std::string entityName;
};

enum class SceneAudioRequestType {
	Play,
	Stop,
	Pause,
	Resume
};

struct SceneAudioRequest {
	SceneAudioRequestType type = SceneAudioRequestType::Play;
	uint64_t entityId = 0;
};

enum class SceneTextMotionRequestType {
	Play,
	Stop,
	Reset
};

struct SceneTextMotionRequest {
	SceneTextMotionRequestType type = SceneTextMotionRequestType::Play;
	uint64_t entityId = 0;
	std::string clipId;
};

enum class ScenePresentationRequestType { Play, Stop, Reset };

// Event順を保持し、RuntimeSceneが次FrameのObject同期前に再解決して適用する。
struct ScenePresentationRequest {
	ScenePresentationRequestType type = ScenePresentationRequestType::Play;
	uint64_t entityId = 0;
	std::string clipName;
};

struct SceneEventRuntimeSignals {
	uint64_t completedCameraPathEntityId = 0;
	std::vector<uint64_t> finishedAudioEntityIds;
	std::vector<SceneTextMotionCompletion> textMotionCompletions;
	std::vector<ScenePresentationCompletion> presentationCompletions;
};

struct SceneEventResult {
	std::string sceneTransitionId;
	std::string transitionAssetPath;
	ScenePostProcessRequest postProcessRequest;
	std::vector<SceneCameraRequest> cameraRequests;
	std::vector<SceneAudioRequest> audioRequests;
	std::vector<SceneTextMotionRequest> textMotionRequests;
	std::vector<ScenePresentationRequest> presentationRequests;
};

class SceneEventSystem {
public:
	SceneEventResult Update(
		SceneDocument& document,
		SceneStatSystem& statSystem,
		SceneStateMachineSystem& stateMachineSystem,
		float deltaTime,
		const SceneEventRuntimeSignals& signals
	);
	void Clear();

private:
	struct BindingRuntime {
		float cooldown = 0.0f;
		bool initialized = false;
		bool wasConditionTrue = false;
		bool fired = false;
	};

	std::unordered_map<uint64_t, std::vector<BindingRuntime>> runtimes_;
};
