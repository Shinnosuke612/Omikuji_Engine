// 役割: CameraPathPointから経路を収集し、時間に応じたカメラ姿勢を計算する。
#include "CameraPathRuntime.h"

#include "Camera.h"
#include "../math/Math.h"
#include "../math/Matrix4x4.h"
#include "../math/Quaternion.h"
#include "../scene/SceneDocument.h"
#include "../scene/SceneEntityQuery.h"
#include "../scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneTransformResolver::ResolveScene3DTransform;

	Transform ResolveCameraWorldTransform(const Camera& camera) {
		Transform result{};
		result.scale = { 1.0f, 1.0f, 1.0f };
		result.translate = camera.GetTranslate();
		result.rotate = camera.GetRotate();
		result.useQuaternionRotation = true;
		result.quaternionRotate = camera.UsesQuaternionRotation()
			? camera.GetRotateQuaternion()
			: MakeQuaternionFromEuler(camera.GetRotate());

		Vector3 scale{};
		Quaternion rotate = MakeIdentityQuaternion();
		Vector3 translate{};
		if (DecomposeAffineMatrix(
			camera.GetWorldMatrix(),
			scale,
			rotate,
			translate
		)) {
			result.scale = { 1.0f, 1.0f, 1.0f };
			result.rotate = MakeEulerFromQuaternion(rotate);
			result.translate = translate;
			result.quaternionRotate = rotate;
		}
		return result;
	}

	float ApplyEasing(float t, const std::string& easing) {
		t = std::clamp(t, 0.0f, 1.0f);
		if (easing == "EaseIn") {
			return t * t;
		}
		if (easing == "EaseOut") {
			const float inverse = 1.0f - t;
			return 1.0f - inverse * inverse;
		}
		if (easing == "EaseInOut") {
			return t < 0.5f
				? 2.0f * t * t
				: 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
		}
		if (easing == "SmoothStep") {
			return t * t * (3.0f - 2.0f * t);
		}
		return t;
	}

	float Lerp(float a, float b, float t) {
		return a + (b - a) * t;
	}

	Transform LerpTransform(
		const Transform& a,
		const Transform& b,
		float t
	) {
		Transform result = a;
		result.translate = {
			Lerp(a.translate.x, b.translate.x, t),
			Lerp(a.translate.y, b.translate.y, t),
			Lerp(a.translate.z, b.translate.z, t)
		};
		const Quaternion aRotate = a.useQuaternionRotation
			? a.quaternionRotate
			: MakeQuaternionFromEuler(a.rotate);
		const Quaternion bRotate = b.useQuaternionRotation
			? b.quaternionRotate
			: MakeQuaternionFromEuler(b.rotate);
		result.quaternionRotate = Slerp(aRotate, bRotate, t);
		result.useQuaternionRotation = true;
		result.rotate = MakeEulerFromQuaternion(result.quaternionRotate);
		result.scale = {
			Lerp(a.scale.x, b.scale.x, t),
			Lerp(a.scale.y, b.scale.y, t),
			Lerp(a.scale.z, b.scale.z, t)
		};
		return result;
	}

	Vector3 CatmullRom(
		const Vector3& p0,
		const Vector3& p1,
		const Vector3& p2,
		const Vector3& p3,
		float t
	) {
		const float t2 = t * t;
		const float t3 = t2 * t;
		return {
			0.5f * (
				2.0f * p1.x +
				(-p0.x + p2.x) * t +
				(2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
				(-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3
			),
			0.5f * (
				2.0f * p1.y +
				(-p0.y + p2.y) * t +
				(2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
				(-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3
			),
			0.5f * (
				2.0f * p1.z +
				(-p0.z + p2.z) * t +
				(2.0f * p0.z - 5.0f * p1.z + 4.0f * p2.z - p3.z) * t2 +
				(-p0.z + 3.0f * p1.z - 3.0f * p2.z + p3.z) * t3
			)
		};
	}
}

void CameraPathRuntime::Play(
	const SceneDocument& document,
	const SceneEntity& pathEntity,
	const SceneComponent& pathComponent,
	const Camera& currentCamera
) {
	finishedThisFrame_ = false;
	points_.clear();
	for (const SceneEntity& entity : document.GetEntities()) {
		if (entity.parentId != pathEntity.id) {
			continue;
		}
		const SceneComponent* pointComponent =
			FindEnabledComponent(entity, "CameraPathPoint");
		if (!pointComponent) {
			continue;
		}
		PointSample sample{};
		sample.transform = ResolveScene3DTransform(document, entity);
		sample.durationToNext =
			(std::max)(pointComponent->cameraPathPointDurationToNext, 0.0f);
		sample.easingToNext =
			pointComponent->cameraPathPointEasingToNext.empty()
			? pathComponent.cameraPathDefaultEasing
			: pointComponent->cameraPathPointEasingToNext;
		sample.entityId = entity.id;
		points_.push_back(sample);
	}

	if (points_.empty()) {
		Stop();
		return;
	}

	startTransform_ = ResolveCameraWorldTransform(currentCamera);
	returnTransform_ = startTransform_;
	if (!pathComponent.cameraPathStartFromCurrentCamera) {
		startTransform_ = points_.front().transform;
	}
	enterDuration_ = pathComponent.cameraPathStartFromCurrentCamera
		? (std::max)(pathComponent.cameraPathEnterDuration, 0.0f)
		: 0.0f;
	exitDuration_ = (std::max)(pathComponent.cameraPathExitDuration, 0.0f);
	enterEasing_ = pathComponent.cameraPathDefaultEasing;
	exitEasing_ = pathComponent.cameraPathDefaultEasing;
	returnToPreviousCamera_ = pathComponent.cameraPathReturnToPreviousCamera;
	interpolation_ = pathComponent.cameraPathInterpolation.empty()
		? "Linear"
		: pathComponent.cameraPathInterpolation;
	elapsed_ = 0.0f;
	pathElapsed_ = 0.0f;
	hasCurrentTransform_ = false;
	phase_ = enterDuration_ > 0.0f ? Phase::Enter : Phase::Path;
}

void CameraPathRuntime::Stop() {
	phase_ = Phase::Finished;
	elapsed_ = 0.0f;
	pathElapsed_ = 0.0f;
	points_.clear();
	hasCurrentTransform_ = false;
	finishedThisFrame_ = false;
}

void CameraPathRuntime::Update(float deltaTime, Camera& camera) {
	finishedThisFrame_ = false;
	if (phase_ == Phase::Finished) {
		return;
	}

	deltaTime = (std::max)(deltaTime, 0.0f);

	Transform evaluated{};
	if (phase_ == Phase::Enter) {
		const float t = enterDuration_ <= 0.0f
			? 1.0f
			: std::clamp(elapsed_ / enterDuration_, 0.0f, 1.0f);
		evaluated = LerpTransform(
			startTransform_,
			points_.front().transform,
			ApplyEasing(t, enterEasing_)
		);
		elapsed_ += deltaTime;
		if (enterDuration_ <= 0.0f || elapsed_ >= enterDuration_) {
			phase_ = Phase::Path;
			elapsed_ = 0.0f;
			pathElapsed_ = 0.0f;
		}
	} else if (phase_ == Phase::Path) {
		const float duration = GetPathDuration();
		evaluated = EvaluatePath((std::min)(pathElapsed_, duration));
		pathElapsed_ += deltaTime;
		if (duration <= 0.0f || pathElapsed_ >= duration) {
			phase_ =
				returnToPreviousCamera_ && exitDuration_ > 0.0f
				? Phase::Exit
				: Phase::Finished;
			elapsed_ = 0.0f;
			if (phase_ == Phase::Finished) {
				evaluated = returnToPreviousCamera_
					? returnTransform_
					: points_.back().transform;
				finishedThisFrame_ = true;
			}
		}
	} else if (phase_ == Phase::Exit) {
		const float t = exitDuration_ <= 0.0f
			? 1.0f
			: std::clamp(elapsed_ / exitDuration_, 0.0f, 1.0f);
		evaluated = LerpTransform(
			points_.back().transform,
			returnTransform_,
			ApplyEasing(t, exitEasing_)
		);
		elapsed_ += deltaTime;
		if (exitDuration_ <= 0.0f || elapsed_ >= exitDuration_) {
			phase_ = Phase::Finished;
			evaluated = returnTransform_;
			finishedThisFrame_ = true;
		}
	}

	currentTransform_ = evaluated;
	hasCurrentTransform_ = true;
	ApplyTransform(camera, evaluated);
}

void CameraPathRuntime::UpdateDeathContinuation(float worldDelta, Camera& camera) {
	if (!std::isfinite(worldDelta) || worldDelta <= 0.0f || phase_ == Phase::Finished || points_.empty()) {
		return;
	}
	finishedThisFrame_ = false;
	float remaining = worldDelta;
	// Enter/Path/Exitは各一度だけ通過する。ゼロ秒区間も無限ループにしない。
	for (int stage = 0; stage < 3 && phase_ != Phase::Finished; ++stage) {
		const float duration = phase_ == Phase::Enter ? enterDuration_
			: phase_ == Phase::Path ? GetPathDuration() : exitDuration_;
		float& elapsed = phase_ == Phase::Path ? pathElapsed_ : elapsed_;
		if (!std::isfinite(duration) || !std::isfinite(elapsed)) {
			return; // 最後の有効姿勢を維持し、通常経路への復帰を要求しない。
		}
		const float end = (std::max)(duration, 0.0f);
		const float used = (std::min)(remaining, (std::max)(end - elapsed, 0.0f));
		elapsed = (std::min)(elapsed + used, end);
		remaining -= used;
		const float t = end > 0.0f ? std::clamp(elapsed / end, 0.0f, 1.0f) : 1.0f;
		Transform evaluated{};
		if (phase_ == Phase::Enter) {
			evaluated = LerpTransform(startTransform_, points_.front().transform, ApplyEasing(t, enterEasing_));
		} else if (phase_ == Phase::Path) {
			evaluated = EvaluatePath(elapsed);
		} else {
			evaluated = LerpTransform(points_.back().transform, returnTransform_, ApplyEasing(t, exitEasing_));
		}
		const bool reachedEnd = elapsed >= end;
		if (reachedEnd) {
			if (phase_ == Phase::Enter) {
				phase_ = Phase::Path;
				elapsed_ = pathElapsed_ = 0.0f;
			} else if (phase_ == Phase::Path && returnToPreviousCamera_ && exitDuration_ > 0.0f) {
				phase_ = Phase::Exit;
				elapsed_ = 0.0f;
			} else {
				phase_ = Phase::Finished;
				evaluated = returnToPreviousCamera_ ? returnTransform_ : points_.back().transform;
			}
		}
		currentTransform_ = evaluated;
		hasCurrentTransform_ = true;
		ApplyTransform(camera, evaluated);
		if (!reachedEnd) break;
	}
}

void CameraPathRuntime::ApplyTransform(
	Camera& camera,
	const Transform& transform
) {
	camera.SetOrbitMode(false);
	camera.SetTranslate(transform.translate);
	if (transform.useQuaternionRotation) {
		camera.SetRotateQuaternion(transform.quaternionRotate);
	} else {
		camera.SetRotate(transform.rotate);
	}
	camera.UpdatePreviewMatrices();
}

Transform CameraPathRuntime::EvaluatePath(float elapsed) const {
	if (points_.empty()) {
		return {};
	}
	if (points_.size() == 1) {
		return points_.front().transform;
	}

	float cursor = 0.0f;
	for (size_t index = 0; index + 1 < points_.size(); ++index) {
		const float duration = (std::max)(points_[index].durationToNext, 0.0f);
		if (duration <= 0.0f) {
			continue;
		}
		if (elapsed <= cursor + duration) {
			const float localT = (elapsed - cursor) / duration;
			Transform result = LerpTransform(
				points_[index].transform,
				points_[index + 1].transform,
				ApplyEasing(localT, points_[index].easingToNext)
			);
			if (interpolation_ == "CatmullRom") {
				const float easedT = ApplyEasing(
					localT,
					points_[index].easingToNext
				);
				const size_t p0Index = index == 0 ? index : index - 1;
				const size_t p1Index = index;
				const size_t p2Index = index + 1;
				const size_t p3Index = (std::min)(
					index + 2,
					points_.size() - 1
				);
				result.translate = CatmullRom(
					points_[p0Index].transform.translate,
					points_[p1Index].transform.translate,
					points_[p2Index].transform.translate,
					points_[p3Index].transform.translate,
					easedT
				);
			}
			return result;
		}
		cursor += duration;
	}
	return points_.back().transform;
}

float CameraPathRuntime::GetPathDuration() const {
	if (points_.size() < 2) {
		return 0.0f;
	}
	float duration = 0.0f;
	for (size_t index = 0; index + 1 < points_.size(); ++index) {
		duration += (std::max)(points_[index].durationToNext, 0.0f);
	}
	return duration;
}
