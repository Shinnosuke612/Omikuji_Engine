// 役割: Combat HitからGameplayの一時停止時間だけを所有する。
#pragma once

class SceneHitStopSystem {
public:
	// 停止中は0、通常時はraw deltaをGameplay用に返す。
	float Advance(float rawDeltaTime);
	void Request(float duration);
	bool IsActive() const { return remainingTime_ > 0.0f; }
	void Clear() { remainingTime_ = 0.0f; }

private:
	float remainingTime_ = 0.0f;
};
