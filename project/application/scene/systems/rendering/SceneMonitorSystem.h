// 役割: MonitorRenderer用のCamera、RenderTarget、Offscreen描画状態を管理する。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../SceneRuntimeObjectBinding.h"
#include "../../../../engine/math/Vector3.h"

class Camera;
class DirectXCommon;
class Object3d;
class SceneCameraSystem;
class SceneDocument;
class SceneRenderTarget;
class SrvManager;

// Monitor EntityごとのCameraとRenderTargetを所有する。
// ObjectのTexture Overrideは借用状態なので、Object破棄前にFinalizeで必ず解除する。
class SceneMonitorSystem {
public:
	using DrawSceneCallback = std::function<void(Camera*, uint64_t)>;

	SceneMonitorSystem();
	~SceneMonitorSystem();

	void Initialize(DirectXCommon* dxCommon, SrvManager* srvManager);
	void DrawOffscreen(
		const SceneDocument* document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		SceneCameraSystem& cameraSystem,
		const DrawSceneCallback& drawScene
	);
	void DrawEditor(const SceneDocument* document, Camera* sceneViewCamera);
	void Finalize(
		const std::vector<SceneRuntimeObjectBinding>* bindings = nullptr
	);

private:
	struct MonitorRuntime {
		std::unique_ptr<Camera> camera;
		std::unique_ptr<SceneRenderTarget> renderTarget;
		uint32_t width = 512;
		uint32_t height = 512;
		bool hideSelf = true;
		uint64_t debugPassFrame = 0;
		bool debugResolvedCamera = false;
		bool debugRendered = false;
		bool debugTextureApplied = false;
		uint64_t debugTargetCameraId = 0;
		std::string debugTargetCameraName;
		Vector3 debugTargetTranslate{};
		Vector3 debugTargetRotate{};
		bool debugTargetIsMain = false;
		bool debugTargetHasPlayerBehavior = false;
		uint64_t debugSrvPtr = 0;
		uint64_t debugAppliedTextureOverridePtr = 0;
		std::string debugStatus = "Waiting for offscreen pass";
	};

	void Sync(
		const SceneDocument* document,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	);
	void ClearTextureOverride(
		uint64_t entityId,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	) const;
	Object3d* FindObject(
		uint64_t entityId,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	) const;

	DirectXCommon* dxCommon_ = nullptr;
	SrvManager* srvManager_ = nullptr;
	std::unordered_map<uint64_t, MonitorRuntime> runtimes_;
	uint64_t debugFrame_ = 0;
	bool debugForceProbeCamera_ = false;
};
