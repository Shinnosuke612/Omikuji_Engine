// 役割: プレイヤーの移動状態、物理ボディ、表示オブジェクトと攻撃中の固定回転を所有する。
#pragma once
#include <cstdint>

#include "../../engine/collision/Collider.h"
#include "../../engine/physics/PhysicsBody.h"
#include "../../engine/math/Vector3.h"
#include "../../engine/math/Transform.h"

class Object3d;
class Object3dCommon;
class Camera;
struct Matrix4x4;

class Player {
public:
	void Initialize(Object3dCommon* object3dCommon, const char* modelName);
	void Initialize(Object3d* object);
	void Update(
		const Camera* camera,
		bool acceptGameplayInput
	);
	void PostPhysicsUpdate();
	void Draw();
	void DrawShadow(const Matrix4x4& lightViewProjection);
	void Finalize();

	const Vector3& GetPosition() const { return position_; }
	Collider* GetCollider() const { return collider_; }
	PhysicsBody& GetPhysicsBody() { return physicsBody_; }
	// 現在の環境の歩行基準速度。水中補正を含み、ダッシュ/入力/状態を変更しない。
	float GetWalkSpeed() const { return moveSpeed_ * (inWater_ ? waterMoveSpeedMultiplier_ : 1.0f); }
	Object3d* GetObject() const { return object_; }
	void SetCollider(Collider* collider);
	void SetTransform(const Transform& transform);
	// Runtime専用。回転だけを保持/解放し、再反映した場合true。callerがEntity/子孫を同期する。
	// 世代/owner変更時には旧姿勢を破棄。位置・速度・接地・保存データは変更しない。
	bool SetAttackBodyFacingLock(bool active, uint64_t documentGeneration, uint64_t ownerEntityId);
	// 配置変更/無効化では旧回転を書き戻さず破棄する。次Updateで補間状態を再初期化。
	void ClearAttackBodyFacingLock();
	bool IsAttackBodyFacingLocked() const { return attackBodyFacingLocked_; }
	void SetBehaviorSettings(
		float moveSpeed,
		float jumpVelocity,
		float turnResponsiveness,
		float dashMultiplier,
		bool cameraRelativeMove,
		bool allowJump
	);
	void SetWaterState(
		bool inWater,
		float moveSpeedMultiplier,
		float swimUpSpeed
	);

private:
	void ApplyPosition();
	void SyncRotationStateFromObject();

private:
	Object3d* object_ = nullptr;
	bool ownsObject_ = false;
	Collider* collider_ = nullptr;
	PhysicsBody physicsBody_;
	Vector3 position_ = { 0.0f, 1.0f, -4.0f };
	float moveSpeed_ = 10.8f;
	float turnResponsiveness_ = 0.018f;
	float jumpVelocity_ = 37.2f;
	float dashMultiplier_ = 1.65f;
	float currentYaw_ = 0.0f;
	float currentPitch_ = 0.0f;
	Quaternion currentRotation_ = { 0.0f, 0.0f, 0.0f, 1.0f };
	bool rotationInitialized_ = false;
	// Runtime Scene/ownerに限定した値snapshot。Object/Entityの借用pointerは保持しない。
	bool attackBodyFacingLocked_ = false, attackBodyUsesQuaternion_ = false;
	uint64_t attackBodyGeneration_ = 0, attackBodyOwner_ = 0;
	Vector3 attackBodyEuler_{};
	Quaternion attackBodyQuaternion_ = { 0.0f, 0.0f, 0.0f, 1.0f };
	bool cameraRelativeMove_ = true;
	bool allowJump_ = true;
	bool inWater_ = false;
	float waterMoveSpeedMultiplier_ = 0.45f;
	float waterSwimUpSpeed_ = 12.0f;
};
