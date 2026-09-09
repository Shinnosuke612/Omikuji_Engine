// 役割: HierarchyからEntityの2Dおよび3DワールドTransformを解決する。
#pragma once

#include "../math/Matrix4x4.h"
#include "../math/Transform.h"

class SceneDocument;
struct SceneEntity;

namespace SceneTransformResolver {
	Transform ResolveScene2DTransform(
		const SceneDocument& document,
		const SceneEntity& entity
	);

	Matrix4x4 ResolveSceneWorldMatrix(
		const SceneDocument& document,
		const SceneEntity& entity
	);

	Transform ResolveScene3DTransform(
		const SceneDocument& document,
		const SceneEntity& entity
	);

	bool TryConvertSceneWorldTransformToLocal(
		const SceneDocument& document,
		const SceneEntity& entity,
		const Transform& worldTransform,
		Transform& localTransform
	);
}
