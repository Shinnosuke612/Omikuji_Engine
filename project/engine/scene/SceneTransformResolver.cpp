// 役割: 親子関係をたどり、循環を防ぎながらワールドTransformを計算する。
#include "SceneTransformResolver.h"

#include "SceneDocument.h"
#include "../math/Math.h"

#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace {
	Transform ResolveScene2DTransformRecursive(
		const SceneDocument& document,
		const SceneEntity& entity,
		std::unordered_set<uint64_t>& visited
	) {
		Transform result{};
		result.scale = entity.transform.scale;
		result.rotate = MakeEulerFromQuaternion(entity.transform.rotate);
		result.translate = entity.transform.translate;
		if (entity.parentId == 0 || !visited.insert(entity.id).second) {
			return result;
		}

		const SceneEntity* parent = document.FindEntity(entity.parentId);
		if (!parent) {
			return result;
		}

		const Transform parentTransform = ResolveScene2DTransformRecursive(
			document,
			*parent,
			visited
		);
		const float scaledX = result.translate.x * parentTransform.scale.x;
		const float scaledY = result.translate.y * parentTransform.scale.y;
		const float cosine = std::cos(parentTransform.rotate.z);
		const float sine = std::sin(parentTransform.rotate.z);
		result.translate.x =
			parentTransform.translate.x + scaledX * cosine - scaledY * sine;
		result.translate.y =
			parentTransform.translate.y + scaledX * sine + scaledY * cosine;
		result.rotate.z += parentTransform.rotate.z;
		result.scale.x *= parentTransform.scale.x;
		result.scale.y *= parentTransform.scale.y;
		return result;
	}

	Matrix4x4 ResolveSceneWorldMatrixRecursive(
		const SceneDocument& document,
		const SceneEntity& entity,
		std::unordered_set<uint64_t>& visited
	) {
		const Matrix4x4 local = MakeAffineMatrix(
			entity.transform.scale,
			entity.transform.rotate,
			entity.transform.translate
		);
		if (entity.parentId == 0 || !visited.insert(entity.id).second) {
			return local;
		}

		const SceneEntity* parent = document.FindEntity(entity.parentId);
		if (!parent) {
			return local;
		}
		return Multiply(
			local,
			ResolveSceneWorldMatrixRecursive(document, *parent, visited)
		);
	}
}

namespace SceneTransformResolver {
	Transform ResolveScene2DTransform(
		const SceneDocument& document,
		const SceneEntity& entity
	) {
		std::unordered_set<uint64_t> visited;
		return ResolveScene2DTransformRecursive(document, entity, visited);
	}

	Matrix4x4 ResolveSceneWorldMatrix(
		const SceneDocument& document,
		const SceneEntity& entity
	) {
		std::unordered_set<uint64_t> visited;
		return ResolveSceneWorldMatrixRecursive(document, entity, visited);
	}

	Transform ResolveScene3DTransform(
		const SceneDocument& document,
		const SceneEntity& entity
	) {
		const Matrix4x4 world = ResolveSceneWorldMatrix(document, entity);
		Transform result{};
		result.scale = entity.transform.scale;
		result.rotate = MakeEulerFromQuaternion(entity.transform.rotate);
		result.translate = entity.transform.translate;
		result.useQuaternionRotation = true;
		result.quaternionRotate = entity.transform.rotate;
		Vector3 scale{};
		Quaternion rotate = MakeIdentityQuaternion();
		Vector3 translate{};
		if (DecomposeAffineMatrix(world, scale, rotate, translate)) {
			result.scale = scale;
			result.rotate = MakeEulerFromQuaternion(rotate);
			result.translate = translate;
			result.quaternionRotate = rotate;
		}
		return result;
	}

	bool TryConvertSceneWorldTransformToLocal(
		const SceneDocument& document,
		const SceneEntity& entity,
		const Transform& worldTransform,
		Transform& localTransform
	) {
		const Quaternion worldRotation = worldTransform.useQuaternionRotation
			? worldTransform.quaternionRotate
			: MakeQuaternionFromEuler(worldTransform.rotate);
		const Matrix4x4 worldMatrix = MakeAffineMatrix(
			worldTransform.scale,
			worldRotation,
			worldTransform.translate
		);
		Matrix4x4 localMatrix = worldMatrix;
		if (entity.parentId != 0) {
			const SceneEntity* parent = document.FindEntity(entity.parentId);
			if (!parent) {
				return false;
			}
			const Matrix4x4 parentWorld = ResolveSceneWorldMatrix(document, *parent);
			if (std::abs(Determinant(parentWorld)) < 0.000001f) {
				return false;
			}
			localMatrix = Multiply(worldMatrix, Inverse(parentWorld));
		}
		Vector3 localScale{};
		Quaternion localRotation = MakeIdentityQuaternion();
		Vector3 localTranslate{};
		if (!DecomposeAffineMatrix(
			localMatrix,
			localScale,
			localRotation,
			localTranslate
		)) {
			return false;
		}
		localTransform.scale = localScale;
		localTransform.rotate = MakeEulerFromQuaternion(localRotation);
		localTransform.translate = localTranslate;
		localTransform.useQuaternionRotation = true;
		localTransform.quaternionRotate = localRotation;
		return true;
	}
}
