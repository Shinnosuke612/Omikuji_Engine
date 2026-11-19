// 役割: PhysicsBodyとColliderを使った重力、移動、衝突解決を管理する。
#pragma once

#include <vector>

#include "PhysicsBody.h"
#include "../math/Vector3.h"

class Collider;

class PhysicsWorld {
public:
	void Clear();
	void AddBody(PhysicsBody* body);
	void AddStaticCollider(Collider* collider);
	// Physicsとcontrolled攻撃時計が共有する秒数。非有限値は進めない。
	static float EffectiveStepDelta(float deltaTime);
	void Step(float deltaTime);
	// 確定前の姿勢と補正区間を、既存の静的障害物と同じ登録集合で照会する。
	bool IsPlacementBlocked(const PhysicsBody& body) const;
	bool IsPlanarCorrectionBlocked(const PhysicsBody& body, const Vector3& delta) const;

	void SetGravity(const Vector3& gravity) { gravity_ = gravity; }
	const Vector3& GetGravity() const { return gravity_; }

private:
	bool CollidesWithStatic(const PhysicsBody& body) const;
	bool SnapToGround(PhysicsBody& body, float probeDistance) const;
	bool ResolveStaticPenetration(PhysicsBody& body) const;
	bool IntegrateAxis(
		PhysicsBody& body,
		float delta,
		uint32_t axis
	) const;

	Vector3 gravity_ = { 0.0f, -9.8f, 0.0f };
	std::vector<PhysicsBody*> bodies_;
	std::vector<Collider*> staticColliders_;
};
