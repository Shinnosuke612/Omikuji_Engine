// 役割: 三人称カメラの追従、遮蔽回避、マウス入力反映を実装する。
#define NOMINMAX
#include "ThirdPersonCameraController.h"

#include "../../engine/3d/Camera.h"
#include "../../engine/collision/OBBCollider.h"
#include "../../engine/io/Input.h"
#include "../../engine/math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
	constexpr float kTwoPi = 6.28318530718f;

	float NormalizeAngle(float radians) {
		return std::remainder(radians, kTwoPi);
	}

	float SmoothAlpha(float smoothTime, float deltaTime) {
		if (smoothTime <= 0.000001f) {
			return 1.0f;
		}
		return 1.0f - std::exp(-(std::max)(deltaTime, 0.0f) / smoothTime);
	}
}

void ThirdPersonCameraController::Initialize(Camera* camera) {
	camera_ = camera;
	focusInitialized_ = false;
	isAimMode_ = false;
	targetYawInitialized_ = false;
	occlusionDistanceInitialized_ = false;
	occlusionActive_ = false;
	distance_ = normalDistance_;
	targetDistance_ = normalDistance_;
	if (camera_) {
		camera_->SetOrbitMode(false);
	}
}

void ThirdPersonCameraController::SetYawPitch(float yaw, float pitch) {
	yaw_ = yaw;
	pitch_ = std::clamp(pitch, minPitch_, maxPitch_);
	desiredYaw_ = yaw_;
	desiredPitch_ = pitch_;
}

void ThirdPersonCameraController::SyncFromCameraPose(
	const Vector3& cameraPosition,
	const Vector3& focusPosition
) {
	currentFocus_ = focusPosition;
	focusInitialized_ = true;

	const Vector3 toFocus = Math::Subtract(focusPosition, cameraPosition);
	const float distance = Math::Length(toFocus);
	if (distance <= 0.0001f) {
		distance_ = minDistance_;
		targetDistance_ = distance_;
		normalDistance_ = distance_;
		return;
	}

	const Vector3 forward = Math::Multiply(toFocus, 1.0f / distance);
	yaw_ = std::atan2(forward.x, forward.z);
	pitch_ = std::clamp(
		std::asin(std::clamp(-forward.y, -1.0f, 1.0f)),
		minPitch_,
		maxPitch_
	);
	desiredYaw_ = yaw_;
	desiredPitch_ = pitch_;

	distance_ = std::clamp(distance, minDistance_, maxDistance_);
	targetDistance_ = distance_;
	normalDistance_ = distance_;
	occlusionDistance_ = distance_;
	occlusionDistanceInitialized_ = true;
	occlusionActive_ = false;
}

Vector3 ThirdPersonCameraController::GetForwardDirection() const {
	Vector3 forward = {
		std::sin(yaw_) * std::cos(pitch_),
		-std::sin(pitch_),
		std::cos(yaw_) * std::cos(pitch_)
	};
	if (Math::Length(forward) < 0.000001f) {
		return { 0.0f, 0.0f, 1.0f };
	}
	return Math::Normalize(forward);
}

void ThirdPersonCameraController::SetDistance(float distance) {
	normalDistance_ = std::clamp(distance, minDistance_, maxDistance_);
	targetDistance_ = normalDistance_;
	distance_ = normalDistance_;
	occlusionDistance_ = distance_;
	occlusionDistanceInitialized_ = false;
	occlusionActive_ = false;
}

void ThirdPersonCameraController::SetAimDistance(float distance) {
	aimDistance_ = std::clamp(distance, minDistance_, maxDistance_);
}

void ThirdPersonCameraController::SetPitchLimit(float minPitch, float maxPitch) {
	minPitch_ = std::min(minPitch, maxPitch);
	maxPitch_ = std::max(minPitch, maxPitch);
	pitch_ = std::clamp(pitch_, minPitch_, maxPitch_);
	desiredPitch_ = std::clamp(
		desiredPitch_,
		minPitch_,
		maxPitch_
	);
}

void ThirdPersonCameraController::Update(
	const Vector3& targetPosition,
	float targetYaw,
	const std::vector<OBBCollider*>& obstacleColliders,
	float deltaTime,
	bool acceptMouseInput,
	bool acceptWheelZoom
) {
	if (!camera_) {
		return;
	}

	Input* input = Input::GetInstance();
	if (followTargetYaw_) {
		if (!targetYawInitialized_) {
			previousTargetYaw_ = targetYaw;
			targetYawInitialized_ = true;
		} else {
			desiredYaw_ += NormalizeAngle(targetYaw - previousTargetYaw_);
			previousTargetYaw_ = targetYaw;
		}
	}

	if (acceptMouseInput) {
		isAimMode_ = aimModeEnabled_ && input &&
			input->PushMouse(Input::MouseButton::Right);
		if (input) {
			const Vector2 mouseMove = input->GetMouseMove();
			desiredYaw_ += mouseMove.x * mouseSensitivity_ *
				(invertYaw_ ? -1.0f : 1.0f);
			desiredPitch_ += mouseMove.y * mouseSensitivity_ *
				(invertPitch_ ? 1.0f : -1.0f);
			desiredPitch_ = std::clamp(
				desiredPitch_,
				minPitch_,
				maxPitch_
			);

			if (!isAimMode_ && acceptWheelZoom) {
				const float wheel = input->GetMouseWheel();
				if (std::abs(wheel) > 0.000001f) {
					targetDistance_ = std::clamp(
						targetDistance_ - wheel * zoomStep_,
						minDistance_,
						maxDistance_
					);
					normalDistance_ = targetDistance_;
				}
			}
		}
	}
	const float rotationAlpha = SmoothAlpha(rotationSmoothTime_, deltaTime);
	yaw_ += NormalizeAngle(desiredYaw_ - yaw_) * rotationAlpha;
	pitch_ += (desiredPitch_ - pitch_) * rotationAlpha;
	pitch_ = std::clamp(pitch_, minPitch_, maxPitch_);

	const Vector3 desiredFocus = Math::Add(
		targetPosition,
		isAimMode_ ? aimTargetOffset_ : targetOffset_
	);

	if (!focusInitialized_) {
		currentFocus_ = desiredFocus;
		focusInitialized_ = true;
	} else {
		const float positionAlpha = SmoothAlpha(
			positionSmoothTime_,
			deltaTime
		);
		currentFocus_ = Math::Add(
			currentFocus_,
			Math::Multiply(
				Math::Subtract(desiredFocus, currentFocus_),
				positionAlpha
			)
		);
	}

	const float desiredDistance = isAimMode_ ? aimDistance_ : targetDistance_;
	distance_ += (desiredDistance - distance_) * SmoothAlpha(
		positionSmoothTime_,
		deltaTime
	);
	distance_ = std::clamp(distance_, minDistance_, maxDistance_);

	const float cosPitch = std::cos(pitch_);
	const float sinPitch = std::sin(pitch_);
	const float sinYaw = std::sin(yaw_);
	const float cosYaw = std::cos(yaw_);

	const Vector3 desiredOffset = {
		-distance_ * sinYaw * cosPitch,
		distance_ * sinPitch,
		-distance_ * cosYaw * cosPitch
	};
	const Vector3 desiredCameraPosition = Math::Add(
		currentFocus_,
		desiredOffset
	);

	Vector3 ray = Math::Subtract(desiredCameraPosition, currentFocus_);
	const float rayLength = Math::Length(ray);
	Vector3 cameraPosition = desiredCameraPosition;
	if (rayLength > 0.0001f) {
		const Vector3 direction = Math::Multiply(ray, 1.0f / rayLength);
		float cameraDistance = rayLength;
		if (occlusionEnabled_) {
			const float safeDistance = ResolveOcclusionDistance(
				currentFocus_,
				desiredCameraPosition,
				obstacleColliders
			);
			if (!occlusionDistanceInitialized_) {
				occlusionDistance_ = rayLength;
				occlusionDistanceInitialized_ = true;
			}

			const bool obstructed = safeDistance < rayLength - 0.0001f;
			if (obstructed) {
				occlusionActive_ = true;
			}
			if (obstructed || occlusionActive_) {
				// 以前は安全距離を即時適用していたため、ColliderがRayへ入った
				// 1FrameでCameraが跳ねた。寄りと復帰を別速度で補間する。
				const float desiredOcclusionDistance = obstructed
					? safeDistance
					: rayLength;
				const float smoothTime =
					desiredOcclusionDistance < occlusionDistance_
					? occlusionPullInSmoothTime_
					: occlusionRecoverySmoothTime_;
				occlusionDistance_ +=
					(desiredOcclusionDistance - occlusionDistance_) * SmoothAlpha(
						smoothTime, deltaTime
					);
				if (
					!obstructed &&
					std::abs(rayLength - occlusionDistance_) <= 0.001f
				) {
					occlusionDistance_ = rayLength;
					occlusionActive_ = false;
				}
			} else {
				// 通常Zoomの補間へ二重に遅延を加えない。
				occlusionDistance_ = rayLength;
			}
			occlusionDistance_ = std::clamp(
				occlusionDistance_,
				minDistance_,
				rayLength
			);
			cameraDistance = occlusionDistance_;
		} else {
			occlusionDistance_ = rayLength;
			occlusionDistanceInitialized_ = true;
			occlusionActive_ = false;
		}
		cameraPosition = Math::Add(
			currentFocus_,
			Math::Multiply(direction, cameraDistance)
		);
	}

	for (int iteration = 0; iteration < 3; ++iteration) {
		bool pushed = false;
		for (const OBBCollider* collider : obstacleColliders) {
			if (!collider) {
				continue;
			}
			const Vector3 closest = ClosestPointOnOBB(cameraPosition, *collider);
			Vector3 diff = Math::Subtract(cameraPosition, closest);
			const float distSq = Math::Dot(diff, diff);
			const float radiusSq = cameraBodyRadius_ * cameraBodyRadius_;
			if (distSq >= radiusSq) {
				continue;
			}
			float dist = std::sqrt(std::max(distSq, 0.000001f));
			Vector3 pushDir = dist > 0.0001f
				? Math::Multiply(diff, 1.0f / dist)
				: Vector3{ 0.0f, 1.0f, 0.0f };
			cameraPosition = Math::Add(
				cameraPosition,
				Math::Multiply(
					pushDir,
					cameraBodyRadius_ - dist + 0.02f
				)
			);
			pushed = true;
		}
		if (!pushed) {
			break;
		}
	}

	camera_->SetLookAt(cameraPosition, currentFocus_);
}

Vector3 ThirdPersonCameraController::ClosestPointOnOBB(
	const Vector3& point,
	const OBBCollider& collider
) const {
	const OBBCollider::OBB obb = collider.GetOBB();
	Vector3 delta = Math::Subtract(point, obb.center);
	Vector3 closest = obb.center;
	const float halfSizes[3] = {
		obb.halfSize.x,
		obb.halfSize.y,
		obb.halfSize.z
	};
	for (uint32_t index = 0; index < 3; ++index) {
		const float distance = std::clamp(
			Math::Dot(delta, obb.axis[index]),
			-halfSizes[index],
			halfSizes[index]
		);
		closest = Math::Add(
			closest,
			Math::Multiply(obb.axis[index], distance)
		);
	}
	return closest;
}

float ThirdPersonCameraController::ResolveOcclusionDistance(
	const Vector3& focus,
	const Vector3& desiredCameraPosition,
	const std::vector<OBBCollider*>& obstacleColliders
) const {
	Vector3 ray = Math::Subtract(desiredCameraPosition, focus);
	const float desiredLength = Math::Length(ray);
	if (desiredLength <= 0.0001f) {
		return minDistance_;
	}

	const Vector3 direction = Math::Multiply(ray, 1.0f / desiredLength);
	Vector3 right = {
		std::cos(yaw_),
		0.0f,
		-std::sin(yaw_)
	};
	if (Math::Length(right) < 0.000001f) {
		right = { 1.0f, 0.0f, 0.0f };
	} else {
		right = Math::Normalize(right);
	}
	Vector3 up = Math::Cross(direction, right);
	if (Math::Length(up) < 0.000001f) {
		up = { 0.0f, 1.0f, 0.0f };
	} else {
		up = Math::Normalize(up);
	}

	constexpr float kHorizontalProbe = 0.08f;
	constexpr float kVerticalProbe = 0.08f;
	const Vector3 origins[] = {
		focus,
		Math::Add(focus, Math::Multiply(up, kVerticalProbe)),
		Math::Add(focus, Math::Multiply(up, -kVerticalProbe)),
		Math::Add(focus, Math::Multiply(right, kHorizontalProbe)),
		Math::Add(focus, Math::Multiply(right, -kHorizontalProbe))
	};

	float nearest = std::numeric_limits<float>::max();
	bool hit = false;
	for (const Vector3& origin : origins) {
		for (const OBBCollider* collider : obstacleColliders) {
			if (!collider) {
				continue;
			}
			float distance = 0.0f;
			if (RayIntersectOBB(
				origin,
				direction,
				desiredLength,
				*collider,
				distance
			)) {
				nearest = std::min(nearest, distance);
				hit = true;
			}
		}
	}

	if (!hit) {
		return desiredLength;
	}

	return std::clamp(
		nearest - occlusionMargin_,
		minDistance_,
		desiredLength
	);
}

bool ThirdPersonCameraController::RayIntersectOBB(
	const Vector3& origin,
	const Vector3& direction,
	float maxDistance,
	const OBBCollider& collider,
	float& outDistance
) const {
	const OBBCollider::OBB obb = collider.GetOBB();
	Vector3 delta = Math::Subtract(obb.center, origin);

	float tMin = 0.0f;
	float tMax = maxDistance;
	const float halfSizes[3] = {
		obb.halfSize.x,
		obb.halfSize.y,
		obb.halfSize.z
	};

	for (uint32_t axisIndex = 0; axisIndex < 3; ++axisIndex) {
		const Vector3& axis = obb.axis[axisIndex];
		const float e = Math::Dot(axis, delta);
		const float f = Math::Dot(axis, direction);

		if (std::abs(f) > 0.000001f) {
			float t1 = (e + halfSizes[axisIndex]) / f;
			float t2 = (e - halfSizes[axisIndex]) / f;
			if (t1 > t2) {
				std::swap(t1, t2);
			}
			tMin = std::max(tMin, t1);
			tMax = std::min(tMax, t2);
			if (tMin > tMax) {
				return false;
			}
		} else if (
			-e - halfSizes[axisIndex] > 0.0f ||
			-e + halfSizes[axisIndex] < 0.0f
		) {
			return false;
		}
	}

	outDistance = tMin;
	return outDistance >= 0.0f && outDistance <= maxDistance;
}
