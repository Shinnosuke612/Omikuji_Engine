// 役割: raw deltaでHit Stopを消化し、Gameplay用deltaを決定する。
#include "SceneHitStopSystem.h"

#include <algorithm>

float SceneHitStopSystem::Advance(float rawDeltaTime) {
	const float rawDelta = (std::max)(rawDeltaTime, 0.0f);
	if (remainingTime_ <= 0.0f) {
		return rawDelta;
	}
	remainingTime_ = (std::max)(remainingTime_ - rawDelta, 0.0f);
	return 0.0f;
}

void SceneHitStopSystem::Request(float duration) {
	remainingTime_ = (std::max)(
		remainingTime_,
		(std::max)(duration, 0.0f)
	);
}
