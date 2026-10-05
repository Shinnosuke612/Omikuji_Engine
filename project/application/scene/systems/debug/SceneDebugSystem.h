// 役割: SceneのDebug設定、Editor操作、補助描画を管理する。
#pragma once

#include <cstdint>

class Camera;
class SceneCameraSystem;
class SceneDocument;
class SceneObjectSystem;

// SceneDebugSettingsを永続値、メンバをEditor表示用キャッシュとして扱う。
// DebugRendererやObjectは所有せず、毎フレーム描画命令だけを登録する。
class SceneDebugSystem {
public:
	void LoadSettings(const SceneDocument* document);
	void DrawEditor(
		SceneDocument* document,
		SceneObjectSystem& objectSystem,
		bool paused
	);
	void AddDebugDraw(
		const SceneDocument* document,
		SceneObjectSystem& objectSystem,
		const SceneCameraSystem& cameraSystem,
		Camera* runtimeCamera,
		bool runtimeActive,
		bool playing,
		bool paused
	) const;

private:
	void SaveSettings(SceneDocument& document) const;
	void DrawAnimationControls(
		const SceneDocument& document,
		SceneObjectSystem& objectSystem
	);
	void DrawSkeletonControls(bool& settingsChanged);
	void AddCameraPathDebugDraw(
		const SceneDocument& document,
		const SceneCameraSystem& cameraSystem,
		Camera* runtimeCamera
	) const;
	void AddColliderDebugDraw(
		const SceneDocument& document,
		const SceneObjectSystem& objectSystem
	) const;
	void AddSkeletonDebugDraw(
		const SceneDocument& document,
		const SceneObjectSystem& objectSystem
	) const;

	bool showSkeleton_ = false;
	bool showCamera_ = false;
	bool showColliders_ = false;
	bool showCameraPath_ = true;
	bool showCameraPathPointCamera_ = true;
	bool showJointNames_ = false;
	bool showJointAxes_ = true;
	uint64_t animationEntityId_ = 0;
	float animationTransitionDuration_ = 0.2f;
	float jointRadius_ = 0.018f;
	float jointAxisLength_ = 0.06f;
};
