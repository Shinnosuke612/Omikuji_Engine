// 役割: Sceneで共有する光源とShadow Mapの実行時リソースを管理する。
#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include "../../../../engine/scene/SceneRuntimePreparation.h"

class DirectXCommon;
class LightManager;
class Object3d;
class SceneDocument;
class ShadowManager;
class SrvManager;

// LightManagerとShadowManagerを対で所有し、通常描画とShadow Passで共有する。
class SceneLightingSystem {
public:
	SceneLightingSystem();
	~SceneLightingSystem();

	void Initialize(
		DirectXCommon* dxCommon,
		SrvManager* srvManager
	);
    // 新規Replay instanceへCPU設定を複製。元SceneのShadow Map/定数bufferを共有しない。
    bool TryInitializeReplay(DirectXCommon*, SrvManager*, const SceneLightingSystem& source);
    uint64_t EstimateReplayGpuBytes() const;
	// ActiveなLight ComponentをHierarchy順で実行時ライトへ変換する。
	void Sync(const SceneDocument* document);
	RuntimePrepareStep StepPrepared(const SceneDocument&, const SceneRuntimePrepareBudget&, std::string&);
	// Model描画前に、現在のLightとShadow MapをCommandListへ結び付ける。
	void Bind();
	void RenderShadows(const std::vector<Object3d*>& shadowCasters);
	void Finalize();

private:
	DirectXCommon* dxCommon_ = nullptr;
	std::unique_ptr<LightManager> lightManager_;
	std::unique_ptr<ShadowManager> shadowManager_;
	enum class PrepareStage { Light, Shadow, Sync, Ready };
	PrepareStage prepareStage_ = PrepareStage::Light;
};
