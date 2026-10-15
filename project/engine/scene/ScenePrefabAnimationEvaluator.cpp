// 役割: Transform／Active TrackをRuntimeとEditor Previewで共通評価する。
#include "ScenePrefabAnimationEvaluator.h"

#include "SceneDocument.h"
#include "../math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
	SceneEntity* ResolveTrackTarget(
		SceneDocument& document,
		const SceneAnimationTrack& track,
		uint64_t ownerEntityId
	) {
		if (track.targetEntityId != 0) {
			if (SceneEntity* entity = document.FindEntity(track.targetEntityId)) {
				return entity;
			}
		}
		if (!track.targetEntityName.empty()) {
			if (SceneEntity* entity =
				document.FindEntityByName(track.targetEntityName)) {
				return entity;
			}
		}
		return document.FindEntity(ownerEntityId);
	}

	float ApplyEasing(float value, const std::string& easing) {
		value = std::clamp(value, 0.0f, 1.0f);
		if (easing == "Linear") {
			return value;
		}
		if (easing == "EaseIn") {
			return value * value * value;
		}
		if (easing == "EaseOut") {
			return Math::EaseOutCubic(value);
		}
		if (easing == "EaseInOut") {
			return value < 0.5f
				? 4.0f * value * value * value
				: 1.0f - std::pow(-2.0f * value + 2.0f, 3.0f) * 0.5f;
		}
		return Math::SmoothStep(value);
	}

	const std::string& ResolveSegmentEasing(
		const SceneAnimationTrack& track,
		const SceneAnimationKeyframe& startKey
	) {
		return startKey.easingToNext.empty()
			? track.easing
			: startKey.easingToNext;
	}

	Vector3 SampleTrack(
		const SceneAnimationTrack& track,
		float time,
		bool applyPositionBulge
	) {
		if (track.keyframes.empty()) {
			return {};
		}
		if (time <= track.keyframes.front().time) {
			return track.keyframes.front().value;
		}
		for (size_t index = 1; index < track.keyframes.size(); ++index) {
			const SceneAnimationKeyframe& next = track.keyframes[index];
			if (time > next.time) {
				continue;
			}
			const SceneAnimationKeyframe& previous = track.keyframes[index - 1];
			const float duration = (std::max)(
				next.time - previous.time,
				0.0001f
			);
			const float amount = ApplyEasing(
				(time - previous.time) / duration,
				ResolveSegmentEasing(track, previous)
			);
			Vector3 result{
				Math::Lerp(previous.value.x, next.value.x, amount),
				Math::Lerp(previous.value.y, next.value.y, amount),
				Math::Lerp(previous.value.z, next.value.z, amount)
			};
			if (applyPositionBulge) {
				// 4t(1-t) makes positionBulge the exact offset at the
				// interpolation midpoint while preserving both Pose endpoints.
				const float bulgeAmount = 4.0f * amount * (1.0f - amount);
				result.x += previous.positionBulge.x * bulgeAmount;
				result.y += previous.positionBulge.y * bulgeAmount;
				result.z += previous.positionBulge.z * bulgeAmount;
			}
			return result;
		}
		return track.keyframes.back().value;
	}

	Quaternion SampleRotation(const SceneAnimationTrack& track, float time) {
		if (track.keyframes.empty()) {
			return Quaternion{};
		}
		if (time <= track.keyframes.front().time) {
			return MakeQuaternionFromEuler(track.keyframes.front().value);
		}
		for (size_t index = 1; index < track.keyframes.size(); ++index) {
			const SceneAnimationKeyframe& next = track.keyframes[index];
			if (time > next.time) {
				continue;
			}
			const SceneAnimationKeyframe& previous = track.keyframes[index - 1];
			const float duration = (std::max)(
				next.time - previous.time,
				0.0001f
			);
			const float amount = ApplyEasing(
				(time - previous.time) / duration,
				ResolveSegmentEasing(track, previous)
			);
			return Slerp(
				MakeQuaternionFromEuler(previous.value),
				MakeQuaternionFromEuler(next.value),
				amount
			);
		}
		return MakeQuaternionFromEuler(track.keyframes.back().value);
	}
}

bool ScenePrefabAnimationEvaluator::TrySampleContinuousEuler(
	const SceneAnimationTrack& track,
	float time,
	Vector3& value
) {
	if (track.property != "LocalRotation" || track.rotationInterpolation != "EulerContinuous" ||
		track.keyframes.empty() || !std::isfinite(time)) {
		return false;
	}
	float previousTime = -1.0f;
	for (const auto& key : track.keyframes) {
		if (!std::isfinite(key.time) || key.time < 0.0f || key.time <= previousTime ||
			!std::isfinite(key.value.x) || !std::isfinite(key.value.y) || !std::isfinite(key.value.z)) {
			return false;
		}
		previousTime = key.time;
	}
	Vector3 candidate = track.keyframes.back().value;
	if (time <= track.keyframes.front().time) {
		candidate = track.keyframes.front().value;
	} else {
		for (size_t index = 1; index < track.keyframes.size(); ++index) {
			const auto& next = track.keyframes[index];
			if (time > next.time) continue;
			const auto& previous = track.keyframes[index - 1];
			if (time == next.time) {
				// 極端な累積角の差分計算でも、保存端点を丸めで失わない。
				candidate = next.value;
				break;
			}
			// 有限float同士の差もfloatではoverflowし得る。中間計算だけdoubleを使う。
			const double duration = (std::max)(
				static_cast<double>(next.time) - previous.time, static_cast<double>(0.0001f)
			);
			const float amount = ApplyEasing(
				static_cast<float>((static_cast<double>(time) - previous.time) / duration),
				ResolveSegmentEasing(track, previous)
			);
			const auto interpolate = [amount](float start, float end, float& output) {
				// wrap/最短角差を使わず、回転数と符号を残したまま各軸を補間する。
				const double result = static_cast<double>(start) +
					(static_cast<double>(end) - start) * amount;
				if (!std::isfinite(result) ||
					std::abs(result) > (std::numeric_limits<float>::max)()) return false;
				output = static_cast<float>(result);
				return std::isfinite(output);
			};
			if (!interpolate(previous.value.x, next.value.x, candidate.x) ||
				!interpolate(previous.value.y, next.value.y, candidate.y) ||
				!interpolate(previous.value.z, next.value.z, candidate.z)) return false;
			break;
		}
	}
	value = candidate;
	return true;
}

void ScenePrefabAnimationEvaluator::ApplyClip(
	SceneDocument& document,
	uint64_t ownerEntityId,
	const ScenePrefabAnimationClip& clip,
	float time
) {
	for (const SceneAnimationTrack& track : clip.tracks) {
		SceneEntity* target = ResolveTrackTarget(
			document,
			track,
			ownerEntityId
		);
		if (!target || track.keyframes.empty()) {
			continue;
		}
		if (track.property == "LocalPosition") {
			target->transform.translate = SampleTrack(track, time, true);
		} else if (track.property == "LocalScale") {
			target->transform.scale = SampleTrack(track, time, false);
		} else if (track.property == "LocalRotation") {
			if (track.rotationInterpolation == "EulerContinuous") {
				Vector3 euler{};
				if (!TrySampleContinuousEuler(track, time, euler)) continue;
				// Quaternion化は補間後だけ。Transformには姿勢を、Trackには周回数を保持する。
				const Quaternion rotation = MakeQuaternionFromEuler(euler);
				if (std::isfinite(rotation.x) && std::isfinite(rotation.y) &&
					std::isfinite(rotation.z) && std::isfinite(rotation.w)) {
					target->transform.rotate = rotation;
				}
			} else {
				target->transform.rotate = SampleRotation(track, time);
			}
		} else if (track.property == "Active") {
			Vector3 value = track.keyframes.front().value;
			for (const SceneAnimationKeyframe& keyframe : track.keyframes) {
				if (keyframe.time > time) {
					break;
				}
				value = keyframe.value;
			}
			target->active = value.x >= 0.5f;
		}
	}
}
