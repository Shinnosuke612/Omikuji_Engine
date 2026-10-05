// 役割: Debug UIの状態をScene設定へ保存し、CameraやColliderの補助線を登録する。
#include "SceneDebugSystem.h"

#include "../camera/SceneCameraSystem.h"
#include "../rendering/SceneObjectSystem.h"
#include "../../../../engine/3d/Camera.h"
#include "../../../../engine/3d/Object3d.h"
#include "../../../../engine/debug/DebugRenderer.h"
#include "../../../../engine/math/Math.h"
#include "../../../../engine/math/Matrix4x4.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/SceneTransformResolver.h"
#include "../../../../externals/imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::HasComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveScene3DTransform;

	Vector3 TransformCoord(const Vector3& value, const Matrix4x4& matrix) {
		const float x =
			value.x * matrix.m[0][0] +
			value.y * matrix.m[1][0] +
			value.z * matrix.m[2][0] +
			matrix.m[3][0];
		const float y =
			value.x * matrix.m[0][1] +
			value.y * matrix.m[1][1] +
			value.z * matrix.m[2][1] +
			matrix.m[3][1];
		const float z =
			value.x * matrix.m[0][2] +
			value.y * matrix.m[1][2] +
			value.z * matrix.m[2][2] +
			matrix.m[3][2];
		const float w =
			value.x * matrix.m[0][3] +
			value.y * matrix.m[1][3] +
			value.z * matrix.m[2][3] +
			matrix.m[3][3];
		const float inverseW = std::abs(w) > 0.000001f ? 1.0f / w : 1.0f;
		return { x * inverseW, y * inverseW, z * inverseW };
	}

	void AddCameraDebugDraw(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& cameraComponent,
		float aspectRatio,
		const Transform* overrideTransform = nullptr,
		const Camera* overrideCamera = nullptr
	) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
		if (!IsEntityActiveInHierarchy(document, entity)) {
			return;
		}

		Matrix4x4 cameraWorld{};
		Matrix4x4 cameraViewProjection{};
		Vector3 origin{};
		if (overrideCamera) {
			cameraWorld = overrideCamera->GetWorldMatrix();
			cameraViewProjection = overrideCamera->GetViewProjectionMatrix();
			origin = overrideCamera->GetTranslate();
		} else {
			const Transform cameraTransform = overrideTransform
				? *overrideTransform
				: ResolveScene3DTransform(document, entity);
			Camera debugCamera;
			debugCamera.SetOrbitMode(false);
			debugCamera.SetTranslate(cameraTransform.translate);
			if (cameraTransform.useQuaternionRotation) {
				debugCamera.SetRotateQuaternion(
					cameraTransform.quaternionRotate
				);
			} else {
				debugCamera.SetRotate(cameraTransform.rotate);
			}
			debugCamera.SetFovY(std::clamp(
				cameraComponent.cameraFovY,
				0.0174532925f,
				3.12413936f
			));
			debugCamera.SetAspectRatio((std::max)(aspectRatio, 0.001f));
			debugCamera.SetNearClip((std::max)(
				cameraComponent.cameraNearClip,
				0.001f
			));
			debugCamera.SetFarClip((std::max)(
				(std::min)(cameraComponent.cameraFarClip, 20.0f),
				cameraComponent.cameraNearClip + 0.001f
			));
			debugCamera.Update();
			cameraWorld = debugCamera.GetWorldMatrix();
			cameraViewProjection = debugCamera.GetViewProjectionMatrix();
			origin = debugCamera.GetTranslate();
		}

		const Vector3 right = Math::Normalize({
			cameraWorld.m[0][0],
			cameraWorld.m[0][1],
			cameraWorld.m[0][2]
		});
		const Vector3 up = Math::Normalize({
			cameraWorld.m[1][0],
			cameraWorld.m[1][1],
			cameraWorld.m[1][2]
		});
		const Vector3 forward = Math::Normalize({
			cameraWorld.m[2][0],
			cameraWorld.m[2][1],
			cameraWorld.m[2][2]
		});

		DebugRenderer* renderer = DebugRenderer::GetInstance();
		renderer->AddSphere(
			origin,
			cameraComponent.cameraIsMain ? 0.16f : 0.11f,
			cameraComponent.cameraIsMain
				? Vector4{ 1.0f, 0.85f, 0.15f, 1.0f }
				: Vector4{ 0.15f, 0.85f, 1.0f, 1.0f },
			8
		);
		renderer->AddLine(
			origin,
			Math::Add(origin, Math::Multiply(forward, 1.2f)),
			{ 0.25f, 0.55f, 1.0f, 1.0f }
		);
		renderer->AddLine(
			origin,
			Math::Add(origin, Math::Multiply(up, 0.7f)),
			{ 0.2f, 1.0f, 0.35f, 1.0f }
		);
		renderer->AddLine(
			origin,
			Math::Add(origin, Math::Multiply(right, 0.7f)),
			{ 1.0f, 0.25f, 0.25f, 1.0f }
		);

		const Matrix4x4 inverseViewProjection = Inverse(
			cameraViewProjection
		);
		Vector3 corners[8]{};
		uint32_t index = 0;
		for (float z : { 0.0f, 1.0f }) {
			for (float y : { -1.0f, 1.0f }) {
				for (float x : { -1.0f, 1.0f }) {
					corners[index++] = TransformCoord(
						{ x, y, z },
						inverseViewProjection
					);
				}
			}
		}
		const Vector4 frustumColor = cameraComponent.cameraIsMain
			? Vector4{ 1.0f, 0.8f, 0.1f, 1.0f }
			: Vector4{ 0.2f, 0.85f, 1.0f, 1.0f };
		const uint32_t edges[][2] = {
			{ 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
			{ 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
			{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
		};
		for (const auto& edge : edges) {
			renderer->AddLine(
				corners[edge[0]],
				corners[edge[1]],
				frustumColor
			);
		}
#else
		(void)document;
		(void)entity;
		(void)cameraComponent;
		(void)aspectRatio;
		(void)overrideTransform;
		(void)overrideCamera;
#endif
	}

	void AddThirdPersonCameraDebugDraw(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& thirdPersonCamera,
		const Vector3& cameraPosition
	) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
		if (!IsEntityActiveInHierarchy(document, entity)) {
			return;
		}
		const SceneEntity* target = thirdPersonCamera.thirdPersonTargetEntityId != 0
			? document.FindEntity(thirdPersonCamera.thirdPersonTargetEntityId)
			: nullptr;
		if (
			!thirdPersonCamera.thirdPersonTargetEntityName.empty() &&
			(!target ||
				target->name != thirdPersonCamera.thirdPersonTargetEntityName)
		) {
			target = document.FindEntityByName(
				thirdPersonCamera.thirdPersonTargetEntityName
			);
		}
		if (!target) {
			target = &entity;
		}
		const Transform targetTransform = ResolveScene3DTransform(
			document,
			*target
		);
		const Vector3 focus = Math::Add(
			targetTransform.translate,
			thirdPersonCamera.thirdPersonTargetOffset
		);
		DebugRenderer* renderer = DebugRenderer::GetInstance();
		renderer->AddSphere(
			focus,
			0.12f,
			{ 0.95f, 0.35f, 1.0f, 1.0f },
			8
		);
		renderer->AddLine(
			focus,
			cameraPosition,
			{ 0.95f, 0.35f, 1.0f, 1.0f }
		);
#else
		(void)document;
		(void)entity;
		(void)thirdPersonCamera;
		(void)cameraPosition;
#endif
	}
}

void SceneDebugSystem::LoadSettings(const SceneDocument* document) {
	if (!document) {
		return;
	}
	const SceneDebugSettings& settings = document->GetDebugSettings();
	showCamera_ = settings.showCameraDirection;
	showColliders_ = settings.showColliders;
	showCameraPath_ = settings.showCameraPath;
	showCameraPathPointCamera_ = settings.showCameraPathPointCameraDirection;
	showSkeleton_ = settings.showSkeleton;
	showJointNames_ = settings.showJointNames;
	showJointAxes_ = settings.showJointAxes;
	jointRadius_ = settings.jointRadius;
	jointAxisLength_ = settings.jointAxisLength;
}

void SceneDebugSystem::SaveSettings(SceneDocument& document) const {
	SceneDebugSettings settings = document.GetDebugSettings();
	settings.showCameraDirection = showCamera_;
	settings.showColliders = showColliders_;
	settings.showCameraPath = showCameraPath_;
	settings.showCameraPathPointCameraDirection = showCameraPathPointCamera_;
	settings.showSkeleton = showSkeleton_;
	settings.showJointNames = showJointNames_;
	settings.showJointAxes = showJointAxes_;
	settings.jointRadius = jointRadius_;
	settings.jointAxisLength = jointAxisLength_;
	document.SetDebugSettings(settings);
}

void SceneDebugSystem::DrawEditor(
	SceneDocument* document,
	SceneObjectSystem& objectSystem,
	bool paused
) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	ImGui::Begin(
		"Scene Controls",
		nullptr,
		ImGuiWindowFlags_NoFocusOnAppearing
	);
	if (paused) {
		ImGui::TextDisabled("Paused Debug View");
	}
	if (document) {
		DrawAnimationControls(*document, objectSystem);
	}
	if (!paused) {
		ImGui::SeparatorText("Debug Draw");
	}

	bool settingsChanged = false;
	settingsChanged |= ImGui::Checkbox(
		"Show Camera Direction",
		&showCamera_
	);
	settingsChanged |= ImGui::Checkbox(
		"Show Colliders",
		&showColliders_
	);
	settingsChanged |= ImGui::Checkbox(
		"Show CameraPath",
		&showCameraPath_
	);
	settingsChanged |= ImGui::Checkbox(
		"Show CameraPath Point Camera Direction",
		&showCameraPathPointCamera_
	);
	DrawSkeletonControls(settingsChanged);
	if (settingsChanged && document) {
		SaveSettings(*document);
	}
	ImGui::End();
#else
	(void)document;
	(void)objectSystem;
	(void)paused;
#endif
}

void SceneDebugSystem::DrawAnimationControls(
	const SceneDocument& document,
	SceneObjectSystem& objectSystem
) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	ImGui::SeparatorText("Animation");
	std::vector<const SceneEntity*> animatorEntities;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (FindEnabledComponent(entity, "Animator")) {
			animatorEntities.push_back(&entity);
		}
	}

	if (animatorEntities.empty()) {
		animationEntityId_ = 0;
		ImGui::TextDisabled("No enabled Animator components");
		return;
	}

	auto selected = std::find_if(
		animatorEntities.begin(),
		animatorEntities.end(),
		[this](const SceneEntity* entity) {
			return entity->id == animationEntityId_;
		}
	);
	if (selected == animatorEntities.end()) {
		selected = animatorEntities.begin();
		animationEntityId_ = (*selected)->id;
		const SceneObjectSystem::ModelRuntime* runtime =
			objectSystem.FindModelRuntime(animationEntityId_);
		if (runtime) {
			animationTransitionDuration_ =
				runtime->animatorTransitionDuration;
		}
	}

	const SceneEntity* selectedEntity = *selected;
	if (ImGui::BeginCombo("Animator Entity", selectedEntity->name.c_str())) {
		for (const SceneEntity* entity : animatorEntities) {
			const std::string itemLabel = entity->name + "##" +
				std::to_string(entity->id);
			if (ImGui::Selectable(
				itemLabel.c_str(),
				entity->id == animationEntityId_
			)) {
				animationEntityId_ = entity->id;
				selectedEntity = entity;
				const SceneObjectSystem::ModelRuntime* runtime =
					objectSystem.FindModelRuntime(entity->id);
				if (runtime) {
					animationTransitionDuration_ =
						runtime->animatorTransitionDuration;
				}
			}
		}
		ImGui::EndCombo();
	}

	SceneObjectSystem::ModelRuntime* runtime =
		objectSystem.FindModelRuntime(animationEntityId_);
	if (!runtime || !runtime->object || !runtime->object->HasAnimation()) {
		ImGui::TextDisabled("The selected model has no animation clips");
		return;
	}

	Object3d* object = runtime->object.get();
	size_t currentClip = object->GetAnimationClipIndex();
	std::string currentClipName = object->GetAnimationClipName(currentClip);
	if (currentClipName.empty()) {
		currentClipName = "Clip " + std::to_string(currentClip);
	}
	if (ImGui::BeginCombo("Clip", currentClipName.c_str())) {
		for (size_t index = 0; index < object->GetAnimationClipCount(); ++index) {
			std::string clipName = object->GetAnimationClipName(index);
			if (clipName.empty()) {
				clipName = "Clip " + std::to_string(index);
			}
			const std::string itemLabel = clipName + "##" +
				std::to_string(index);
			if (ImGui::Selectable(itemLabel.c_str(), index == currentClip)) {
				if (object->PlayAnimation(
					index,
					animationTransitionDuration_,
					true
				)) {
					currentClip = index;
				}
			}
		}
		ImGui::EndCombo();
	}

	bool playing = object->IsAnimationPlaying();
	if (ImGui::Checkbox("Playing", &playing)) {
		object->SetAnimationPlaying(playing);
	}
	bool looping = object->IsAnimationLooping();
	if (ImGui::Checkbox("Loop", &looping)) {
		object->SetAnimationLoop(looping);
	}
	float speed = object->GetAnimationSpeed();
	if (ImGui::DragFloat("Speed", &speed, 0.01f, -8.0f, 8.0f)) {
		object->SetAnimationSpeed(speed);
	}
	ImGui::DragFloat(
		"Transition Duration",
		&animationTransitionDuration_,
		0.01f,
		0.0f,
		10.0f
	);
	animationTransitionDuration_ = (std::max)(
		animationTransitionDuration_,
		0.0f
	);

	const char* blendCurve =
		object->GetAnimationBlendCurve() == AnimationBlendCurve::Linear
		? "Linear"
		: "SmoothStep";
	if (ImGui::BeginCombo("Blend Curve", blendCurve)) {
		for (const char* candidate : { "Linear", "SmoothStep" }) {
			if (ImGui::Selectable(
				candidate,
				std::strcmp(candidate, blendCurve) == 0
			)) {
				object->SetAnimationBlendCurve(
					std::strcmp(candidate, "Linear") == 0
					? AnimationBlendCurve::Linear
					: AnimationBlendCurve::SmoothStep
				);
			}
		}
		ImGui::EndCombo();
	}

	float time = object->GetAnimationTime();
	const float duration = object->GetAnimationDuration();
	if (ImGui::SliderFloat("Time", &time, 0.0f, duration)) {
		object->SetAnimationTime(time);
	}
	if (ImGui::Button("Restart")) {
		object->PlayAnimation(currentClip, 0.0f, true);
	}
	if (object->IsAnimationTransitioning()) {
		ImGui::SameLine();
		ImGui::Text(
			"Blend %.0f%%",
			object->GetAnimationBlendWeight() * 100.0f
		);
	}
#else
	(void)document;
	(void)objectSystem;
#endif
}

void SceneDebugSystem::DrawSkeletonControls(bool& settingsChanged) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	ImGui::SeparatorText("Skeleton");
	settingsChanged |= ImGui::Checkbox("Show Skeleton", &showSkeleton_);
	if (!showSkeleton_) {
		return;
	}

	settingsChanged |= ImGui::Checkbox("Show Joint Names", &showJointNames_);
	settingsChanged |= ImGui::Checkbox("Show Joint Axes", &showJointAxes_);
	settingsChanged |= ImGui::DragFloat(
		"Joint Radius",
		&jointRadius_,
		0.001f,
		0.002f,
		0.1f
	);
	settingsChanged |= ImGui::DragFloat(
		"Joint Axis Length",
		&jointAxisLength_,
		0.002f,
		0.01f,
		0.5f
	);
#else
	(void)settingsChanged;
#endif
}

void SceneDebugSystem::AddDebugDraw(
	const SceneDocument* document,
	SceneObjectSystem& objectSystem,
	const SceneCameraSystem& cameraSystem,
	Camera* runtimeCamera,
	bool runtimeActive,
	bool playing,
	bool paused
) const {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	// Debug表示はRuntime状態を変更せず、確定済みTransformから描画命令だけを作る。
	if (!document) {
		return;
	}

	if (showCamera_) {
		const float aspectRatio = runtimeCamera
			? runtimeCamera->GetAspectRatio()
			: 1.0f;
		for (const SceneEntity& entity : document->GetEntities()) {
			const SceneComponent* cameraComponent =
				FindEnabledComponent(entity, "Camera");
			if (!cameraComponent) {
				continue;
			}

			const SceneComponent* thirdPersonCamera =
				FindEnabledComponent(entity, "ThirdPersonCamera");
			if (thirdPersonCamera && playing && !paused) {
				continue;
			}
			const bool useRuntimeCamera =
				runtimeCamera &&
				thirdPersonCamera &&
				HasComponent(entity, "PlayerBehavior") &&
				(runtimeActive || paused);
			if (useRuntimeCamera) {
				AddCameraDebugDraw(
					*document,
					entity,
					*cameraComponent,
					aspectRatio,
					nullptr,
					runtimeCamera
				);
				AddThirdPersonCameraDebugDraw(
					*document,
					entity,
					*thirdPersonCamera,
					runtimeCamera->GetTranslate()
				);
			} else {
				AddCameraDebugDraw(
					*document,
					entity,
					*cameraComponent,
					aspectRatio
				);
			}
		}
	}

	AddCameraPathDebugDraw(
		*document,
		cameraSystem,
		runtimeCamera
	);
	if (showColliders_) {
		AddColliderDebugDraw(*document, objectSystem);
	}
	if (showSkeleton_) {
		AddSkeletonDebugDraw(*document, objectSystem);
	}
#else
	(void)document;
	(void)objectSystem;
	(void)cameraSystem;
	(void)runtimeCamera;
	(void)runtimeActive;
	(void)playing;
	(void)paused;
#endif
}

void SceneDebugSystem::AddCameraPathDebugDraw(
	const SceneDocument& document,
	const SceneCameraSystem& cameraSystem,
	Camera* runtimeCamera
) const {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (!showCameraPath_ && !showCameraPathPointCamera_) {
		return;
	}

	DebugRenderer* renderer = DebugRenderer::GetInstance();
	const float aspectRatio = runtimeCamera
		? runtimeCamera->GetAspectRatio()
		: 1.0f;
	SceneComponent pointDebugCamera{};
	pointDebugCamera.type = "Camera";
	pointDebugCamera.enabled = true;
	pointDebugCamera.cameraFovY = 0.45f;
	pointDebugCamera.cameraNearClip = 0.1f;
	pointDebugCamera.cameraFarClip = 8.0f;
	pointDebugCamera.cameraIsMain = false;

	for (const SceneEntity& pathEntity : document.GetEntities()) {
		if (
			!IsEntityActiveInHierarchy(document, pathEntity) ||
			!FindEnabledComponent(pathEntity, "CameraPath")
		) {
			continue;
		}

		std::vector<Transform> points;
		std::vector<const SceneEntity*> pointEntities;
		for (const SceneEntity& pointEntity : document.GetEntities()) {
			if (
				pointEntity.parentId != pathEntity.id ||
				!IsEntityActiveInHierarchy(document, pointEntity) ||
				!FindEnabledComponent(pointEntity, "CameraPathPoint")
			) {
				continue;
			}
			points.push_back(ResolveScene3DTransform(document, pointEntity));
			pointEntities.push_back(&pointEntity);
		}

		for (size_t index = 0; index < points.size(); ++index) {
			const Transform& point = points[index];
			if (showCameraPath_) {
				renderer->AddSphere(
					point.translate,
					0.14f,
					{ 1.0f, 0.45f, 0.2f, 1.0f },
					8
				);
				if (index + 1 < points.size()) {
					renderer->AddLine(
						point.translate,
						points[index + 1].translate,
						{ 1.0f, 0.45f, 0.2f, 1.0f }
					);
				}
			}
			if (
				showCameraPathPointCamera_ &&
				index < pointEntities.size()
			) {
				AddCameraDebugDraw(
					document,
					*pointEntities[index],
					pointDebugCamera,
					aspectRatio,
					&point
				);
			}
		}
	}

	if (
		showCameraPath_ &&
		cameraSystem.IsPathPlaying() &&
		cameraSystem.HasCurrentPathTransform()
	) {
		const Transform& current = cameraSystem.GetCurrentPathTransform();
		renderer->AddSphere(
			current.translate,
			0.18f,
			{ 0.35f, 1.0f, 0.55f, 1.0f },
			8
		);
	}
#else
	(void)document;
	(void)cameraSystem;
	(void)runtimeCamera;
#endif
}

void SceneDebugSystem::AddColliderDebugDraw(
	const SceneDocument& document,
	const SceneObjectSystem& objectSystem
) const {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	DebugRenderer* renderer = DebugRenderer::GetInstance();
	auto drawCollider = [renderer](
		const Collider* collider,
		const Vector4& color,
		const std::string& mode,
		uint32_t segments
	) {
		if (!collider) {
			return;
		}
		const bool drawWire = mode != "Solid";
		const bool drawSolid = mode != "Wireframe";
		Vector4 solidColor = color;
		solidColor.w = (std::min)(solidColor.w, 0.25f);
		if (collider->GetType() == Collider::Type::Sphere) {
			const auto& sphere = static_cast<const SphereCollider&>(*collider);
			if (drawSolid) {
				renderer->AddSolidSphere(
					sphere.GetWorldCenter(),
					sphere.GetRadius(),
					solidColor,
					segments
				);
			}
			if (drawWire) {
				renderer->AddSphere(
					sphere.GetWorldCenter(),
					sphere.GetRadius(),
					color,
					segments
				);
			}
			return;
		}

		const auto& box = static_cast<const OBBCollider&>(*collider);
		const OBBCollider::OBB obb = box.GetOBB();
		if (drawSolid) {
			renderer->AddSolidOBB(
				obb.center,
				obb.axis,
				obb.halfSize,
				solidColor
			);
		}
		if (drawWire) {
			renderer->AddOBB(obb.center, obb.axis, obb.halfSize, color);
		}
	};

	for (const SceneEntity& entity : document.GetEntities()) {
		const SceneObjectSystem::ModelRuntime* runtime =
			objectSystem.FindModelRuntime(entity.id);
		if (
			runtime &&
			runtime->hasCollider &&
			IsEntityActiveInHierarchy(document, entity) &&
			runtime->colliderDebugVisible
		) {
			drawCollider(
				runtime->collider,
				runtime->colliderDebugColor,
				runtime->colliderDebugDrawMode,
				runtime->colliderDebugSegments
			);
		}
	}
#else
	(void)document;
	(void)objectSystem;
#endif
}

void SceneDebugSystem::AddSkeletonDebugDraw(
	const SceneDocument& document,
	const SceneObjectSystem& objectSystem
) const {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		Object3d* modelObject = objectSystem.FindObject(entity.id);
		if (!modelObject || !modelObject->GetSkeleton()) {
			continue;
		}
		modelObject->DrawSkeletonDebug(
			showJointNames_,
			showJointAxes_,
			jointRadius_,
			jointAxisLength_
		);
	}
#else
	(void)document;
	(void)objectSystem;
#endif
}
