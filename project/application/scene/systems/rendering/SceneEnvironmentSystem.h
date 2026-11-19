// 役割: SceneのSkybox、環境反射、水面描画と生成Editorを管理する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../../../engine/effect/WaterSurfaceRenderer.h"
#include "../../../../engine/scene/SceneRuntimePreparation.h"

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
	RuntimePrepareStep StepPreparedSkybox(const SceneDocument&, Camera&,
		const std::vector<SceneRuntimeObjectBinding>&, const SceneRuntimePrepareBudget&, std::string&);
	// Object生成後に呼び、Scene設定と各Materialの反射強度を同じ状態へ揃える。
	void Sync(
		const SceneDocument* document,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	);
    struct ReplayWater { Vector3 center{}, halfSize{}; WaterSurfaceRenderer::Settings settings; };
    struct ReplaySnapshot { std::string skyboxKey; Vector4 skyboxColor{}; std::vector<ReplayWater> waters; };
    // 呼出中だけSceneを参照し、GPU資源/Entity参照を含まない値を返す。
    ReplaySnapshot CaptureReplayEnvironment(const SceneDocument&) const;
	void Update(float deltaTime);
	bool TryGetWaterTime(const SceneDocument& document, float& out) const; // 描画対象の水面がなければout不変。
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
	std::unique_ptr<Skybox> preparingSkybox_;
	size_t preparedBindingCursor_ = 0;
	std::unique_ptr<WaterSurfaceRenderer> waterSurfaceRenderer_;
	// 保存元pathと描画用keyを分離し、HDR変換cache keyをSceneへ保存しない。
	std::string environmentSourcePath_;
	std::string environmentMapPath_;
	float reflectionIntensity_ = 0.3f;
	StarFieldGenerator starFieldGenerator_;
};
