// 役割: 衝突形状に共通する種別、ワールド行列、ローカルオフセットを定義する。
#pragma once

#include <cstdint>
#include <array>

#include "../math/Matrix4x4.h"
#include "../math/Vector3.h"

class SphereCollider;
class OBBCollider;

class Collider {
public:
	enum class Type {
		Sphere,
		OBB
	};

	virtual ~Collider() = default;

	virtual Type GetType() const = 0;
	virtual bool Intersects(const Collider& other) const = 0;

	void SetWorldMatrix(const Matrix4x4* worldMatrix) { worldMatrix_ = worldMatrix; }
	const Matrix4x4* GetWorldMatrix() const { return worldMatrix_; }

	void SetOffset(const Vector3& offset) { offset_ = offset; }
	const Vector3& GetOffset() const { return offset_; }

	void SetActive(bool active) { isActive_ = active; }
	bool IsActive() const { return isActive_; }
	void SetTrigger(bool trigger) { isTrigger_ = trigger; }
	bool IsTrigger() const { return isTrigger_; }

	void SetCollisionAttribute(uint32_t attribute) { collisionAttribute_ = attribute; }
	uint32_t GetCollisionAttribute() const { return collisionAttribute_; }

	void SetCollisionMask(uint32_t mask) { collisionMask_ = mask; }
	uint32_t GetCollisionMask() const { return collisionMask_; }

	bool CanCollideWith(const Collider& other) const;
	Vector3 GetWorldCenter() const;

protected:
	float GetWorldAxisScale(uint32_t axis) const;
	float GetMaxWorldScale() const;

	const Matrix4x4* worldMatrix_ = nullptr;
	Vector3 offset_ = { 0.0f, 0.0f, 0.0f };
	bool isActive_ = true;
	bool isTrigger_ = false;
	uint32_t collisionAttribute_ = 0xffffffffu;
	uint32_t collisionMask_ = 0xffffffffu;
};

bool CheckCollision(const SphereCollider& a, const SphereCollider& b);
bool CheckCollision(const SphereCollider& sphere, const OBBCollider& obb);
bool CheckCollision(const OBBCollider& a, const OBBCollider& b);

// World空間の形状コピー。行列/Colliderへの参照を持たず、一区間内の回転・寸法は固定する。
struct ColliderSweepShape {
	Collider::Type type = Collider::Type::Sphere;
	Vector3 center{};
	float radius = 0.0f;
	std::array<Vector3, 3> axis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
	Vector3 halfSize{};
	uint32_t attribute = 0, mask = 0;
};
struct ColliderSweepHit {
	float fraction = 0.0f;
	Vector3 normal{}; // BからAへ向く。初期重なりでは最小分離方向。
	Vector3 position{}; // 接触代表位置。接触面全体のmanifoldではない。
	bool initialOverlap = false;
};
enum class ColliderSweepResult { Miss, Hit, Invalid };
// 不正な形状ではfalse、out不変。active/triggerの採否は呼出側が決める。
bool CaptureColliderSweepShape(const Collider& collider, ColliderSweepShape& out);
// 両者のworld並進を同じ0..1時間で比較。Miss/Invalid時はout不変。
ColliderSweepResult SweepColliderTranslation(const ColliderSweepShape& a, const Vector3& deltaA,
	const ColliderSweepShape& b, const Vector3& deltaB, ColliderSweepHit& out);
