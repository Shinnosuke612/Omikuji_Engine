// 役割: Sceneで共有する光源とShadow Mapの実行時リソースを管理する。
#pragma once

#include <memory>
#include <vector>

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
	// ActiveなLight ComponentをHierarchy順で実行時ライトへ変換する。
	void Sync(const SceneDocument* document);
	// Model描画前に、現在のLightとShadow MapをCommandListへ結び付ける。
	void Bind();
	void RenderShadows(const std::vector<Object3d*>& shadowCasters);
	void Finalize();

private:
	DirectXCommon* dxCommon_ = nullptr;
	std::unique_ptr<LightManager> lightManager_;
	std::unique_ptr<ShadowManager> shadowManager_;
};
