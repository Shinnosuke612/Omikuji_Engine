// 役割: glTFアニメーションのキー情報と補間結果を定義する。
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "../math/Quaternion.h"
#include "../math/Vector3.h"

template<typename TValue>
struct Keyframe {
	float time;
	TValue value;
};

template<typename TValue>
struct AnimationCurve {
	std::vector<Keyframe<TValue>> keyframes;
};

struct NodeAnimation {
	AnimationCurve<Vector3> translate;
	AnimationCurve<Quaternion> rotate;
	AnimationCurve<Vector3> scale;
};

struct Animation {
	std::string name;
	float duration = 0.0f;
	std::unordered_map<std::string, NodeAnimation> nodeAnimations;

	bool IsValid() const {
		return duration > 0.0f && !nodeAnimations.empty();
	}
};

enum class AnimationBlendCurve {
	Linear,
	SmoothStep
};

class AnimationPlayer {
public:
	void SetAnimations(const std::vector<Animation>* animations);
	void SetEnabled(bool enabled) { enabled_ = enabled; }
	void SetPlaying(bool playing) { playing_ = playing; }
	void SetLooping(bool looping) { looping_ = looping; }
	void SetSpeed(float speed) { speed_ = speed; }
	void SetBlendCurve(AnimationBlendCurve curve) { blendCurve_ = curve; }

	bool Play(size_t clipIndex, float transitionDuration, bool restart = true);
	void Reset();
	void Update(float deltaTime);
	void SetTime(float time);

	bool HasAnimations() const;
	bool IsEnabled() const { return enabled_; }
	bool IsPlaying() const { return playing_; }
	bool IsLooping() const { return looping_; }
	bool IsTransitioning() const;
	float GetSpeed() const { return speed_; }
	float GetTime() const { return currentTime_; }
	float GetDuration() const;
	float GetBlendWeight() const;
	size_t GetCurrentClipIndex() const { return currentClipIndex_; }
	AnimationBlendCurve GetBlendCurve() const { return blendCurve_; }
	const Animation* GetCurrentAnimation() const;
	const Animation* GetPreviousAnimation() const;
	float GetPreviousTime() const { return previousTime_; }

private:
	static constexpr size_t kInvalidClipIndex = static_cast<size_t>(-1);

	void AdvanceTime(float& time, const Animation& animation, float deltaTime);

	const std::vector<Animation>* animations_ = nullptr;
	size_t currentClipIndex_ = kInvalidClipIndex;
	size_t previousClipIndex_ = kInvalidClipIndex;
	float currentTime_ = 0.0f;
	float previousTime_ = 0.0f;
	float transitionTime_ = 0.0f;
	float transitionDuration_ = 0.0f;
	float speed_ = 1.0f;
	bool enabled_ = true;
	bool playing_ = false;
	bool looping_ = true;
	AnimationBlendCurve blendCurve_ = AnimationBlendCurve::SmoothStep;
};

std::vector<Animation> LoadAnimationFiles(
	const std::filesystem::path& modelFilePath
);

// CPU読込。クリップが無い場合は空配列、読込失敗の場合は診断も返す。
std::vector<Animation> ReadAnimationFiles(
	const std::filesystem::path& resolvedModelPath, std::string& diagnostic
);

Vector3 CalculateValue(
	const AnimationCurve<Vector3>& curve,
	float time,
	const Vector3& defaultValue
);

Quaternion CalculateValue(
	const AnimationCurve<Quaternion>& curve,
	float time,
	const Quaternion& defaultValue
);

