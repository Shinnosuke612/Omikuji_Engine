// 役割: プレイヤーの入力移動、物理状態の同期、攻撃中の体向き保持と解放を実装する。
#include "Player.h"

#include "../../engine/3d/Object3d.h"
#include "../../engine/3d/Object3dCommon.h"
#include "../../engine/3d/Camera.h"
#include "../../engine/io/Input.h"
#include "../../engine/math/Math.h"
#include "../../engine/math/Matrix4x4.h"
#include "../../engine/math/Quaternion.h"

#include <algorithm>
#include <cmath>

namespace {
	constexpr float kMaxWaterPitch = 1.15f;

	Vector3 FlattenAndNormalize(Vector3 value) {
		value.y = 0.0f;
		if (Math::Length(value) < 0.000001f) {
			return { 0.0f, 0.0f, 0.0f };
		}
		return Math::Normalize(value);
	}

	float RotationFollowAmount(float responsiveness) {
		return Math::EaseOutCubic(std::clamp(responsiveness, 0.0f, 1.0f));
	}
}

void Player::Initialize(Object3dCommon* object3dCommon, const char* modelName) {
	ClearAttackBodyFacingLock();
	object_ = new Object3d();
	ownsObject_ = true;
	object_->Initialize(object3dCommon);
	object_->SetModel(modelName);
	object_->SetScale({ 1.0f, 1.0f, 1.0f });
	ApplyPosition();
	object_->Update();

	physicsBody_.type = PhysicsBodyType::Dynamic;
	physicsBody_.transform = &object_->GetTransform();
	physicsBody_.collider = collider_;
	physicsBody_.useGravity = true;
	physicsBody_.gravityScale = 8.0f;
	physicsBody_.maxFallSpeed = 60.0f;
	physicsBody_.friction = 0.0f;
}

void Player::Initialize(Object3d* object) {
	ClearAttackBodyFacingLock();
	object_ = object;
	ownsObject_ = false;
	if (!object_) {
		return;
	}
	position_ = object_->GetTransform().translate;
	physicsBody_.type = PhysicsBodyType::Dynamic;
	physicsBody_.transform = &object_->GetTransform();
	physicsBody_.collider = collider_;
	physicsBody_.useGravity = true;
	physicsBody_.gravityScale = 8.0f;
	physicsBody_.maxFallSpeed = 60.0f;
	physicsBody_.friction = 0.0f;
}

void Player::SetCollider(Collider* collider) {
	collider_ = collider;
	physicsBody_.collider = collider_;
}

void Player::Update(
	const Camera* camera,
	bool acceptGameplayInput
) {
	if (!object_) {
		return;
	}
	if (!rotationInitialized_) {
		SyncRotationStateFromObject();
	}
	Vector3 inputMove{};
	Input* input = acceptGameplayInput ? Input::GetInstance() : nullptr;

	if (input && input->PushKey(DIK_W)) {
		inputMove.z += 1.0f;
	}
	if (input && input->PushKey(DIK_S)) {
		inputMove.z -= 1.0f;
	}
	if (input && input->PushKey(DIK_A)) {
		inputMove.x -= 1.0f;
	}
	if (input && input->PushKey(DIK_D)) {
		inputMove.x += 1.0f;
	}

	Vector3 desiredVelocity = physicsBody_.velocity;
	desiredVelocity.x = 0.0f;
	desiredVelocity.z = 0.0f;

	Vector3 cameraForward = { 0.0f, 0.0f, 1.0f };
	Vector3 cameraRight = { 1.0f, 0.0f, 0.0f };
	if (acceptGameplayInput && cameraRelativeMove_ && camera) {
		const Matrix4x4& cameraWorld = camera->GetWorldMatrix();
		cameraRight = {
			cameraWorld.m[0][0],
			cameraWorld.m[0][1],
			cameraWorld.m[0][2]
		};
		cameraForward = {
			cameraWorld.m[2][0],
			cameraWorld.m[2][1],
			cameraWorld.m[2][2]
		};
	}

	Vector3 movementRight = cameraRight;
	Vector3 movementForward = cameraForward;
	if (!inWater_) {
		movementRight = FlattenAndNormalize(movementRight);
		movementForward = FlattenAndNormalize(movementForward);
	} else {
		movementRight.y = 0.0f;
		movementRight = Math::Length(movementRight) > 0.000001f
			? Math::Normalize(movementRight)
			: Vector3{ 1.0f, 0.0f, 0.0f };
		movementForward = Math::Length(movementForward) > 0.000001f
			? Math::Normalize(movementForward)
			: Vector3{ 0.0f, 0.0f, 1.0f };
	}
	if (Math::Length(movementRight) < 0.000001f) {
		movementRight = { 1.0f, 0.0f, 0.0f };
	}
	if (Math::Length(movementForward) < 0.000001f) {
		movementForward = { 0.0f, 0.0f, 1.0f };
	}

	const bool dash =
		input && (input->PushKey(DIK_LSHIFT) || input->PushKey(DIK_RSHIFT));
	const float speedMultiplier = dash ? dashMultiplier_ : 1.0f;

	if (Math::Length(inputMove) > 0.000001f) {
		Vector3 move = Math::Add(
			Math::Multiply(movementRight, inputMove.x),
			Math::Multiply(movementForward, inputMove.z)
		);
		const float activeMoveSpeed =
			moveSpeed_ *
			speedMultiplier *
			(inWater_ ? waterMoveSpeedMultiplier_ : 1.0f);
		move = Math::Multiply(Math::Normalize(move), activeMoveSpeed);
		desiredVelocity.x = move.x;
		desiredVelocity.z = move.z;
		if (inWater_) {
			desiredVelocity.y = move.y;
		}
	}

	if (acceptGameplayInput && cameraRelativeMove_ && camera && !IsAttackBodyFacingLocked()) {
		Vector3 facing = cameraForward;
		if (!inWater_) {
			facing = FlattenAndNormalize(facing);
		} else if (Math::Length(facing) > 0.000001f) {
			facing = Math::Normalize(facing);
		}
		if (Math::Length(facing) > 0.000001f) {
			const float horizontalLength = std::sqrt(
				facing.x * facing.x + facing.z * facing.z
			);
			const float targetYaw = horizontalLength > 0.000001f
				? std::atan2(facing.x, facing.z)
				: currentYaw_;
			const float targetPitch = inWater_
				? std::clamp(
					-std::atan2(
						facing.y,
						(std::max)(horizontalLength, 0.000001f)
					),
					-kMaxWaterPitch,
					kMaxWaterPitch
				)
				: 0.0f;
			const float yawT = RotationFollowAmount(turnResponsiveness_);
			const float pitchT = inWater_
				? yawT
				: Math::SmoothStep(yawT);
			const Quaternion targetRotation =
				MakeLookRotationQuaternion(facing, { 0.0f, 1.0f, 0.0f });
			currentYaw_ = Math::LerpAngle(currentYaw_, targetYaw, yawT);
			currentPitch_ = Math::Lerp(currentPitch_, targetPitch, pitchT);
			currentRotation_ = Slerp(currentRotation_, targetRotation, yawT);
			object_->SetRotate({ currentPitch_, currentYaw_, 0.0f });
			object_->SetRotateQuaternion(currentRotation_);
		}
	}

	if (acceptGameplayInput && inWater_) {
		const bool swimUp = input && input->PushKey(DIK_SPACE);
		const bool swimDown = input && input->PushKey(DIK_LCONTROL);
		if (swimUp) {
			desiredVelocity.y = waterSwimUpSpeed_ * speedMultiplier;
			physicsBody_.isGrounded = false;
		} else if (swimDown) {
			desiredVelocity.y = -waterSwimUpSpeed_ * 10.6f * speedMultiplier;
			physicsBody_.isGrounded = false;
		} else {
			desiredVelocity.y = std::clamp(
				desiredVelocity.y,
				-waterSwimUpSpeed_ * 0.35f * speedMultiplier,
				waterSwimUpSpeed_ * 0.35f * speedMultiplier
			);
		}
	} else if (input && allowJump_ && physicsBody_.isGrounded && input->TriggerKey(DIK_SPACE)) {
		desiredVelocity.y = jumpVelocity_;
		physicsBody_.isGrounded = false;
	}

	physicsBody_.velocity = desiredVelocity;
	object_->Update();
}

void Player::PostPhysicsUpdate() {
	if (!object_) {
		return;
	}
	position_ = object_->GetTransform().translate;
	object_->Update();
}

void Player::Draw() {
	if (object_) {
		object_->Draw();
	}
}

void Player::DrawShadow(const Matrix4x4& lightViewProjection) {
	if (object_) {
		object_->DrawShadow(lightViewProjection);
	}
}

void Player::Finalize() {
	ClearAttackBodyFacingLock();
	if (ownsObject_) {
		delete object_;
	}
	object_ = nullptr;
	ownsObject_ = false;
}

void Player::SetTransform(const Transform& transform) {
	ClearAttackBodyFacingLock();
	if (!object_) {
		return;
	}
	position_ = transform.translate;
	physicsBody_.velocity = {};
	physicsBody_.isGrounded = false;
	object_->GetTransform() = transform;
	SyncRotationStateFromObject();
	ApplyPosition();
	object_->Update();
}

bool Player::SetAttackBodyFacingLock(bool active, uint64_t documentGeneration, uint64_t ownerEntityId) {
	if (!object_ || !documentGeneration || !ownerEntityId) {
		ClearAttackBodyFacingLock();
		return false;
	}
	if (attackBodyGeneration_ != documentGeneration || attackBodyOwner_ != ownerEntityId) {
		// Scene交換/再利用先へ前のownerの姿勢を復元しない。
		ClearAttackBodyFacingLock();
		attackBodyGeneration_ = documentGeneration;
		attackBodyOwner_ = ownerEntityId;
	}
	if (!active && !attackBodyFacingLocked_) return false;
	if (active && !attackBodyFacingLocked_) {
		attackBodyEuler_ = object_->GetRotate();
		attackBodyQuaternion_ = object_->GetRotateQuaternion();
		attackBodyUsesQuaternion_ = object_->UsesQuaternionRotation();
	}
	object_->SetRotate(attackBodyEuler_);
	if (attackBodyUsesQuaternion_) object_->SetRotateQuaternion(attackBodyQuaternion_);
	attackBodyFacingLocked_ = active;
	// 解放後にCameraへ跳ばず、最後の実姿勢から既存の追従補間を再開する。
	SyncRotationStateFromObject();
	return true;
}

void Player::ClearAttackBodyFacingLock() {
	attackBodyFacingLocked_ = false;
	attackBodyUsesQuaternion_ = false;
	attackBodyGeneration_ = attackBodyOwner_ = 0;
	attackBodyEuler_ = {};
	attackBodyQuaternion_ = {0.0f, 0.0f, 0.0f, 1.0f};
	rotationInitialized_ = false;
}

void Player::SetBehaviorSettings(
	float moveSpeed,
	float jumpVelocity,
	float turnResponsiveness,
	float dashMultiplier,
	bool cameraRelativeMove,
	bool allowJump
) {
	moveSpeed_ = (std::max)(moveSpeed, 0.0f);
	jumpVelocity_ = (std::max)(jumpVelocity, 0.0f);
	turnResponsiveness_ = std::clamp(turnResponsiveness, 0.0f, 1.0f);
	dashMultiplier_ = (std::max)(dashMultiplier, 1.0f);
	cameraRelativeMove_ = cameraRelativeMove;
	allowJump_ = allowJump;
}

void Player::SetWaterState(
	bool inWater,
	float moveSpeedMultiplier,
	float swimUpSpeed
) {
	inWater_ = inWater;
	waterMoveSpeedMultiplier_ = std::clamp(moveSpeedMultiplier, 0.0f, 1.0f);
	waterSwimUpSpeed_ = (std::max)(swimUpSpeed, 0.0f);
}

void Player::ApplyPosition() {
	if (object_) {
		object_->SetTranslate(position_);
	}
}

void Player::SyncRotationStateFromObject() {
	if (!object_) {
		return;
	}
	currentYaw_ = object_->GetRotate().y;
	currentPitch_ = object_->GetRotate().x;
	currentRotation_ = object_->UsesQuaternionRotation()
		? object_->GetRotateQuaternion()
		: MakeQuaternionFromEuler(object_->GetRotate());
	rotationInitialized_ = true;
}
