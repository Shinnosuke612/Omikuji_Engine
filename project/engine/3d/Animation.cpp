// 役割: アニメーションキー補間とノードTransform適用を実装する。
#include "Animation.h"
#include "AssimpUnicodeIO.h"
#include "../utility/Logger.h"
#include "../utility/StringUtility.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

namespace {

template<typename TValue, typename TInterpolator>
TValue CalculateCurveValue(
	const AnimationCurve<TValue>& curve,
	float time,
	const TValue& defaultValue,
	TInterpolator interpolate
) {
	const auto& keyframes = curve.keyframes;
	if (keyframes.empty()) {
		return defaultValue;
	}
	if (keyframes.size() == 1 || time <= keyframes.front().time) {
		return keyframes.front().value;
	}

	const auto next = std::upper_bound(
		keyframes.begin(),
		keyframes.end(),
		time,
		[](float value, const Keyframe<TValue>& keyframe) {
			return value < keyframe.time;
		}
	);
	if (next == keyframes.end()) {
		return keyframes.back().value;
	}

	const Keyframe<TValue>& current = *(next - 1);
	const float duration = next->time - current.time;
	const float t = duration > 0.0f
		? (time - current.time) / duration
		: 0.0f;
	return interpolate(current.value, next->value, t);
}

Vector3 LerpVector3(const Vector3& start, const Vector3& end, float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	return {
		start.x + (end.x - start.x) * t,
		start.y + (end.y - start.y) * t,
		start.z + (end.z - start.z) * t
	};
}

Animation ReadAnimation(const aiAnimation& source, size_t index) {
	Animation animation{};
	animation.name = source.mName.length > 0
		? source.mName.C_Str()
		: "Animation " + std::to_string(index);
	const double ticksPerSecond = source.mTicksPerSecond != 0.0
		? source.mTicksPerSecond
		: 1.0;
	animation.duration = static_cast<float>(
		source.mDuration / ticksPerSecond
	);

	for (uint32_t channelIndex = 0;
		channelIndex < source.mNumChannels;
		++channelIndex) {
		const aiNodeAnim* nodeAnimationAssimp = source.mChannels[channelIndex];
		NodeAnimation& nodeAnimation =
			animation.nodeAnimations[nodeAnimationAssimp->mNodeName.C_Str()];

		nodeAnimation.translate.keyframes.reserve(
			nodeAnimationAssimp->mNumPositionKeys
		);
		for (uint32_t keyIndex = 0;
			keyIndex < nodeAnimationAssimp->mNumPositionKeys;
			++keyIndex) {
			const aiVectorKey& key = nodeAnimationAssimp->mPositionKeys[keyIndex];
			nodeAnimation.translate.keyframes.push_back({
				static_cast<float>(key.mTime / ticksPerSecond),
				{ -key.mValue.x, key.mValue.y, key.mValue.z }
			});
		}

		nodeAnimation.rotate.keyframes.reserve(
			nodeAnimationAssimp->mNumRotationKeys
		);
		for (uint32_t keyIndex = 0;
			keyIndex < nodeAnimationAssimp->mNumRotationKeys;
			++keyIndex) {
			const aiQuatKey& key = nodeAnimationAssimp->mRotationKeys[keyIndex];
			nodeAnimation.rotate.keyframes.push_back({
				static_cast<float>(key.mTime / ticksPerSecond),
				Normalize({
					key.mValue.x,
					-key.mValue.y,
					-key.mValue.z,
					key.mValue.w
				})
			});
		}

		nodeAnimation.scale.keyframes.reserve(
			nodeAnimationAssimp->mNumScalingKeys
		);
		for (uint32_t keyIndex = 0;
			keyIndex < nodeAnimationAssimp->mNumScalingKeys;
			++keyIndex) {
			const aiVectorKey& key = nodeAnimationAssimp->mScalingKeys[keyIndex];
			nodeAnimation.scale.keyframes.push_back({
				static_cast<float>(key.mTime / ticksPerSecond),
				{ key.mValue.x, key.mValue.y, key.mValue.z }
			});
		}
	}

	return animation;
}

} // namespace

std::vector<Animation> ReadAnimationFiles(
	const std::filesystem::path& requestedModelFilePath, std::string& diagnostic
) {
	diagnostic.clear();
	std::vector<Animation> animations;
	Assimp::Importer importer;
	const std::filesystem::path modelFilePath =
		requestedModelFilePath.lexically_normal();
	const std::string filePath = StringUtility::ToUtf8(modelFilePath);
	importer.SetIOHandler(new AssimpUnicodeIOSystem(modelFilePath.parent_path()));
	const aiScene* scene = importer.ReadFile(filePath.c_str(), 0);
	if (!scene) {
		diagnostic = "Failed to load model animations: " + filePath +
			"\nAssimp: " + importer.GetErrorString() + "\n";
		return animations;
	}
	if (scene->mNumAnimations == 0) {
		return animations;
	}

	animations.reserve(scene->mNumAnimations);
	for (uint32_t index = 0; index < scene->mNumAnimations; ++index) {
		Animation animation = ReadAnimation(*scene->mAnimations[index], index);
		if (animation.IsValid()) {
			animations.push_back(std::move(animation));
		}
	}
	return animations;
}

std::vector<Animation> LoadAnimationFiles(const std::filesystem::path& modelFilePath) {
	std::string diagnostic;
	auto animations = ReadAnimationFiles(modelFilePath, diagnostic);
	if (!diagnostic.empty()) Logger::Log(diagnostic);
	return animations;
}

void AnimationPlayer::SetAnimations(
	const std::vector<Animation>* animations
) {
	animations_ = animations;
	currentClipIndex_ = kInvalidClipIndex;
	if (animations_) {
		for (size_t index = 0; index < animations_->size(); ++index) {
			if ((*animations_)[index].IsValid()) {
				currentClipIndex_ = index;
				break;
			}
		}
	}
	Reset();
}

bool AnimationPlayer::Play(
	size_t clipIndex,
	float transitionDuration,
	bool restart
) {
	if (
		!animations_ ||
		clipIndex >= animations_->size() ||
		!(*animations_)[clipIndex].IsValid()
	) {
		return false;
	}

	if (clipIndex == currentClipIndex_) {
		if (restart) {
			currentTime_ = 0.0f;
		}
		previousClipIndex_ = kInvalidClipIndex;
		transitionTime_ = 0.0f;
		transitionDuration_ = 0.0f;
		playing_ = true;
		return true;
	}

	const bool canTransition = GetCurrentAnimation() != nullptr &&
		transitionDuration > 0.0f;
	previousClipIndex_ = canTransition
		? currentClipIndex_
		: kInvalidClipIndex;
	previousTime_ = currentTime_;
	currentClipIndex_ = clipIndex;
	currentTime_ = 0.0f;
	transitionTime_ = 0.0f;
	transitionDuration_ = canTransition
		? transitionDuration
		: 0.0f;
	playing_ = true;
	return true;
}

void AnimationPlayer::Reset() {
	currentTime_ = 0.0f;
	previousTime_ = 0.0f;
	transitionTime_ = 0.0f;
	transitionDuration_ = 0.0f;
	previousClipIndex_ = kInvalidClipIndex;
	playing_ = false;
}

void AnimationPlayer::Update(float deltaTime) {
	if (!enabled_ || !playing_ || deltaTime <= 0.0f) {
		return;
	}

	const Animation* current = GetCurrentAnimation();
	if (!current) {
		playing_ = false;
		return;
	}

	AdvanceTime(currentTime_, *current, deltaTime * speed_);
	if (const Animation* previous = GetPreviousAnimation()) {
		AdvanceTime(previousTime_, *previous, deltaTime * speed_);
		transitionTime_ += deltaTime;
		if (transitionTime_ >= transitionDuration_) {
			previousClipIndex_ = kInvalidClipIndex;
			transitionTime_ = transitionDuration_;
		}
	}

	if (
		!looping_ &&
		(
			(speed_ >= 0.0f && currentTime_ >= current->duration) ||
			(speed_ < 0.0f && currentTime_ <= 0.0f)
		)
	) {
		playing_ = false;
	}
}

void AnimationPlayer::SetTime(float time) {
	const Animation* animation = GetCurrentAnimation();
	currentTime_ = animation
		? std::clamp(time, 0.0f, animation->duration)
		: 0.0f;
	previousClipIndex_ = kInvalidClipIndex;
	transitionTime_ = 0.0f;
	transitionDuration_ = 0.0f;
}

bool AnimationPlayer::HasAnimations() const {
	return GetCurrentAnimation() != nullptr;
}

bool AnimationPlayer::IsTransitioning() const {
	return GetPreviousAnimation() != nullptr &&
		transitionDuration_ > 0.0f &&
		transitionTime_ < transitionDuration_;
}

float AnimationPlayer::GetDuration() const {
	const Animation* animation = GetCurrentAnimation();
	return animation ? animation->duration : 0.0f;
}

float AnimationPlayer::GetBlendWeight() const {
	if (!IsTransitioning()) {
		return 1.0f;
	}
	float value = std::clamp(
		transitionTime_ / transitionDuration_,
		0.0f,
		1.0f
	);
	if (blendCurve_ == AnimationBlendCurve::SmoothStep) {
		value = value * value * (3.0f - 2.0f * value);
	}
	return value;
}

const Animation* AnimationPlayer::GetCurrentAnimation() const {
	if (
		!animations_ ||
		currentClipIndex_ >= animations_->size() ||
		!(*animations_)[currentClipIndex_].IsValid()
	) {
		return nullptr;
	}
	return &(*animations_)[currentClipIndex_];
}

const Animation* AnimationPlayer::GetPreviousAnimation() const {
	if (
		!animations_ ||
		previousClipIndex_ >= animations_->size() ||
		!(*animations_)[previousClipIndex_].IsValid()
	) {
		return nullptr;
	}
	return &(*animations_)[previousClipIndex_];
}

void AnimationPlayer::AdvanceTime(
	float& time,
	const Animation& animation,
	float deltaTime
) {
	if (animation.duration <= 0.0f) {
		time = 0.0f;
		return;
	}
	time += deltaTime;
	if (looping_) {
		time = std::fmod(time, animation.duration);
		if (time < 0.0f) {
			time += animation.duration;
		}
	} else {
		time = std::clamp(time, 0.0f, animation.duration);
	}
}

Vector3 CalculateValue(
	const AnimationCurve<Vector3>& curve,
	float time,
	const Vector3& defaultValue
) {
	return CalculateCurveValue(
		curve,
		time,
		defaultValue,
		[](const Vector3& start, const Vector3& end, float t) {
			return LerpVector3(start, end, t);
		}
	);
}

Quaternion CalculateValue(
	const AnimationCurve<Quaternion>& curve,
	float time,
	const Quaternion& defaultValue
) {
	return CalculateCurveValue(
		curve,
		time,
		defaultValue,
		[](const Quaternion& start, const Quaternion& end, float t) {
			return Slerp(start, end, t);
		}
	);
}

