// 役割: Prefab全階層の3D/UIを通常Sceneから分離したRenderTargetへ描画する。
#pragma once

#include <cstdint>
#include "../engine/base/PlayerCombatPreview.h"
#include "../engine/base/PrefabAnimationGhostFrame.h"
#include "../engine/scene/SceneReplayPresentation.h"
#include <memory>
#include <map>
#include <string>
#include <unordered_map>

#include <d3d12.h>

#include "../engine/math/Matrix4x4.h"
#include "../engine/math/Vector3.h"

class Camera;
class DirectXCommon;
class Object3d;
class SceneDocument;
class SceneRenderTarget;
class SrvManager;
class SceneObjectSystem;
class SceneTextRenderSystem;
class SceneUIRenderSystem;

class PrefabPreviewRenderer final {
public:
	struct OverlayOptions {
		uint64_t selectedEntityId = 0;
		bool showSkeleton = true;
		bool showJointAxes = false;
		bool showColliders = true;
		bool showCombatVolumes = true;
		bool isolateSelectedCollider = false;
		bool showGrid = true; PlayerCombatPreview::EditVolume editVolume;
	};

	PrefabPreviewRenderer();
	~PrefabPreviewRenderer();

	bool Initialize(DirectXCommon* dxCommon, SrvManager* srvManager);
	// Documentはこの呼出し中だけ借用する。既存描画SystemのPreview専用資源へ同期し、
	// UIは制作座標で描く。Event/Simulation/演出時計を進めず、原本へ書き戻さない。
    // replayFramingもこの呼出し中だけ借用。成功結果だけ表示Cameraへ適用し、編集orbitは保持。
	// animationGhostも呼出し中だけ借用。既存Frame Fence後に専用資源へ同期し、原本/通常構図は変えない。
	void Render(
		const std::string& assetPath,
		const SceneDocument& document,
		const SceneDocument* ghostDocument,
		uint32_t width,
		uint32_t height,
		float yaw,
		float pitch,
		float zoom,
		uint64_t framingSerial,
		const OverlayOptions& overlayOptions,
        const SceneReplayFramingRequest* replayFraming = nullptr, const PlayerCombatPreview::ViewRequest* attackView = nullptr,
        const PrefabAnimationGhost::Frame* animationGhost = nullptr
	);
    // 直近Renderの構図計算結果。GPU/Document参照を返さない。
    bool GetReplayFramingResult(Camera& camera, SceneReplayViewSettings& manual) const {
        if (!replayFramingValid_) return false;
        camera=replayFramingCamera_; manual=replayFramingManual_; return true;
    }
	// 直近Renderの値だけを返す。次Renderを越えて内部Cameraを借用させない。
	const PlayerCombatPreview::ViewResult& GetAttackViewResult() const { return attackViewResult_; }
	// 直近Renderと同じ画像/Cameraに対応する値結果。次Renderを越える場合はコピーする。
	const PrefabAnimationGhost::Result& GetAnimationGhostResult() const { return animationGhostResult_; }
	void Finalize();

	D3D12_GPU_DESCRIPTOR_HANDLE GetTexture() const;
	uint32_t GetWidth() const;
	uint32_t GetHeight() const;
	const Matrix4x4& GetViewMatrix() const { return viewMatrix_; }
	const Matrix4x4& GetProjectionMatrix() const { return projectionMatrix_; }

private:
	struct ModelRuntime {
		std::unique_ptr<Object3d> object;
		std::string modelPath;
	};

    bool TryGetReplayFraming(const SceneDocument&, uint64_t owner, SceneReplayFramingBasis&) const;
    PlayerCombatPreview::ViewResult attackViewResult_; bool replayFramingValid_=false;
    Camera replayFramingCamera_;
    SceneReplayViewSettings replayFramingManual_;
	void SyncModels(const SceneDocument& document);
	void SyncGhostModels(const SceneDocument* document);
	void SyncAnimationGhostModels(const PrefabAnimationGhost::Frame* frame);
	void DrawAnimationGhostModels();
	void UpdateFraming(
		const SceneDocument& document,
		const OverlayOptions& options
	);
	void DrawEditorOverlays(
		const SceneDocument& document,
		const SceneDocument* ghostDocument,
		const OverlayOptions& options
	);

	// Preview専用の既存描画System。Simulationは持たず、FinalizeでGPU借用元より先に破棄する。
	std::unique_ptr<SceneObjectSystem> uiObjects_;
	std::unique_ptr<SceneTextRenderSystem> uiTexts_;
	std::unique_ptr<SceneUIRenderSystem> uiRenderer_;
	DirectXCommon* dxCommon_ = nullptr;
	SrvManager* srvManager_ = nullptr;
	SceneRenderTarget* renderTarget_ = nullptr;
	Camera* camera_ = nullptr;
	std::unordered_map<uint64_t, ModelRuntime> models_;
	std::unordered_map<uint64_t, ModelRuntime> ghostModels_;
	// poseSerialを切り替える時は前Frameの既存Fence完了後、Draw記録前に破棄する。
	std::map<std::pair<uint64_t, uint64_t>, ModelRuntime> animationGhostModels_;
	struct GhostDraw { std::pair<uint64_t, uint64_t> key; float distance = 0; };
	std::vector<GhostDraw> animationGhostDraws_;
	uint64_t animationGhostPoseSerial_ = 0;
	PrefabAnimationGhost::Result animationGhostResult_;
	std::string framedAssetPath_;
	uint64_t appliedFramingSerial_ = 0;
	bool framingInitialized_ = false;
	Vector3 orbitTarget_{};
	float fitDistance_ = 5.0f;
	Matrix4x4 viewMatrix_ = MakeIdentity4x4();
	Matrix4x4 projectionMatrix_ = MakeIdentity4x4();
};
