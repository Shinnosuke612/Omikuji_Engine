// 役割: Joint名を解決し、武器などのローカルTransformへ親行列を適用する。
#include "SceneAttachmentSystem.h"

#include "../rendering/SceneObjectSystem.h"
#include "../../../../engine/3d/Object3d.h"
#include "../../../../engine/math/Math.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"

namespace {
	SceneEntity* ResolveTarget(
		SceneDocument& document,
		const SceneEntity& attachedEntity,
		const SceneComponent& attachment
	) {
		if (attachment.boneAttachmentTargetEntityId != 0) {
			if (SceneEntity* entity = document.FindEntity(
				attachment.boneAttachmentTargetEntityId)) {
				return entity;
			}
		}
		if (!attachment.boneAttachmentTargetEntityName.empty()) {
			if (SceneEntity* entity = document.FindEntityByName(
				attachment.boneAttachmentTargetEntityName)) {
				return entity;
			}
		}
		return document.FindEntity(attachedEntity.parentId);
	}

	Matrix4x4 RemoveScale(const Matrix4x4& source) {
		Matrix4x4 result = source;
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const Vector3 sourceAxis{
				source.m[axis][0],
				source.m[axis][1],
				source.m[axis][2]
			};
			const Vector3 normalized = Math::Length(sourceAxis) > 0.0001f
				? Math::Normalize(sourceAxis)
				: Vector3{
					axis == 0 ? 1.0f : 0.0f,
					axis == 1 ? 1.0f : 0.0f,
					axis == 2 ? 1.0f : 0.0f
				};
			result.m[axis][0] = normalized.x;
			result.m[axis][1] = normalized.y;
			result.m[axis][2] = normalized.z;
		}
		return result;
	}

	Matrix4x4 MakeEntityLocalMatrix(const SceneEntity& entity) {
		return MakeAffineMatrix(
			entity.transform.scale,
			entity.transform.rotate,
			entity.transform.translate
		);
	}

	bool TryGetSourceJointMatrix(
		const Object3d& object,
		const std::string& jointName,
		Matrix4x4& skeletonSpaceMatrix
	) {
		const Skeleton* skeleton = object.GetSkeleton();
		if (!skeleton) {
			return false;
		}
		const auto found = skeleton->jointMap.find(jointName);
		if (
			found == skeleton->jointMap.end() ||
			found->second < 0 ||
			found->second >= static_cast<int32_t>(skeleton->joints.size())
		) {
			return false;
		}
		skeletonSpaceMatrix = skeleton->joints[found->second].skeletonSpaceMatrix;
		return true;
	}
}

void SceneAttachmentSystem::Update(
	SceneDocument& document,
	SceneObjectSystem& objectSystem,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) {
	for (uint64_t entityId : attachedEntityIds_) {
		if (Object3d* object = objectSystem.FindObject(entityId)) {
			object->ClearParentMatrixOverride();
			object->Update();
		}
	}
	attachedEntityIds_.clear();

	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.object ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, *binding.entity)
		) {
			continue;
		}
		const SceneComponent* attachment =
			SceneEntityQuery::FindEnabledComponent(*binding.entity, "BoneAttachment");
		if (!attachment || attachment->boneAttachmentJointName.empty()) {
			continue;
		}
		SceneEntity* target = ResolveTarget(document, *binding.entity, *attachment);
		Object3d* targetObject = target
			? objectSystem.FindObject(target->id)
			: nullptr;
		Matrix4x4 jointWorld{};
		if (
			!targetObject ||
			!targetObject->TryGetJointWorldMatrix(
				attachment->boneAttachmentJointName,
				jointWorld
			)
		) {
			continue;
		}
		Matrix4x4 attachmentParentMatrix =
			attachment->boneAttachmentInheritScale
				? jointWorld
				: RemoveScale(jointWorld);
		if (
			attachment->boneAttachmentAlignmentMode == "MatchSourceBone" &&
			!attachment->boneAttachmentSourceJointName.empty()
		) {
			Matrix4x4 sourceJointMatrix{};
			if (TryGetSourceJointMatrix(
				*binding.object,
				attachment->boneAttachmentSourceJointName,
				sourceJointMatrix
			)) {
				// sourceJoint * entityLocal * parent = targetJoint を満たす。
				attachmentParentMatrix = Multiply(
					Multiply(
						Inverse(MakeEntityLocalMatrix(*binding.entity)),
						Inverse(sourceJointMatrix)
					),
					attachmentParentMatrix
				);
			}
		}
		binding.object->SetParentMatrixOverride(attachmentParentMatrix);
		binding.object->Update();
		attachedEntityIds_.insert(binding.entity->id);
	}
}

void SceneAttachmentSystem::Clear(SceneObjectSystem* objectSystem) {
	if (objectSystem) {
		for (uint64_t entityId : attachedEntityIds_) {
			if (Object3d* object = objectSystem->FindObject(entityId)) {
				object->ClearParentMatrixOverride();
			}
		}
	}
	attachedEntityIds_.clear();
}
