// 役割: 演出付き遷移要求とSceneロード予約の到達結果を、所有ポインタなしの値で公開する。
#pragma once

#include <string>

#include "SceneEntityReference.h"

struct PresentedTransitionRequest {
	SceneInstanceId sourceInstanceId = kInvalidSceneInstanceId;
	std::string destinationSceneId;
	std::string transitionAssetPath;
};

enum class SceneLoadStatus {
	Activated,
	Failed
};

// ActivatedはInitializeの正常復帰を表す。取消はFailedと診断で返し、演出完了ではない。
struct SceneLoadOutcome {
	SceneInstanceId instanceId = kInvalidSceneInstanceId;
	SceneLoadStatus status = SceneLoadStatus::Failed;
	std::string diagnostic;
};
