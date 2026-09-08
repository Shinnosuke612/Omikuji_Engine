// 役割: 三人称カメラの距離、注視点、入力による回転を管理する。
#pragma once

#include <vector>

#include "../../engine/math/Vector3.h"

class Camera;
class OBBCollider;

class ThirdPersonCameraController {
public:
	void Initialize(Camera* camera);
	void Update(
		const Vector3& targetPosition,
		float targetYaw,
		const std::vector<OBBCollider*>& obstacleColliders,
		float deltaTime,
		bool acceptMouseInput,
		bool acceptWheelZoom
	);

	void SetYawPitch(float yaw, float pitch);
	void SyncFromCameraPose(
		const Vector3& cameraPosition,
		const Vector3& focusPosition
	);
	float GetYaw() const { return yaw_; }
	float GetPitch() const { return pitch_; }
	Vector3 GetForwardDirection() const;
	bool IsAimMode() const { return isAimMode_; }
	bool IsFirstPersonMode() const {
		return isAimMode_ && distance_ <= minDistance_ + 0.05f;
	}

	void SetDistance(float distance);
	void SetAimDistance(float distance);
	void SetTargetOffset(const Vector3& offset) { targetOffset_ = offset; }
	void SetAimTargetOffset(const Vector3& offset) { aimTargetOffset_ = offset; }
	void SetMouseSensitivity(float sensitivity) { mouseSensitivity_ = sensitivity; }
	void SetMouseInvert(bool invertYaw, bool invertPitch) {
		invertYaw_ = invertYaw;
		invertPitch_ = invertPitch;
	}
	void SetPitchLimit(float minPitch, float maxPitch);
	void SetOcclusionMargin(float margin) { occlusionMargin_ = margin; }
	void SetOcclusionSmoothTimes(float pullInSeconds, float recoverySeconds) {
		occlusionPullInSmoothTime_ = pullInSeconds;
		occlusionRecoverySmoothTime_ = recoverySeconds;
	}
	void SetPositionSmoothTime(float seconds) { positionSmoothTime_ = seconds; }
	void SetRotationSmoothTime(float seconds) { rotationSmoothTime_ = seconds; }
	void SetFollowTargetYaw(bool enabled) {
		if (followTargetYaw_ == enabled) {
			return;
		}
		followTargetYaw_ = enabled;
		targetYawInitialized_ = false;
	}
	void SetOcclusionEnabled(bool enabled) { occlusionEnabled_ = enabled; }
	void SetAimModeEnabled(bool enabled) {
		aimModeEnabled_ = enabled;
		if (!enabled) {
			isAimMode_ = false;
		}
	}

private:
	bool RayIntersectOBB(
		const Vector3& origin,
		const Vector3& direction,
		float maxDistance,
		const OBBCollider& collider,
		float& outDistance
	) const;
	Vector3 ClosestPointOnOBB(
		const Vector3& point,
		const OBBCollider& collider
	) const;
	float ResolveOcclusionDistance(
		const Vector3& focus,
		const Vector3& desiredCameraPosition,
		const std::vector<OBBCollider*>& obstacleColliders
	) const;

	Camera* camera_ = nullptr;
	float yaw_ = 0.0f;
	float pitch_ = 0.35f;
	float desiredYaw_ = 0.0f;
	float desiredPitch_ = 0.35f;
	float distance_ = 8.0f;
	float targetDistance_ = 8.0f;
	float normalDistance_ = 8.0f;
	float aimDistance_ = 3.0f;
	float minDistance_ = 1.4f;
	float maxDistance_ = 30.0f;
	float minPitch_ = -1.45f;
	float maxPitch_ = 1.35f;
	float mouseSensitivity_ = 0.005f;
	float zoomStep_ = 1.0f;
	float positionSmoothTime_ = 0.12f;
	float rotationSmoothTime_ = 0.08f;
	float occlusionMargin_ = 0.45f;
	float occlusionPullInSmoothTime_ = 0.04f;
	float occlusionRecoverySmoothTime_ = 0.18f;
	float occlusionDistance_ = 8.0f;
	float cameraBodyRadius_ = 0.12f;
	bool isAimMode_ = false;
	bool focusInitialized_ = false;
	bool followTargetYaw_ = false;
	bool targetYawInitialized_ = false;
	bool occlusionDistanceInitialized_ = false;
	bool occlusionActive_ = false;
	bool occlusionEnabled_ = true;
	bool aimModeEnabled_ = true;
	bool invertYaw_ = false;
	bool invertPitch_ = false;
	float previousTargetYaw_ = 0.0f;
	Vector3 currentFocus_ = {};
	Vector3 targetOffset_ = { 0.0f, 1.35f, 0.0f };
	Vector3 aimTargetOffset_ = { 0.0f, 1.55f, 0.0f };
};
