// 役割: エディタとゲーム実行を統合し、フレーム更新と描画順序を管理する。
#pragma once
#include "../engine/base/Framework.h"
#include "../engine/base/BloomRenderer.h"
#include "../engine/scene/SceneDocument.h"
#include "../engine/scene/SceneManager.h"
#include "../engine/math/Vector3.h"
#include "../engine/math/Vector4.h"
#include "../engine/math/Matrix4x4.h"
#include <filesystem>
#include <string>
#include <memory>
#include <vector>
#include <d3d12.h>
#include <wrl.h>

class SceneRenderTarget;
class FullscreenCopy;
class EditorBootstrap;
class EditorSession;
class RuntimeBootstrap;
class SceneCatalog;
class SceneAssetService;
class SceneExecutionContext;
class SceneStartupErrorScreen;
class SceneTemplateRegistry;
class PrefabPreviewRenderer;
class Camera;
class Object3d;
class Model;
class ProjectRegistry;
class ProjectCreationService;
class ProjectSolutionGenerationService;
class VisualStudioLocator;
struct ProjectLauncherView;

class Game : public Framework {
public:
	Game();
	~Game() override;
	void Initialize() override;
	void Finalize() override;
	void Update() override;
	void Draw() override;

private:
	struct PresentedTransitionState;
	std::unique_ptr<PresentedTransitionState> presentedTransition_;
	// 資源交換/取消は前Frameの全利用完了後。Drawは借用Displayへcommandを記録するだけ。
	void CancelPresentedTransition(const std::string& reason = {});
	void UpdatePresentedTransitionBeforeScene(float delta, bool paused);
	void UpdatePresentedTransitionAfterScene();
	D3D12_GPU_DESCRIPTOR_HANDLE DrawPresentedTransition(D3D12_GPU_DESCRIPTOR_HANDLE source, uint32_t width, uint32_t height);
	bool IsPresentedTransitionDrawable() const;
	struct CameraSnapshot {
		bool valid = false;
		bool orbitMode = false;
		Vector3 translate{};
		Vector3 rotate{};
		Vector3 orbitTarget{};
		float orbitDistance = 10.0f;
		float orbitYaw = 0.0f;
		float orbitPitch = 0.0f;
		float fovY = 0.45f;
		float aspectRatio = 1.0f;
		float nearClip = 0.1f;
		float farClip = 1000.0f;
	};

	struct WaterPostEffectState {
		bool hasVolume = false;
		bool cameraInside = false;
		Vector3 center{};
		Vector3 halfSize{};
		bool lightShaftEnabled = false;
		Vector4 lightColor{ 0.55f, 0.90f, 1.15f, 1.0f };
		Vector3 lightDirection{ -0.25f, -1.0f, 0.18f };
		float lightIntensity = 0.0f;
		float lightDensity = 0.045f;
		float causticsIntensity = 0.35f;
		float causticsScale = 0.08f;
		float causticsSpeed = 1.0f;
		float lightBreakupStrength = 1.0f;
		float lightWarpStrength = 1.0f;
		float lightNoiseScale = 1.0f;
		int lightSampleCount = 16;
	};

	int GetEnabledPostEffectCount() const;
	ScenePostProcessSettings CapturePostProcessSettings() const;
	void ApplyPostProcessSettings(const ScenePostProcessSettings& settings);
	void ApplyRuntimePostProcessSettings();
	std::string GetActivePostProcessSourceKey() const;
	void StorePostProcessSettingsToDocument();
	void InvalidateMotionBlurHistory();
	WaterPostEffectState ResolveWaterPostEffectState(const Camera* camera) const;
	void DrawModelPreview();
	void DrawPrefabPreview();
	// Ghost GPU資源は前Frame Fence完了後のUpdateとFinalizeだけで作成/破棄する。
	void PrepareSceneSpatialPreview();
	void DrawSceneSpatialPreview();
	void ClearSceneSpatialPreview();
	struct SpatialPreviewModel {
		Model* model = nullptr; // ModelManager所有。GPU geometryを姿勢ごとに複製しない。
		D3D12_GPU_VIRTUAL_ADDRESS matrixAddress = 0;
		size_t materialGroup = 0;
		uint32_t poseId = 0;
		bool moving = false;
		uint64_t entityId = 0;
		std::string cullMode;
		Vector3 worldPosition{};
	};
	struct SpatialPreviewMaterials {
		Model* model = nullptr;
		Vector4 tint{};
		bool used = false, valid = false;
		std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> owned;
		std::vector<ID3D12Resource*> resources;
	};
	// 前FrameのFence完了後だけ更新/破棄する。cameraと各poseのCB領域は256-byte境界。
	Microsoft::WRL::ComPtr<ID3D12Resource> spatialPreviewBuffer_;
	unsigned char* spatialPreviewMapped_ = nullptr;
	size_t spatialPreviewCapacity_ = 0;
	std::vector<SpatialPreviewMaterials> spatialPreviewMaterials_;
	std::vector<SpatialPreviewModel> spatialPreviewModels_;
	struct SpatialPreviewLine { Vector3 start{}, end{}; Vector4 color{}; };
	std::vector<SpatialPreviewLine> spatialPreviewLines_;
	uint64_t spatialPreviewGeneration_ = 0, spatialPreviewRevision_ = 0;
	uint64_t spatialPreviewSerial_ = 0;
	std::string spatialPreviewIssue_;
	CameraSnapshot CaptureCameraSnapshot() const;
	void RestoreCameraSnapshot(const CameraSnapshot& snapshot) const;
	void BeginPauseDebugCamera();
	void EndPauseDebugCamera();
	void ProcessSceneAssetRequests();
	void ProcessSceneInstanceRequests();
	void ProcessProjectSettingsRequests();
	void InitializeProjectLauncher();
	void UpdateProjectLauncher();
	void RefreshProjectLauncherView();
	bool ProcessProjectLauncherRequest();
	bool LaunchProjectSolution(
		const std::filesystem::path& descriptorPath,
		const std::string& requestedInstanceId,
		std::string& errorMessage
	);
	bool LaunchPreviewSolution(
		const std::filesystem::path& solutionPath,
		const std::string& requestedInstanceId,
		std::string& errorMessage
	);
	bool LaunchProjectFolder(
		const std::filesystem::path& descriptorPath,
		std::string& errorMessage
	);
	bool LaunchProjectEditor(
		const std::filesystem::path& descriptorPath,
		bool switchCurrentEditor,
		std::string& errorMessage
	);
	bool OpenRegisteredEditScene(
		const std::string& sceneId,
		bool discardUnsavedChanges,
		std::string& errorMessage
	);

	SceneManager* sceneManager_ = nullptr;
	SceneExecutionContext* executionContext_ = nullptr;
	EditorSession* editorSession_ = nullptr;
	EditorBootstrap* editorBootstrap_ = nullptr;
	RuntimeBootstrap* runtimeBootstrap_ = nullptr;
	SceneCatalog* sceneCatalog_ = nullptr;
	SceneAssetService* sceneAssetService_ = nullptr;
	SceneStartupErrorScreen* startupErrorScreen_ = nullptr;
	SceneTemplateRegistry* sceneTemplateRegistry_ = nullptr;
	SceneRenderTarget* sceneRenderTarget_ = nullptr;
	SceneRenderTarget* postProcessRenderTargets_[2]{};
	SceneRenderTarget* textOverlayRenderTarget_ = nullptr;
	SceneRenderTarget* motionBlurHistoryRenderTarget_ = nullptr;
	SceneRenderTarget* foregroundComposeRenderTarget_ = nullptr;
	FullscreenCopy* fullscreenCopy_ = nullptr;
	BloomRenderer* bloomRenderer_ = nullptr;
	SceneRenderTarget* modelPreviewRenderTarget_ = nullptr;
	Camera* modelPreviewCamera_ = nullptr;
	Object3d* modelPreviewObject_ = nullptr;
	PrefabPreviewRenderer* prefabPreviewRenderer_ = nullptr;
	ProjectRegistry* projectRegistry_ = nullptr;
	ProjectCreationService* projectCreationService_ = nullptr;
	ProjectSolutionGenerationService* projectSolutionGenerationService_ = nullptr;
	VisualStudioLocator* visualStudioLocator_ = nullptr;
	std::string projectLauncherStatusMessage_;
	bool projectLauncherInitialized_ = false;
	bool projectLauncherViewDirty_ = true;
	bool projectLauncherDynamicStateInitialized_ = false;
	bool projectLauncherSceneDirty_ = false;
	bool projectLauncherPrefabDirty_ = false;
	bool projectLauncherPlayBlocked_ = false;
	bool pendingCreateOpenSolution_ = false;
	std::string pendingCreateVisualStudioInstanceId_;
	CameraSnapshot editorCameraSnapshot_{};
	CameraSnapshot pauseMainCameraSnapshot_{};
	bool editorWasEditingLastFrame_ = true;
	bool wasPausedLastFrame_ = false;
	uint64_t appliedPostProcessRevision_ = static_cast<uint64_t>(-1);
	std::string appliedPostProcessSourceKey_;
	uint64_t appliedRuntimePostProcessGeneration_ = static_cast<uint64_t>(-1);
	SceneInstanceId appliedRuntimePostProcessInstanceId_ = kInvalidSceneInstanceId;
	std::string modelPreviewPath_;
	float modelPreviewFitDistance_ = 5.0f;
	BloomRenderer::Parameters bloomParameters_{};
	float baseExposure_ = 1.0f;
	float currentExposure_ = 1.0f;
	float exposureReturnSpeed_ = 6.0f;
	bool grayscaleEnabled_ = false;
	bool vignetteEnabled_ = false;
	bool boxBlurEnabled_ = false;
	bool gaussianBlurEnabled_ = false;
	bool radialBlurEnabled_ = false;
	bool noiseEnabled_ = false;
	bool dissolveEnabled_ = false;
	bool outlineEnabled_ = false;
	bool underwaterEnabled_ = false;
	bool waterRefractionEnabled_ = false;
	bool pixelationEnabled_ = false;
	bool chromaticAberrationEnabled_ = false;
	bool motionBlurEnabled_ = false;
	bool depthOfFieldEnabled_ = false;
	bool outlineLuminanceEnabled_ = false;
	bool outlineDepthEnabled_ = true;
	float vignetteScale_ = 16.0f;
	float vignettePower_ = 0.8f;
	float vignetteIntensity_ = 1.0f;
	int boxBlurKernelSize_ = 3;
	float boxBlurStrength_ = 1.0f;
	int gaussianBlurKernelSize_ = 3;
	float gaussianBlurSigma_ = 1.0f;
	float gaussianBlurStrength_ = 1.0f;
	float radialBlurCenter_[2]{ 0.5f, 0.5f };
	float radialBlurWidth_ = 0.01f;
	int radialBlurSamples_ = 10;
	bool noiseAnimate_ = true;
	float noiseTime_ = 0.0f;
	float noiseAmount_ = 0.25f;
	float noiseScale_ = 1.0f;
	float noiseSpeed_ = 1.0f;
	float noiseSeed_ = 0.0f;
	int dissolveMaskIndex_ = 0;
	float dissolveThreshold_ = 0.0f;
	float dissolveEdgeWidth_ = 0.03f;
	float dissolveEdgeColor_[4]{ 1.0f, 0.4f, 0.3f, 1.0f };
	float outlineLuminanceWeight_ = 1.0f;
	float outlineDepthWeight_ = 1.0f;
	float outlineThreshold_ = 0.1f;
	float outlineSoftness_ = 0.05f;
	float outlineThickness_ = 1.0f;
	float outlineColor_[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
	float dofFocusDistance_ = 10.0f;
	float dofFocusRange_ = 2.0f;
	float dofBlurStrength_ = 1.0f;
	float dofNearStrength_ = 0.0f;
	float dofFarStrength_ = 1.0f;
	float dofMaxRadius_ = 4.0f;
	float underwaterTintColor_[4]{ 0.02f, 0.45f, 0.68f, 1.0f };
	float underwaterIntensity_ = 0.65f;
	float underwaterFogDensity_ = 0.035f;
	float underwaterDistortion_ = 0.012f;
	float waterRefractionTintColor_[4]{ 0.02f, 0.55f, 0.82f, 1.0f };
	float waterRefractionStrength_ = 0.018f;
	float waterRefractionEdgeSoftness_ = 0.08f;
	float waterRefractionTintStrength_ = 0.12f;
	int pixelationBlockSize_ = 4;
	float motionBlurStrength_ = 0.5f;
	int motionBlurSamples_ = 8;
	float motionBlurMaxRadius_ = 24.0f;
	Matrix4x4 previousMotionBlurViewProjection_{};
	bool motionBlurHistoryValid_ = false;
	bool motionBlurWasEnabled_ = false;
	bool motionBlurLastPlaying_ = false;
	SceneInstanceId motionBlurLastSceneInstanceId_ = kInvalidSceneInstanceId;
	float chromaticAberrationCenter_[2]{ 0.5f, 0.5f };
	float chromaticAberrationIntensity_ = 0.003f;
	float chromaticAberrationFalloff_ = 1.5f;
};
