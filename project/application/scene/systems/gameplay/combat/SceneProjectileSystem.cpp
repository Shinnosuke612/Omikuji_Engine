// 役割: Prefab生成された弾をSceneDocumentの保存対象外Runtime Entityとして更新する。
#include "SceneProjectileSystem.h"

#include "../../../../../engine/3d/Object3d.h"
#include "../../../../../engine/math/Math.h"
#include "../../../../../engine/math/Matrix4x4.h"
#include "../../../../../engine/math/Quaternion.h"
#include "../../../../../engine/scene/SceneDocument.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
	Vector3 RotateDirection(const Vector3& direction, const Quaternion& rotation) {
		const Matrix4x4 matrix = MakeRotateMatrix(rotation);
		return {
			direction.x * matrix.m[0][0] +
				direction.y * matrix.m[1][0] +
				direction.z * matrix.m[2][0],
			direction.x * matrix.m[0][1] +
				direction.y * matrix.m[1][1] +
				direction.z * matrix.m[2][1],
			direction.x * matrix.m[0][2] +
				direction.y * matrix.m[1][2] +
				direction.z * matrix.m[2][2]
		};
	}

	SceneEntity* ResolveHomingTarget(
		SceneDocument& document,
		const SceneComponent& projectile
	) {
		if (projectile.projectileHomingTargetEntityId != 0) {
			if (SceneEntity* entity = document.FindEntity(
				projectile.projectileHomingTargetEntityId)) {
				return entity;
			}
		}
		return projectile.projectileHomingTargetEntityName.empty()
			? nullptr
			: document.FindEntityByName(
				projectile.projectileHomingTargetEntityName
			);
	}
}

void SceneProjectileSystem::Update(
	SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	float deltaTime
) {
	std::unordered_set<uint64_t> requiredEntities;
	std::vector<uint64_t> expiredEntities;
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (!binding.entity || !binding.object) {
			continue;
		}
		const SceneComponent* projectile =
			SceneEntityQuery::FindEnabledComponent(*binding.entity, "Projectile");
		if (!projectile) {
			continue;
		}
		requiredEntities.insert(binding.entity->id);
		ProjectileRuntime& runtime = runtimes_[binding.entity->id];
		Transform& transform = binding.object->GetTransform();
		if (!runtime.initialized) {
			const Quaternion rotation = transform.useQuaternionRotation
				? transform.quaternionRotate
				: MakeQuaternionFromEuler(transform.rotate);
			Vector3 direction = RotateDirection(
				projectile->projectileDirection,
				rotation
			);
			if (Math::Length(direction) <= 0.0001f) {
				direction = { 0.0f, 0.0f, 1.0f };
			} else {
				direction = Math::Normalize(direction);
			}
			runtime.velocity = Math::Multiply(
				direction,
				projectile->projectileSpeed
			);
			runtime.initialized = true;
		}

		runtime.age += (std::max)(deltaTime, 0.0f);
		if (runtime.age >= (std::max)(projectile->projectileLifetime, 0.0f)) {
			binding.entity->active = false;
			expiredEntities.push_back(binding.entity->id);
			continue;
		}
		if (projectile->projectileHomingStrength > 0.0f) {
			if (SceneEntity* target = ResolveHomingTarget(document, *projectile)) {
				const Transform targetTransform =
					SceneTransformResolver::ResolveScene3DTransform(
						document,
						*target
					);
				const Vector3 toTarget = Math::Subtract(
					targetTransform.translate,
					transform.translate
				);
				if (Math::Length(toTarget) > 0.0001f) {
					const float speed = Math::Length(runtime.velocity);
					if (speed > 0.0001f) {
						const float amount = std::clamp(
							1.0f - std::exp(
								-projectile->projectileHomingStrength * deltaTime
							),
							0.0f,
							1.0f
						);
						Vector3 direction = Math::Add(
							Math::Multiply(
								Math::Normalize(runtime.velocity),
								1.0f - amount
							),
							Math::Multiply(Math::Normalize(toTarget), amount)
						);
						if (Math::Length(direction) > 0.0001f) {
							runtime.velocity = Math::Multiply(
								Math::Normalize(direction),
								speed
							);
						}
					}
				}
			}
		}
		runtime.velocity.y -= projectile->projectileGravity * deltaTime;
		transform.translate = Math::Add(
			transform.translate,
			Math::Multiply(runtime.velocity, deltaTime)
		);
		binding.object->Update();
		binding.entity->transform.translate = transform.translate;
		binding.entity->transform.scale = transform.scale;
		binding.entity->transform.rotate = transform.useQuaternionRotation
			? transform.quaternionRotate
			: MakeQuaternionFromEuler(transform.rotate);
	}

	for (uint64_t entityId : expiredEntities) {
		pendingRemovals_.insert(entityId);
	}
	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredEntities.contains(iterator->first)) {
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void SceneProjectileSystem::FlushRemovals(SceneDocument& document) {
	for (uint64_t entityId : pendingRemovals_) {
		const SceneEntity* entity = document.FindEntity(entityId);
		const bool runtimeOnly = entity && entity->runtimeOnly;
		const bool wasDirty = document.IsDirty();
		document.RemoveEntity(entityId);
		if (runtimeOnly && !wasDirty) {
			document.MarkClean();
		}
		runtimes_.erase(entityId);
	}
	pendingRemovals_.clear();
}

void SceneProjectileSystem::Clear() {
	runtimes_.clear();
	pendingRemovals_.clear();
}
