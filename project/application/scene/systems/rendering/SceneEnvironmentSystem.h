// 役割: SceneのSkybox、環境反射、水面描画と生成Editorを管理する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../SceneRuntimeObjectBinding.h"
#include "../../../../engine/3d/StarFieldGenerator.h"

class Camera;
class DirectXCommon;
class SceneDocument;
class Skybox;
class WaterSurfaceRenderer;

// SkyboxとWaterSurfaceRendererを所有し、Objectへの環境Map設定は非所有で反映する。
class SceneEnvironmentSystem {
public:
	SceneEnvironmentSystem();
	~SceneEnvironmentSystem();

	void Initialize(DirectXCommon* dxCommon);
	// Object生成後に呼び、Scene設定と各Materialの反射強度を同じ状態へ揃える。
	void Sync(
		const SceneDocument* document,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	);
	void Update(float deltaTime);
	void ApplyRenderCamera(Camera* camera);
	void DrawSkybox();
	void DrawWaterSurfaces(
		const SceneDocument& document,
		Camera* camera,
		uint64_t skipEntityId = 0
	);
	void DrawEditor(SceneDocument& document);
	void Finalize();

private:
	std::unique_ptr<Skybox> skybox_;
	std::unique_ptr<WaterSurfaceRenderer> waterSurfaceRenderer_;
	// 保存元pathと描画用keyを分離し、HDR変換cache keyをSceneへ保存しない。
	std::string environmentSourcePath_;
	std::string environmentMapPath_;
	float reflectionIntensity_ = 0.3f;
	StarFieldGenerator starFieldGenerator_;
};
