// 役割: エディタのDockSpace、Hierarchy、Inspector、Project、Scene Viewを管理する。
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include "../Audio/Audio.h"
#include "../math/Quaternion.h"
#include "../math/Matrix4x4.h"
#include "../math/Transform.h"
#include "../math/Vector2.h"
#include "../math/Vector4.h"
#include "../scene/PrefabAssetRegistry.h"
#include "../scene/SceneUIEditorView.h"
#include "../scene/ScenePreparationDiagnostic.h"
#include "EditorLocalization.h"
#include "PlayerCombatEditor.h"
#include "../scene/SceneReplayPresentation.h"

#include <d3d12.h>
#include "../../externals/imgui/imgui.h"

enum class ProjectAccessIssue;
class WinApp;
class DirectXCommon;
class SrvManager;
class EditorSession;
class PrefabEditorSession;
class SceneCatalog;
class SceneDocument;
class SceneManager;
class SceneTemplateRegistry;
struct SceneEntity;
struct SceneComponent;
struct SceneEventBinding;
struct SceneAnimationTrack;
struct ScenePrefabAnimationClip;

// Scene View専用の値型packet。Document/GPU pointerを描画側へ渡さない。
struct SceneSpatialPreviewFrame {
	struct Mesh {
		uint32_t poseId = 0;
		bool moving = false;
		uint64_t entityId = 0;
		std::string modelPath;
		std::string meshCullMode;
		Matrix4x4 world{};
		Vector4 tint{};
	};
	struct Line { Vector3 start{}, end{}; Vector4 color{}; };
	struct KeyPoint {
		size_t trackIndex = 0, keyIndex = 0;
		float keyTime = 0.0f;
		uint64_t targetId = 0;
		Vector3 worldPosition{};
		Matrix4x4 world{};
		float sourceTime = 0.0f;
		unsigned trsMask = 0;
		bool active = true;
	};
	uint64_t sourceGeneration = 0, sourceRevision = 0, ownerId = 0;
	uint64_t serial = 0;
	size_t staticPoseCount = 0;
	std::string issue;
	int clipIndex = -1;
	std::string clipName;
	std::vector<Mesh> meshes;
	std::vector<Line> lines;
	std::vector<KeyPoint> keyPoints;
};
enum class SceneBuildConfiguration : uint8_t;
enum class SceneStartupMode : uint8_t;

enum class SceneAssetOperation {
	None,
	Create,
	Duplicate,
	Rename,
	Delete
};

struct SceneAssetRequest {
	SceneAssetOperation operation = SceneAssetOperation::None;
	std::string sourceSceneId;
	std::string sceneId;
	std::string displayName;
	std::string assetPath;
	std::string templateId;
};

struct PrefabPreviewRequest {
	const SceneDocument* document = nullptr;
	const SceneDocument* ghostDocument = nullptr;
	std::string assetPath;
	uint64_t revision = 0;
	float yaw = 0.65f;
	float pitch = 0.25f;
	float zoom = 1.0f;
	uint32_t width = 768;
	uint32_t height = 432;
	uint64_t selectedEntityId = 0;
	bool showSkeleton = true;
	bool showJointAxes = false;
	bool showColliders = true;
	bool showCombatVolumes = true;
	bool isolateSelectedCollider = false;
	bool showGrid = true;
	uint64_t framingSerial = 0;
};

enum class SceneInstanceOperation {
	None,
	LoadAdditive,
	Unload,
	SetActive,
	SetPersistent
};

struct SceneInstanceRequest {
	SceneInstanceOperation operation = SceneInstanceOperation::None;
	std::string sceneId;
	std::string instanceKey;
	uint64_t instanceId = 0;
	bool persistent = false;
};

enum class ProjectLauncherRequestOperation {
	None,
	Create,
	Import,
	Remove,
	RetryFinalize,
	SetPinned,
	OpenFolder,
	OpenSolution,
	GenerateSolutionPreview,
	RetrySolutionPreview,
	OpenSolutionPreview,
	Refresh,
	AdoptSolutionPreview,
	AdoptGroupedSolutionLayout,
	CommitStagedSolutionGeneration,
	ResumeSolutionGenerationCommit,
	RecheckSolutionGenerationRecovery,
	RestorePreviousSolutionGeneration,
	OpenEditor,
	SwitchEditor,
	RegenerateSolutionPreview
};

struct ProjectLauncherRequest {
	ProjectLauncherRequestOperation operation = ProjectLauncherRequestOperation::None;
	std::string descriptorPath;
	std::string operationId;
	std::string projectId;
	std::string displayName;
	std::string destinationRoot;
	std::string startSceneId;
	std::string visualStudioInstanceId;
	bool openSolutionAfterCreate = false;
	bool pinned = false;
};

struct ProjectLauncherProjectView {
	std::string descriptorPath;
	std::string displayName;
	std::string projectId;
	std::string projectRoot;
	std::string status;
	std::string detail;
	std::string generationStatus;
	std::string generationDetail;
	std::string previewOperationId;
	std::string previewSolutionPath;
	std::string legacyArtifactDirectory;
	std::string groupedArtifactDirectory;
	uint32_t previewArtifactCount = 0;
	uint32_t retiredArtifactCount = 0;
	uint32_t modifiedOwnedArtifactCount = 0;
	bool pinned = false;
	bool canGenerateSolutionPreview = false;
	// 現在の生成入力と一致し、所有元・生成物を再検証したPreviewだけOpen可能。
	bool canOpenSolutionPreview = false;
	bool canAdoptSolutionPreview = false;
	bool canRegenerateSolutionPreview = false;
	bool canAdoptGroupedSolutionLayout = false;
	bool layoutMigrationRequired = false;
	bool canOpenSolution = false;
	bool canOpenEditor = false;
	bool canSwitchEditor = false;
};

struct ProjectLauncherGenerationRecoveryView {
	std::string operationId;
	std::string projectId;
	std::string state;
	std::string stagingRoot;
	std::string rollbackRoot;
	std::string detail;
	uint32_t fileCount = 0;
	bool canRecheck = false;
	bool canCommitStaged = false;
	bool canResumeCommit = false;
	bool canRestorePrevious = false;
};

struct ProjectLauncherOperationView {
	std::string operationId;
	std::string projectId;
	std::string state;
	std::string detail;
	bool canRetryFinalize = false;
};

struct ProjectLauncherVisualStudioView {
	std::string instanceId;
	std::string displayName;
	std::string detail;
	bool openable = false;
	bool selected = false;
};

struct ProjectLauncherView {
	std::string templateSourceRoot;
	std::string statusMessage;
	std::vector<ProjectLauncherProjectView> projects;
	std::vector<ProjectLauncherOperationView> operations;
	std::vector<ProjectLauncherGenerationRecoveryView> generationRecoveries;
	std::vector<ProjectLauncherVisualStudioView> visualStudioInstances;
	bool creationInProgress = false;
	bool switchBlockedBySceneDirty = false;
	bool switchBlockedByPrefabDirty = false;
	bool switchBlockedByPlayMode = false;
};

class ImGuiManager{
public:
	static ImGuiManager* GetInstance();
	// DirectX初期化前に適用する起動時フルスクリーン設定を読み込む。
	static bool LoadStartFullscreenSetting();

	// 初期化
	void Initialize(WinApp* winApp, DirectXCommon* dxCommon, SrvManager* srvManager);

	// Input更新とPlay/Stop/Pauseの確定後、一Frameに一度呼ぶ。
	void BeginFrame();
	// Play/Resumeの入口で要求する。Sceneへフォーカスを移し、Editorの編集中入力を解除する。
	// DocumentやDirectInputのキー状態は変更しない。
	void RequestGameInputFocus();
	// Editorの入力遮断を適用済みで、ゲームUI/cursor captureへ入力を渡せる状態。
	// 非active・Alt・Modal・表示不能中はfalse。各Componentの生入力経路は変更しない。
	bool IsGameInputOwned() const { return gameInputOwned_; }

	// フレーム終了
	void EndFrame(bool deferPlatformWindows = false);
	// 演出画像のmain送信完了後にだけ呼ぶ。trueなら追加viewportのqueue利用をcallerが待つ。
	bool RenderDeferredPlatformWindows();
	// UI編集Viewの次描画へ渡す値。非表示/Play中も編集対象そのものは保持する。
	SceneUIEditorView GetSceneUIEditorView();
	bool IsUIEditorViewRenderable() const { return sceneUIEditorRenderable_; }
	uint32_t GetUIEditorViewWidth() const { return sceneUIEditorWidth_; }
	uint32_t GetUIEditorViewHeight() const { return sceneUIEditorHeight_; }
	// 描画済みpacketと実寸を保持し、内容/サイズ変更後の旧画像をPickingしない。
	// width/heightは渡したTextureの実寸。無効Textureでは0にして操作を禁止する。
	void SetUIEditorViewTexture(D3D12_GPU_DESCRIPTOR_HANDLE texture, const SceneUIEditorView& view, uint32_t width, uint32_t height) {
		sceneUIEditorTexture_ = texture; sceneUIEditorRenderedView_ = view;
		sceneUIEditorRenderedWidth_ = texture.ptr ? width : 0;
		sceneUIEditorRenderedHeight_ = texture.ptr ? height : 0;
	}
	bool IsSceneViewRenderable() const { return sceneViewRenderable_; }
	struct MainDockGameImageEvidence {
		uint64_t frameSerial = 0, srv = 0;
		uint32_t width = 0, height = 0, viewportId = 0, dockId = 0;
		bool adoptedInMainDrawData = false;
	};
	enum class MainDockGameViewState { Ready, HiddenOrClipped, NotDockedInMain, Unavailable };
	enum class MainDockTransitionIssue { DockRequired, ViewUnavailable, NotPlaying, TextUnsupported,
		ContextChanged, PresentationFailed };
	// 同一FrameのScene Imageをmain command listに記録した証拠。Present/Fenceの照合はGameが行う。
	void RequireMainDockGameImage(uint64_t frameSerial, uint64_t expectedSrv) noexcept;
	MainDockGameViewState GetMainDockGameViewState() const noexcept { return mainDockGameViewState_; }
	bool TryGetMainDockGameImageEvidence(uint64_t frameSerial, uint64_t expectedSrv,
		MainDockGameImageEvidence& out) const noexcept;
	void ClearMainDockGameImageRequirement() noexcept;
	void ReportMainDockTransitionIssue(MainDockTransitionIssue, const std::string& supplementaryDetail = {});
	// Runtimeからの一時表示。Scene/Prefab/Editor設定へ保存しない。
	void SetPresentedTransitionStatus(const std::string& status, bool active = false, bool requiresMainDock = false) {
		// 失敗は既存の警告窓口へ一回だけ渡す。UI要求の診断も同じPopupで扱う。
		if (!active && !status.empty() && (status != presentedTransitionStatus_ || presentedTransitionActive_)) {
			spawnWarningRequested_ = true;
			spawnWarningRuntime_ = true;
			spawnWarningEntityId_ = 0;
			spawnWarningIssue_ = "SceneTransition:";
			spawnWarningDetail_ = status;
		}
		presentedTransitionStatus_ = status; presentedTransitionActive_ = active;
		presentedTransitionRequiresMainDock_ = active && requiresMainDock;
	}
	bool ConsumePresentedTransitionStopRequest() {
		const bool requested = presentedTransitionStopRequested_; presentedTransitionStopRequested_ = false; return requested;
	}

	void DrawEditorWorkspace(
		D3D12_GPU_DESCRIPTOR_HANDLE sceneTexture,
		uint32_t textureWidth,
		uint32_t textureHeight,
		const char* sceneName
	);

    // 選択変更では解除しない。Scene世代/対象消失/Play開始で古いrequestを破棄する。
    bool GetReplayFramingRequest(bool prefab, SceneReplayFramingRequest& out);
    void SetReplayFramingResult(const SceneReplayFramingRequest&, bool success, const SceneReplayViewSettings& manual);
	// 終了処理
	void Finalize();
	// GameのUpdateから一回、秒単位。Preview評価だけではAuthoring値を変更しない。
	void UpdateSceneSpatialPreview(float deltaTime);
	// 次Updateまでの借用参照。callerはFrame越しに保持しない。
	const SceneSpatialPreviewFrame& GetSceneSpatialPreviewFrame() const { return spatialFrame_; }
	// 次Inspector用の一時診断。Scene保存やUndoには含めない。
	// CPU再評価/対象変更後の古い描画結果を新しいpacketへ混ぜない。
	void SetSceneSpatialPreviewRenderIssue(const std::string& message, uint64_t serial = 0,
		size_t prepared = 0, size_t submitted = 0) {
		if (serial != 0 && serial != spatialFrame_.serial) return;
		spatialRenderIssue_ = message; spatialRenderSerial_ = serial;
		spatialPreparedCount_ = prepared; spatialSubmittedCount_ = submitted;
	}

	// 次のCamera/RT更新用の整数寸法。Min/Maxは現在表示した画像の実際の画面座標。
	uint32_t GetSceneViewWidth() const { return sceneViewWidth_; }
	uint32_t GetSceneViewHeight() const { return sceneViewHeight_; }
	float GetSceneViewMinX() const { return sceneViewMinX_; }
	float GetSceneViewMinY() const { return sceneViewMinY_; }
	float GetSceneViewMaxX() const { return sceneViewMaxX_; }
	float GetSceneViewMaxY() const { return sceneViewMaxY_; }
	bool IsSceneGridVisible() const { return sceneGridVisible_; }
	static bool IsSceneViewInputActive();
	void SetEditorSession(EditorSession* editorSession) {
		editorSession_ = editorSession;
	}
	void SetSceneCatalog(SceneCatalog* sceneCatalog) {
		sceneCatalog_ = sceneCatalog;
		projectDirectoryCacheDirty_ = true;
	}
	void SetSceneManager(SceneManager* sceneManager) {
		sceneManager_ = sceneManager;
	}
	void SetSceneTemplateRegistry(
		const SceneTemplateRegistry* sceneTemplateRegistry
	) {
		sceneTemplateRegistry_ = sceneTemplateRegistry;
	}
	void SetProjectLauncherView(const ProjectLauncherView& value);
	// 生成/登録の競合拒否を、選択言語の説明とmodalで通知する。返却値は状態欄用。
	std::string NotifyProjectAccessFailure(ProjectAccessIssue issue, const std::string& detail);
	bool ConsumeOpenSceneRequest(
		std::string& sceneId,
		bool& discardUnsavedChanges
	);
	bool ConsumeSceneAssetRequest(SceneAssetRequest& request);
	bool ConsumeSceneInstanceRequest(SceneInstanceRequest& request);
	bool ConsumeStartSceneRequest(std::string& sceneId);
	bool ConsumeStartupModeRequest(
		SceneBuildConfiguration& configuration,
		SceneStartupMode& mode
	);
	bool ConsumeProjectLauncherRequest(ProjectLauncherRequest& request);
	bool HasUnsavedPrefabChanges() const;
	void NotifySceneAssetOperationResult(
		bool success,
		const std::string& message
	);
	void NotifySceneInstanceOperationResult(
		bool success,
		const std::string& message
	);
	void NotifyProjectSettingsResult(
		bool success,
		const std::string& message
	);
	void NotifyEditSceneOpened();
	void SetModelPreviewTexture(
		const std::string& modelPath,
		D3D12_GPU_DESCRIPTOR_HANDLE texture,
		uint32_t width,
		uint32_t height
	);
	bool GetModelPreviewRequest(
		std::string& modelPath,
		float& yaw,
		float& pitch,
		float& zoom
	) const;
	void SetPrefabPreviewTexture(
		const std::string& assetPath,
		uint64_t revision,
		D3D12_GPU_DESCRIPTOR_HANDLE texture,
		uint32_t width,
		uint32_t height,
		const Matrix4x4& viewMatrix,
		const Matrix4x4& projectionMatrix
	);
	bool GetPrefabPreviewRequest(PrefabPreviewRequest& request);

	// Communication with scenes
	const std::string& GetSelectedProjectFile() const { return selectedProjectFile_; }
	void ClearSelectedProjectFile() { selectedProjectFile_.clear(); }
	bool GetRequestLoadParticle(std::string& outPath) {
		if (requestLoadParticle_) {
			outPath = particleToLoad_;
			requestLoadParticle_ = false;
			return true;
		}
		return false;
	}

private:
	bool deferredPlatformWindows_ = false, sceneViewRenderable_ = false;
	bool presentedTransitionActive_ = false, presentedTransitionStopRequested_ = false;
	bool presentedTransitionRequiresMainDock_ = false;
	std::string presentedTransitionStatus_;
    SceneReplayFramingRequest replayFramingRequest_;
    SceneReplayViewSettings replayFramingManualResult_, replayFramingResultSource_;
    uint64_t replayFramingSerial_=0, replayFramingResultSerial_=0, replayFramingWarningSerial_=0;
    bool replayFramingManualReady_=false;
	struct SpatialPreviewValidation {
		bool hasTransformKeys = false;
		std::vector<uint64_t> targetIds, meshIds;
		std::string structure;
	};
	// 候補の自動選択とcopy評価で同じ安全検証を使い、Runtimeの起動/制御権は判定しない。
	bool ValidateSceneSpatialPreviewClip(uint64_t ownerId, int clipIndex,
		SpatialPreviewValidation& result, std::string& issue) const;
	// 無効Clipも列挙し、選択後の検証/描画診断へ導く。
	std::vector<std::pair<uint64_t, int>> GetSceneSpatialPreviewCandidates() const;
	// Editorの対象と時計を置換し、保存Documentと全体表示設定は変更しない。
	void BindSceneSpatialPreview(uint64_t ownerId, int clipIndex);
	void DrawSceneSpatialPreviewControls();
	void DrawSceneSpatialPreviewPlayback(const SceneComponent& animator);
	bool RebuildSceneSpatialPreview();
	void RefreshSceneSpatialPreviewAfterEdit();
	void SampleSceneSpatialPreview(float time);
	void AppendSceneSpatialPreviewPose(SceneSpatialPreviewFrame& frame, float time,
		uint32_t poseId, const Vector4& tint, bool moving = false, bool alreadySampled = false);
	void DrawSceneSpatialPreviewInspector(SceneEntity& entity, SceneComponent& animator);
	void DrawSceneSpatialPreviewOperations(bool shortcuts);
	void DrawSceneSpatialPreviewKeyEditor(SceneEntity& entity, SceneComponent& animator);
	bool DrawSceneSpatialPreviewOverlay(float x, float y, float width, float height);
	void DrawSceneSpatialPreviewGizmo(float x, float y, float width, float height);
	bool IsSceneSpatialPreviewCurrent() const;
	bool HasSceneSpatialPreviewPointSelection() const;
	void ResetSceneSpatialPreview();
	// 配列番号は描画Frameの解決結果だけ。時刻変更中も正本の元キーを追跡する。
	struct SpatialPointAddress {
		uint64_t generation = 0, ownerId = 0, targetId = 0;
		std::string clipName;
		float time = 0.0f;
	};
	enum class SpatialDraftKind { None, Time, Value, Gizmo };
	struct SpatialDraftKey {
		std::string property;
		Vector3 original{}, value{};
	};
	// 候補はEditor copyにだけ適用し、正本とSession Undoへ渡すのは確定時の一回。
	struct SpatialEditDraft {
		SpatialDraftKind kind = SpatialDraftKind::None;
		SpatialPointAddress point;
		uint64_t sourceRevision = 0;
		int operation = 0;
		float time = 0.0f, validTime = 0.0f;
		bool valid = true, commitRequested = false;
		std::vector<SpatialDraftKey> keys, validKeys;
	};
	bool ResolveSceneSpatialPoint(const SpatialPointAddress& point,
		int& clipIndex, int& trackIndex, int& keyIndex) const;
	void SelectSceneSpatialPoint(int trackIndex, int keyIndex);
	void CancelSceneSpatialDraft(bool clearSelection = false);
	bool BeginSceneSpatialDraft(SpatialDraftKind kind, int operation);
	bool ValidateSceneSpatialDraft(std::string& issue) const;
	// 正本へ書く前に、copy上で不足TRSキーの補完を含む確定形を作る。
	bool ApplySceneSpatialDraft(SceneDocument& document, int clipIndex, bool lastValid) const;
	void UpdateSceneSpatialDraftPreview();
	void CommitSceneSpatialDraft();
	bool CanSaveSceneSpatialDraft();
	// 方式変更も正本へ確定するまでcopyだけに適用する。Scene/Prefab共通の候補所有。
	struct AnimationRotationModeDraft {
		bool active = false, prefab = false;
		uint64_t generation = 0, revision = 0, ownerId = 0;
		size_t trackIndex = 0;
		std::string clipName, original, candidate;
	};
	AnimationRotationModeDraft rotationModeDraft_;
	const SceneAnimationTrack* ResolveAnimationRotationModeDraft(const SceneDocument& document) const;
	bool ApplyAnimationRotationModeDraft(SceneDocument& document, bool prefab) const;
	void CancelAnimationRotationModeDraft();
	void UpdateAnimationRotationModeDraft();
	bool CanSaveAnimationRotationMode(bool prefab);
	bool DrawAnimationRotationMode(SceneDocument& document, uint64_t ownerId,
		ScenePrefabAnimationClip& clip, size_t trackIndex, bool prefab);
	float SceneSpatialPreviewKeyTime(uint64_t targetId, float time) const;
	SpatialPointAddress spatialPoint_;
	SpatialEditDraft spatialDraft_;
	// 正本Revisionを進めず候補のcacheだけ更新する。公開packetには渡さない。
	uint64_t spatialDraftRevision_ = 0, spatialSampledDraftRevision_ = 0;
	// Escape後、同じImGuizmo dragの残りを新しい編集として取り込まない。
	bool spatialCancelledGesture_ = false;
	// CPU copyはFinalizeで解放。各sample前にTrack対象のTransform/Activeを基準へ戻す。
	SceneDocument* spatialBaseDocument_ = nullptr;
	SceneDocument* spatialSampleDocument_ = nullptr;
	std::vector<std::pair<uint64_t, QuaternionTransform>> spatialBasePoses_;
	std::vector<std::pair<uint64_t, bool>> spatialBaseActive_;
	std::vector<uint64_t> spatialMeshIds_;
	SceneSpatialPreviewFrame spatialStaticFrame_, spatialFrame_;
	uint64_t spatialFrameSerial_ = 0, spatialRenderSerial_ = 0;
	size_t spatialPreparedCount_ = 0, spatialSubmittedCount_ = 0;
	uint64_t spatialOwnerId_ = 0, spatialGeneration_ = 0, spatialRevision_ = 0;
	// 未Bindingの候補再判定は内容Revisionごと。表示OFFや明示Bindingを上書きしない。
	uint64_t spatialCandidateRevision_ = 0;
	bool spatialCandidatesChecked_ = false;
	int spatialClipIndex_ = -1, spatialTrackIndex_ = -1, spatialKeyIndex_ = -1;
	std::string spatialClipName_, spatialIssue_, spatialRenderIssue_, spatialStructure_;
	float spatialTime_ = 0.0f;
	// 表示設定はEditor内で保持し、Documentの世代や選択のリセットとは分離する。
	bool spatialEnabled_ = true, spatialGhosts_ = true, spatialPath_ = true;
	bool spatialPlaying_ = false, spatialCacheDirty_ = true;
	bool spatialOwnEdit_ = false, spatialDragging_ = false;
	Matrix4x4 spatialDragParent_ = MakeIdentity4x4();
	uint64_t spatialDragRevision_ = 0;
	float spatialDragKeyTime_ = 0.0f;
	// Entity用operationとは分離。drag中は解決したkeyの識別を固定する。
	int spatialOperation_ = 0, spatialDragOperation_ = 0;
	int spatialDragPositionTrack_ = -1, spatialDragPositionKey_ = -1;
	int spatialDragTrack_ = -1, spatialDragKey_ = -1;
	uint64_t spatialDragTarget_ = 0;
	std::string spatialEditIssue_;
	bool spatialToolbarHovered_ = false;
	struct PrefabAssetValidationResult {
		std::string filePath;
		std::string message;
		bool error = true;
	};
	struct ComponentPickerItem {
		std::string type;
		std::string requiredByType;
	};
	enum class ComponentTagMatchMode {
		All,
		Any
	};
	struct ComponentPickerState {
		const SceneDocument* document = nullptr;
		std::string documentKey;
		uint64_t entityId = 0;
		char searchBuffer[256] = {};
		int category = -1;
		uint16_t selectedTagMask = 0;
		ComponentTagMatchMode tagMatchMode = ComponentTagMatchMode::All;
		bool favoritesOnly = false;
		std::vector<ComponentPickerItem> items;
		std::string error;
	};
	enum class ComponentPickerTarget {
		Scene,
		Prefab
	};
	enum class ComponentInspectorMode {
		Detailed,
		Simple
	};

	// ドッキング用の土台を作成
	void CreateDockSpace();
	void BuildDefaultLayout();
	void DrawHierarchyWindow(const char* sceneName);
	void DrawInspectorWindow();
	// 要求は値で保持し、各Inspectorの構造操作が終わってから既存Undo枠へ適用する。
	PlayerCombatEditor::DocumentToken MakePlayerCombatToken(const SceneDocument& document, bool prefab) const;
	void DrawPlayerCombatSection(const SceneDocument& document, uint64_t entityId, bool prefab);
	void DrawPlayerCombatFields(const SceneDocument& document, uint64_t entityId,
		const SceneComponent& component, bool prefab, const std::string* attackName = nullptr);
	void QueuePlayerCombatRequest(PlayerCombatEditor::Request request, bool prefab);
	void ApplyPlayerCombatRequests(SceneDocument& document, bool prefab);
	void DrawPlayerCombatDialogs();
	void WarnPlayerCombat(const PlayerCombatEditor::Request& request, const PlayerCombatEditor::Issue& issue);
	struct PlayerCombatContext {
		PlayerCombatEditor::Request mutation, navigation;
		int requestFrame = -1;
		bool reveal = false;
		std::unordered_map<std::string, PlayerCombatEditor::Selection> selections;
		PlayerCombatEditor::DocumentToken returnSource;
		uint64_t returnActor = 0, returnFromEntity = 0;
		std::string returnDestination, returnActorName;
		bool returnOpenPending = false;
		uint64_t returnSerial = 0;
	};
	PlayerCombatContext playerCombatContexts_[2];
	uint64_t playerCombatSerial_ = 0, playerCombatWarningSerial_ = 0;
	bool playerCombatWarningRequested_ = false, playerCombatReferenceRequested_ = false;
	PlayerCombatEditor::Issue playerCombatIssue_;
	PlayerCombatEditor::Operation playerCombatWarningOperation_ = PlayerCombatEditor::Operation::None;
	void DrawSceneUIEditorControls(SceneDocument& document);
	void DrawSceneUIEditorWindow();
	void StartSceneUIEditorView(SceneDocument& document, uint64_t entityId);
	void DrawSceneUIEditorBounds(float x, float y, float width, float height);
	SceneUIEditorView sceneUIEditorView_;
	// タブの対象とInspector選択は独立。永続Sceneデータへ保存しない。
	uint64_t sceneUIEditorEpoch_ = 0;
	std::string sceneUIEditorSceneId_, sceneUIEditorScenePath_, sceneUIEditorIssue_;
	bool sceneUIEditorOpen_ = false, sceneUIEditorFocus_ = false;
	bool sceneUIEditorDocked_ = false, sceneUIEditorRenderable_ = false;
	bool drawingUIEditorView_ = false;
	uint32_t sceneUIEditorWidth_ = 1280, sceneUIEditorHeight_ = 720;
	bool sceneUIEditorFixedAspect_ = true; // 通常Sceneとは独立したEditor表示設定。
	uint32_t sceneUIEditorRenderedWidth_ = 0, sceneUIEditorRenderedHeight_ = 0;
	uint32_t sceneDockId_ = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE sceneUIEditorTexture_{};
	SceneUIEditorView sceneUIEditorRenderedView_;
	struct MenuAuthoringRequest {
		uint64_t entityId = 0;
		std::string operation;
		uint64_t targetEntityId = 0;
		std::string uiOperation; // 共通UI配置操作を走査後に適用する。
	};
	// Scene/Prefab共通の設定UI。Entity/Component追加は走査後へ遅延する。
	bool DrawMenuAuthoringComponent(SceneDocument& document, SceneEntity& entity,
		SceneComponent& component, bool prefab, MenuAuthoringRequest& request);
	// unsetLabelは未指定値の表示だけを置き換え、参照条件と保存値0の意味は維持する。
	bool DrawMenuEntityField(const char* label, SceneDocument& document, uint64_t& entityId,
		const char* requiredType, uint64_t rootId = 0, bool includeRoot = true,
		const std::vector<uint64_t>* allowedIds = nullptr, const char* unsetLabel = nullptr);
	bool DrawUiEventBinding(SceneDocument& document, SceneEventBinding& binding);
	// 呼出し元のbinding ID/Lock/Play/Undo境界を使い、通常/UI・Scene/Prefabでキー一覧を共通編集する。
	bool DrawEventTriggerKeys(SceneEventBinding& binding, const std::vector<std::string>& blockedKeys = {}, bool standard = false);
	// 候補はpreview中だけ所有し、原本の世代/改訂が変わったら適用を拒否する。
	void PrepareMenuInputConversion(SceneDocument& document, uint64_t panelId, bool prefab, const std::string& mode);
	void DrawMenuInputConversion();
	// 前方宣言のまま破棄できる保持型。候補を他の所有者へ共有/公開しない。
	std::shared_ptr<SceneDocument> menuInputCandidate_;
	uint64_t menuInputGeneration_ = 0, menuInputRevision_ = 0;
	bool menuInputPrefab_ = false, menuInputPreviewRequested_ = false, menuInputConversionBlocked_ = false;
	std::vector<std::string> menuInputPreview_;
	std::string eventKeySearch_; // 追加Popupを開くたび初期化し、Documentへ保存しない。
	// 表示文字はMenuItemへ複製せず、参照先TextRendererの基本設定を編集する。
	// 呼出し元がLock/Playを制限し、trueなら同じDocumentをDirtyにする。
	bool DrawMenuTextFields(SceneComponent& text);
	bool DrawUiEventActions(SceneDocument& document, SceneEventBinding& binding);
	bool DrawPrefabUiEvents(SceneDocument& document, SceneComponent& component);
	// 呼出し元のLock/Play制限内で、確認済み要求だけを再解決して適用する。
	// Entity/Component配列を再配置し得るため、適用後はEntityをIDから再解決する。
	void ApplyMenuAuthoringRequest(SceneDocument& document, const MenuAuthoringRequest& request, bool prefab);
	// 候補Documentだけを準備する。失敗時の破棄とUndo境界は呼出し元が担う。
	bool PrepareMenuUILayout(SceneDocument& candidate, uint64_t ownerId, std::string& error);
	// 標準構成は候補Documentだけへ追加。移行/本番データの自動修正には使わない。
	bool PrepareStandardMenuPanel(SceneDocument& candidate, uint64_t ownerId, std::string& error);
	bool PrepareStandardMenuItem(SceneDocument& candidate, uint64_t itemId, bool withText, bool place, std::string& error);
	bool DrawUpgradeSelectionFields(SceneDocument&, SceneEntity&, SceneComponent&, bool prefab, MenuAuthoringRequest&);
	// 候補Documentにだけ構成し、呼出側の既存EditFrameで一回Undoにまとめる。
	bool PrepareUpgradeTemplate(SceneDocument&, uint64_t ownerId);
	Vector2 upgradeRowCenter_{0.5f, 0.35f}, upgradeCardSize_{0.22f, 0.3f};
	Vector2 upgradeConfirmCenter_{0.5f, 0.76f}, upgradeConfirmSize_{0.22f, 0.12f};
	float upgradeCardGap_ = 0.035f;
	bool legacyMenuPanelAddition_ = false;
	std::string LocalizeMenuIssue(const std::string& error) const;
	void DrawMenuAuthoringWarning();
	void DrawProjectAccessWarning();
	// 通常/UI Event共通。原因/対処は選択言語、技術診断は開いた詳細だけへ表示する。
	void DrawTransitionReferenceWarning(const std::string& detail);
	void DrawPresentedTransitionStatus();
	const char* LocalizeTransitionFailure(const std::string& detail) const;
	// 呼出し元がLock/Playを制限する。入力確定時だけ検証し、失敗時は値を戻す。
	bool DrawEnemySpawnAppearance(SceneDocument& document, uint64_t entityId, SceneComponent& component);
	// Scene Inspectorの編集枠内で設定だけを変更する。Wave/Phaseの配列操作は走査後へ遅延する。
	bool DrawGameFlowFields(SceneDocument& document, uint64_t entityId, SceneComponent& component);
	std::string LocalizeGameFlowIssue(const std::string& issue) const;
	std::string LocalizeSceneRestartIssue(const std::string& issue) const;
	std::string LocalizeMenuRuntimeIssue(const std::string& issue) const;
	std::string gameFlowReferenceSearch_;
	uint64_t gameFlowValidationGeneration_ = 0, gameFlowValidationRevision_ = 0, gameFlowValidationEntity_ = 0;
	std::vector<std::string> gameFlowValidationIssues_;
	std::string LocalizeSpawnIssue(const std::string& issue) const;
	void DrawSpawnPreparationWarning();
	// Component配列の描画終了後に追加/Inspector移動を処理し、参照の無効化を避ける。
	void ApplyStatTextAuthoringRequest(SceneDocument& document, uint64_t ownerId, bool prefab, bool addText, uint64_t inspectId, const std::string& inspectType);
	// Component走査後だけ実行する死亡演出の追加・Inspector移動。
	void ApplyDeathAuthoringRequest(SceneDocument& document, uint64_t ownerId, bool prefab, bool addReplay, const std::string& inspect, const std::string& issue, int previewCut, int manualCut, bool endPreview);
	bool spawnWarningRequested_ = false;
	bool spawnWarningRuntime_ = false;
	uint64_t spawnWarningEntityId_ = 0;
	std::string spawnWarningIssue_, spawnWarningDetail_;
	uint64_t spawnDiagnosticInstance_ = 0, spawnDiagnosticGeneration_ = 0, spawnDiagnosticSerial_ = 0;
	std::string DescribeProjectAccessIssue() const;
	bool projectAccessWarningRequested_ = false;
	ProjectAccessIssue projectAccessIssue_{};
	std::string projectAccessDetail_;
	bool menuWarningRequested_ = false;
	std::string menuWarningReason_, menuWarningOperation_, menuWarningTarget_;


	struct UIAuthoringRequest {
		uint64_t entityId = 0;
		std::string operation;
	};
	// Scene/Prefab共通。追加・親付替えはComponent走査後にIDから再解決する。
	bool DrawUILayoutFields(SceneDocument& document, SceneEntity& entity,
		SceneComponent& component, bool prefab, UIAuthoringRequest& request);
	bool DrawUIVisualFields(SceneDocument& document, SceneEntity& entity, SceneComponent& component);
	// 既存clip編集をScene/Prefabで共有する。再生時計やEventはここから進めない。
	bool DrawUITextMotionFields(SceneComponent& component);
	void ApplyUIAuthoringRequest(SceneDocument& document, const UIAuthoringRequest& request, bool prefab);
	// UIの枠と編集は制作座標で行う。仮Canvasは複製にだけ作り、元Documentへ保存しない。
	bool DrawUILayoutGizmo(SceneDocument& document, uint64_t entityId, bool prefab,
		float x, float y, float width, float height);
	void DrawUIMigration(SceneDocument& document, uint64_t entityId, bool prefab);
	void ClearUIMigrationSnapshots();
	bool PickUILayoutEntity(SceneDocument& document, bool prefab, float x, float y, float width, float height);
	// 座標系の変更を伴う除去/親付替えは影響一覧を確認してから適用する。
	bool ConfirmUICoordinateChange(const SceneDocument& document, uint64_t entityId, const char* operation);
	bool QueueUIReparent(const SceneDocument& document, const std::vector<uint64_t>& roots, uint64_t parentId, uint64_t siblingId = 0, bool after = false);
	void DrawUIReparentConfirmation();
	std::vector<uint64_t> uiReparentRoots_;
	uint64_t uiReparentParent_ = 0, uiReparentSibling_ = 0, uiReparentRevision_ = 0;
	bool uiReparentAfter_ = false, uiReparentPopupRequested_ = false;
	std::string uiReparentDocumentKey_;
	std::string uiAuthoringIssue_;
	Vector2 prefabUIReferenceSize_ = { 1280.0f, 720.0f };
	SceneDocument* prefabUILayoutPreviewDocument_ = nullptr;
	// 移行候補はEditor-local snapshot。確認中の原本変更をRevisionとDocument keyで拒否する。
	SceneDocument* uiMigrationOriginal_ = nullptr;
	SceneDocument* uiMigrationCandidate_ = nullptr;
	SceneDocument* uiMigrationPreview_ = nullptr;
	uint64_t uiMigrationRoot_ = 0;
	uint64_t uiMigrationRevision_ = 0;
	std::string uiMigrationDocumentKey_;
	bool uiMigrationPrefab_ = false;
	bool uiMigrationOpen_ = false;
	bool uiMigrationShowAfter_ = false;
	bool uiMigrationBeforeSeen_ = false, uiMigrationAfterSeen_ = false;
	uint64_t uiMigrationPreviewSerial_ = 0;
	uint32_t uiSourceTextureWidth_ = 0, uiSourceTextureHeight_ = 0;
	Vector2 uiMigrationSourceSize_ = {};
	Vector2 uiMigrationTargetSize_ = {1280.0f,720.0f};
	std::string menuReferenceSearch_;
	void StopAudioPreview();
	void DrawSceneComponentPicker();
	void DrawPrefabComponentPicker();
	void DrawComponentPicker(
		ComponentPickerState& state,
		ComponentPickerTarget target
	);
	void OpenComponentPicker(
		ComponentPickerState& state,
		ComponentPickerTarget target,
		SceneDocument& document,
		const std::string& documentKey,
		uint64_t entityId
	);
	void ResetComponentPicker(ComponentPickerState& state);
	void ToggleComponentPickerItem(
		ComponentPickerState& state,
		ComponentPickerTarget target,
		const SceneEntity& entity,
		const std::string& type
	);
	void RemoveComponentPickerItem(
		ComponentPickerState& state,
		const std::string& type
	);
	bool IsComponentPickerItemQueued(
		const ComponentPickerState& state,
		const std::string& type
	) const;
	void DrawComponentSummary(
		const SceneEntity& entity,
		const std::string& documentKey,
		bool prefabAnimationFocusOnly,
		std::string& selectedType
	);
	void DrawProjectWindow();
	void DrawConsoleWindow();
	void DrawLoadedScenesWindow();
	void DrawPrefabWindow();
	void DrawPrefabInspectorWindow();
	void RequestOpenPrefab(const std::string& filePath, int historyIndex = -1);
	bool OpenPrefab(const std::string& filePath, int historyIndex = -1);
	void DrawPrefabOpenConfirmation();
	// 表示状態だけを保持する。Prefab原本/Undo/再生中のDocumentは保存しない。
	struct PrefabWorkspaceState {
		bool windowOpen = false;
		PrefabAssetReference asset;
		uint64_t selectedEntityId = 0;
		float yaw = 0.65f, pitch = 0.25f, zoom = 1.0f;
		bool skeleton = true, jointAxes = false, colliders = true, combatVolumes = true;
		bool clipFocus = true;
		bool operator==(const PrefabWorkspaceState& other) const;
	};
	PrefabWorkspaceState CapturePrefabWorkspace() const;
	void ApplyPrefabWorkspaceView(const PrefabWorkspaceState& state);
	// 初回復元中/失敗直後は保存候補を保持。forceはOpen/Close確定と正常終了時に使う。
	void PersistPrefabWorkspace(bool force = false);
	void RestorePrefabWorkspace();
	void RevealPrefabInspector();
	void ClosePrefabWorkspace();
	void QueuePrefabWorkspaceWarning(bool restoring, const std::string& path, const std::string& detail);
	void DrawPrefabWorkspaceDialogs();
	void SelectPrefabAssetInProject(const std::string& filePath);
	uint64_t InstantiatePrefabInEditScene(
		const std::string& filePath,
		uint64_t parentId,
		const Vector3* rootTranslate = nullptr
	);
	void DrawProjectPrefabAccessPanel();
	void DrawPrefabQuickOpenPopup();
	void RequestPrefabQuickOpen();
	const std::vector<std::string>& GetCachedPrefabAssetPaths();
	const std::vector<std::string>& GetCachedAudioAssetPaths();
	void RefreshPrefabAssetPathCache();
	void RecordRecentPrefab(const std::string& filePath);
	bool IsFavoritePrefab(const std::string& filePath) const;
	void ToggleFavoritePrefab(const std::string& filePath);
	void ToggleFavoritePrefab(const PrefabAssetReference& reference);
	void DrawPrefabPreview();
	void DrawPrefabAnimationTimeline();
	void DrawPrefabTransformPoseInspector(SceneEntity& entity);
	void RebuildPrefabAnimationPreviewDocument();
	void RebuildPlayerCombatPreviewDocument();
	void RebuildPrefabHitBoxGhostDocument();
	const SceneDocument& GetPrefabStageDocument() const;
	void DrawPrefabGizmo(float x, float y, float width, float height);
	bool WritePrefabAnimationGizmoKey(
		uint64_t entityId,
		const std::string& property,
		const Vector3& value
	);
	bool PickPrefabEntity(float x, float y, float width, float height);
	void ValidateAllPrefabAssets();
	void DrawPrefabDiagnostics();
	void DrawPrefabHierarchy();
	void DrawPrefabInspector();
	void DrawWorldAxisIndicator(
		const ImVec2& imageMin,
		const ImVec2& imageMax,
		const Matrix4x4& viewMatrix
	) const;
	void DrawSceneDebugLabels(
		const ImVec2& imageMin,
		const ImVec2& imageMax,
		const Matrix4x4& viewProjectionMatrix
	) const;
	void DrawPlaybackControls();
	void HandleEditShortcuts();
	void DrawSceneGizmo(
		float x,
		float y,
		float width,
		float height,
		uint32_t textureWidth,
		uint32_t textureHeight
	);
	bool PickSceneEntity(
		float x,
		float y,
		float width,
		float height
	);
	void FocusSceneCameraOnSelection();
	const std::vector<std::string>& GetCachedModelAssetPaths();
	const std::vector<std::string>& GetCachedTextureAssetPaths();
	bool DrawAudioClipAssetField(const char* label, std::string& audioClipPath);
	void RefreshAssetPathCache();
	void InvalidateProjectCache();

	// Helper for project tree
	struct ProjectDirectoryEntry {
		std::string fileName;
		std::string filePath;
		std::string extension;
		bool isDirectory = false;
		bool isTexture = false;
		bool isModel = false;
		bool isScene = false;
	};
	struct ProjectDirectoryNode {
		std::string folderName;
		std::string folderPath;
		std::vector<ProjectDirectoryNode> children;
	};
	void DrawDirectoryTreeNode(const ProjectDirectoryNode& node);
	ProjectDirectoryNode BuildProjectDirectoryNode(const std::filesystem::path& path);
	void RefreshProjectTreeCache();
	const std::vector<ProjectDirectoryEntry>& GetCachedProjectDirectoryEntries();
	void RefreshProjectDirectoryCache();
	// エディタ固有の外観設定を管理する。シーンデータには保存しない。
	enum class EditorFontPreset {
		MsGothic,
		YuGothicUi,
		Meiryo,
		BizUdGothic,
		ImGuiDefaultWithJapanese,
		CascadiaMonoWithJapanese
	};
	void DrawSettingsMenu();
	void DrawSceneMenu();
	void DrawSceneSwitchConfirmation();
	void DrawSceneAssetDialogs();
	void DrawProjectSettingsDialogs();
	void DrawProjectLauncherWindow();
	void QueueProjectLauncherRequest(const ProjectLauncherRequest& request);
	void RequestOpenScene(const std::string& sceneId);
	void QueueSceneAssetRequest(const SceneAssetRequest& request);
	void QueueSceneInstanceRequest(const SceneInstanceRequest& request);
	void LoadEditorSettings();
	void SaveEditorSettings() const;
	bool DrawPersistentInspectorHeader(
		const std::string& key,
		const char* label,
		bool defaultOpen = true
	);
	void RequestEditorFontRebuild();
	void ApplyPendingEditorFont();
	void ConfigureEditorFont(ImGuiIO& io, float dpiScale);

	WinApp* winApp_ = nullptr;
	DirectXCommon* dxCommon_ = nullptr;
	SrvManager* srvManager_ = nullptr;
	uint32_t sceneViewWidth_ = 1;
	uint32_t sceneViewHeight_ = 1;
	bool sceneViewFixedAspect_ = true; // Editor設定だけに保存する16:9表示。
	bool sceneViewHasImage_ = false; // 初期の仮矩形でcursor captureを開始しない。
	MainDockGameViewState mainDockGameViewState_ = MainDockGameViewState::Unavailable;
	MainDockGameImageEvidence mainDockEvidence_;
	std::string mainDockTransitionIssueText_;
	// Imageのdrawlist/index範囲は同一FrameのEndFrameまでだけ借用する。
	struct MainDockImageRange {
		const struct ImDrawList* list = nullptr;
		unsigned first = 0, end = 0;
		float left = 0, top = 0, right = 0, bottom = 0;
	};
	MainDockImageRange mainDockImageRange_;
	bool gameInputOwned_ = false;
	bool gameInputFocusRequested_ = false;
	bool editorWasPlaying_ = false;
	void SynchronizeGameInput();
	float sceneViewMinX_ = 0.0f;
	float sceneViewMinY_ = 0.0f;
	float sceneViewMaxX_ = 1.0f;
	float sceneViewMaxY_ = 1.0f;
	bool resetLayout_ = false;
	bool showHierarchy_ = true;
	bool showInspector_ = true;
	bool showProject_ = true;
	bool showConsole_ = true;
	bool showLoadedScenes_ = true;
	bool showPrefab_ = false;
	bool showPrefabInspector_ = true;
	bool prefabInspectorFocusRequested_ = false;
	bool sceneGridVisible_ = true;
	bool prefabGridVisible_ = true;
	bool sceneAxisVisible_ = true;
	bool prefabAxisVisible_ = true;
	EditorLanguage editorLanguage_ = EditorLanguage::Japanese;
	EditorFontPreset editorFontPreset_ = EditorFontPreset::MsGothic;
	float editorFontSize_ = 13.0f;
	bool editorFontRebuildRequested_ = false;
	bool startFullscreen_ = false;
	// シーン/Entity/コンポーネント単位のInspector折りたたみ状態。
	std::unordered_map<std::string, bool> componentFoldoutStates_;
	// Transformなど、Component以外のInspector区画もEditorローカルで保持する。
	std::unordered_map<std::string, bool> inspectorFoldoutStates_;
	std::unordered_set<std::string> favoriteComponentTypes_;
	ComponentInspectorMode componentInspectorMode_ =
		ComponentInspectorMode::Detailed;
	std::string sceneSummarySelectedComponentType_;
	std::string prefabSummarySelectedComponentType_;
	int selectedHierarchyItem_ = 0;
	uint64_t selectedEntityId_ = 0;
	ComponentPickerState sceneComponentPicker_;
	// Hierarchyの複数選択と表示状態を保持する。selectedEntityId_はInspector/Gizmo用の基準Entity。
	std::unordered_set<uint64_t> selectedEntityIds_;
	uint64_t hierarchySelectionAnchorId_ = 0;
	uint64_t hierarchyObservedEntityId_ = 0;
	uint64_t hierarchyRenameEntityId_ = 0;
	char hierarchyRenameBuffer_[128] = {};
	bool hierarchyRenameFocusRequested_ = false;
	bool hierarchyRevealRequested_ = false;
	uint64_t hierarchyDragSourceId_ = 0;
	bool hierarchyDragActive_ = false;
	uint64_t hierarchyAutoOpenFolderId_ = 0;
	double hierarchyAutoOpenStartTime_ = 0.0;
	char hierarchySearchBuffer_[128] = {};
	bool revealInspectorRequested_ = false;
	EditorSession* editorSession_ = nullptr;
	PrefabEditorSession* prefabEditorSession_ = nullptr;
	uint64_t prefabSelectedEntityId_ = 0;
	PrefabWorkspaceState prefabWorkspaceState_;
	bool prefabWorkspaceDirty_ = false;
	bool prefabWorkspaceRestorePending_ = false;
	bool prefabWorkspaceRestoring_ = false;
	bool prefabWorkspaceRestoreFailed_ = false;
	bool prefabInspectorRevealRequested_ = false;
	uint64_t prefabInspectorObservedEntityId_ = 0;
	bool prefabWorkspaceWarningRequested_ = false;
	bool prefabWorkspaceWarningIsRestore_ = false;
	std::string prefabWorkspaceWarningPath_, prefabWorkspaceWarningDetail_;
	bool prefabWorkspaceReferenceRequested_ = false;
	ComponentPickerState prefabComponentPicker_;
	bool prefabClosePopupRequested_ = false;
	bool prefabCloseSaveFailed_ = false;
	bool prefabOpenPopupRequested_ = false;
	int prefabFocusFramesRemaining_ = 0;
	std::string pendingPrefabOpenPath_;
	int pendingPrefabHistoryIndex_ = -1;
	std::vector<PrefabAssetReference> prefabNavigationHistory_;
	int prefabNavigationIndex_ = -1;
	std::string prefabNavigationStatus_;
	SceneCatalog* sceneCatalog_ = nullptr;
	SceneManager* sceneManager_ = nullptr;
	const SceneTemplateRegistry* sceneTemplateRegistry_ = nullptr;
	std::string requestedSceneId_;
	std::string pendingSceneId_;
	bool requestedSceneDiscardUnsavedChanges_ = false;
	bool sceneSwitchPopupRequested_ = false;
	bool sceneSaveFailed_ = false;
	SceneAssetRequest requestedSceneAsset_{};
	bool sceneAssetRequestPending_ = false;
	SceneInstanceRequest requestedSceneInstance_{};
	bool sceneInstanceRequestPending_ = false;
	bool sceneInstanceOperationSucceeded_ = true;
	std::string sceneInstanceStatusMessage_;
	char additiveInstanceKeyBuffer_[64]{};
	bool createScenePopupRequested_ = false;
	bool duplicateScenePopupRequested_ = false;
	bool renameScenePopupRequested_ = false;
	bool deleteScenePopupRequested_ = false;
	bool sceneAssetErrorPopupRequested_ = false;
	std::string sceneAssetTargetId_;
	std::string sceneAssetErrorMessage_;
	std::string requestedStartSceneId_;
	bool startSceneRequestPending_ = false;
	SceneBuildConfiguration requestedStartupConfiguration_{};
	SceneStartupMode requestedStartupMode_{};
	bool startupModeRequestPending_ = false;
	ProjectLauncherView projectLauncherView_{};
	ProjectLauncherRequest projectLauncherRequest_{};
	bool projectLauncherRequestPending_ = false;
	bool showProjectLauncher_ = false;
	bool projectLauncherCreateConfirmationOpen_ = false;
	bool projectLauncherRestoreGenerationConfirmationOpen_ = false;
	std::string projectLauncherRestoreGenerationOperationId_;
	bool projectLauncherGenerationConfirmationOpen_ = false;
	bool projectLauncherGenerationConfirmationVerified_ = false;
	ProjectLauncherRequest projectLauncherGenerationConfirmationRequest_{};
	std::string projectLauncherGenerationConfirmationDetail_;
	uint32_t projectLauncherGenerationConfirmationModifiedOwnedArtifactCount_ = 0;
	bool projectLauncherOpenSolutionAfterCreate_ = false;
	char projectLauncherProjectIdBuffer_[64]{};
	char projectLauncherDisplayNameBuffer_[128]{};
	char projectLauncherDestinationRootBuffer_[512]{};
	char projectLauncherStartSceneIdBuffer_[64]{};
	char projectLauncherImportDescriptorBuffer_[512]{};
	int projectLauncherVisualStudioIndex_ = -1;
	bool projectSettingsErrorPopupRequested_ = false;
	std::string projectSettingsErrorMessage_;
	char sceneAssetNameBuffer_[128]{};
	char sceneAssetIdBuffer_[64]{};
	char sceneAssetFileBuffer_[128]{};
	int sceneTemplateIndex_ = 0;
	int gizmoOperation_ = 0;
	bool gizmoLocalMode_ = true;
	bool gizmoSnapEnabled_ = false;
	float gizmoTranslationSnap_ = 0.5f;
	float gizmoRotationSnapDegrees_ = 15.0f;
	float gizmoScaleSnap_ = 0.1f;
	uint64_t inspectorRotationEntityId_ = 0;
	Vector3 inspectorRotationEuler_{};
	Quaternion inspectorRotationSource_ = { 0.0f, 0.0f, 0.0f, 1.0f };

	static bool sceneViewInputActive_;

	// Singleton instance
	static ImGuiManager* instance;

	// Dynamic asset browser state
	std::string selectedProjectFolder_;
	std::string selectedProjectFile_ = "";
	bool projectFocusRequested_ = false;
	bool projectGridView_ = true;
	bool projectPrefabFilterEnabled_ = false;
	bool prefabQuickOpenFocusRequested_ = false;
	bool prefabQuickOpenPopupRequested_ = false;
	char prefabQuickOpenSearchBuffer_[128] = {};
	float projectThumbnailSize_ = 80.0f;
	std::unordered_set<std::string> projectPreviewLoadAttempted_;
	bool assetPathCacheDirty_ = true;
	std::vector<std::string> cachedModelAssetPaths_;
	std::vector<std::string> cachedTextureAssetPaths_;
	std::vector<std::string> cachedAudioAssetPaths_;
	char audioAssetSearchBuffer_[256] = {};
	bool prefabAssetPathCacheDirty_ = true;
	std::vector<std::string> cachedPrefabAssetPaths_;
	bool prefabAssetValidationCompleted_ = false;
	std::size_t prefabAssetValidationScannedCount_ = 0;
	std::vector<PrefabAssetValidationResult> prefabAssetValidationResults_;
	std::vector<PrefabAssetReference> recentPrefabReferences_;
	std::vector<PrefabAssetReference> favoritePrefabReferences_;
	bool projectDirectoryCacheDirty_ = true;
	std::string cachedProjectFolder_;
	std::vector<ProjectDirectoryEntry> cachedProjectEntries_;
	bool projectTreeCacheDirty_ = true;
	ProjectDirectoryNode cachedProjectTreeRoot_;
	AudioClipPtr previewAudioClip_;
	Audio::PlaybackHandle previewAudioPlayback_;
	std::string previewAudioError_;
	std::string modelPreviewRenderedPath_;
	D3D12_GPU_DESCRIPTOR_HANDLE modelPreviewTexture_{};
	uint32_t modelPreviewWidth_ = 1;
	uint32_t modelPreviewHeight_ = 1;
	float modelPreviewYaw_ = 0.65f;
	float modelPreviewPitch_ = 0.25f;
	float modelPreviewZoom_ = 1.0f;
	std::string prefabPreviewRenderedPath_;
	uint64_t prefabPreviewRenderedRevision_ = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE prefabPreviewTexture_{};
	uint32_t prefabPreviewTextureWidth_ = 1;
	uint32_t prefabPreviewTextureHeight_ = 1;
	Matrix4x4 prefabPreviewViewMatrix_ = MakeIdentity4x4();
	Matrix4x4 prefabPreviewProjectionMatrix_ = MakeIdentity4x4();
	bool prefabPreviewCameraValid_ = false;
	uint32_t prefabPreviewRequestedWidth_ = 768;
	uint32_t prefabPreviewRequestedHeight_ = 432;
	float prefabPreviewYaw_ = 0.65f;
	float prefabPreviewPitch_ = 0.25f;
	float prefabPreviewZoom_ = 1.0f;
	uint64_t prefabPreviewFramingSerial_ = 1;
	// The request producer owns the identity used to accept the asynchronously
	// returned preview texture. Combat Rig uses a composed document, not the
	// currently open prefab document, so DrawPrefabPreview must not reconstruct it.
	std::string prefabPreviewRequestedPath_;
	uint64_t prefabPreviewRequestedRevision_ = 0;
	bool prefabPreviewRequestUsesCombatRig_ = false;
	bool prefabPreviewShowSkeleton_ = true;
	bool prefabPreviewShowJointAxes_ = false;
	bool prefabPreviewShowColliders_ = true;
	bool prefabPreviewShowCombatVolumes_ = true;
	std::string prefabNestedTargetDocumentPath_;
	uint64_t prefabNestedTargetRootId_ = 0;
	SceneDocument* prefabAnimationPreviewDocument_ = nullptr;
	SceneDocument* playerCombatPreviewDocument_ = nullptr;
	SceneDocument* prefabHitBoxGhostDocument_ = nullptr;
	std::string prefabAnimationPreviewAssetPath_;
	uint64_t prefabAnimationPreviewSourceRevision_ = 0;
	uint64_t prefabAnimationPreviewOwnerEntityId_ = 0;
	int prefabAnimationPreviewClipIndex_ = 0;
	bool prefabClipFocusEnabled_ = true;
	bool playerCombatPreviewEnabled_ = false;
	uint64_t playerCombatPreviewRootId_ = 0;
	uint64_t playerCombatPreviewWeaponId_ = 0;
	std::string playerCombatPreviewStatus_;
	float prefabAnimationPreviewTime_ = 0.0f;
	bool prefabAnimationPreviewPlaying_ = false;
	bool prefabAnimationPreviewActive_ = false;
	int prefabAnimationPreviewFrameRate_ = 60;
	bool prefabAnimationPreviewSnapToFrames_ = true;
	float prefabAnimationPreviewFrameAccumulator_ = 0.0f;
	bool prefabHitBoxSetupMode_ = false;
	bool prefabHitBoxGhostVisible_ = false;
	float prefabHitBoxGhostTime_ = 0.0f;
	bool prefabAttackPreviewMode_ = false;
	int prefabAttackPreviewIndex_ = 0;
	float prefabTransformPoseAddTime_ = 0.0f;
	std::string prefabTransformPoseStatus_;
	bool prefabKeyboardFocusThisFrame_ = false;
	// ImGui 1.92以降はフォントデータをAtlasの寿命まで保持する必要がある。
	std::vector<uint8_t> editorBaseFontData_;
	std::vector<uint8_t> editorJapaneseFontData_;
	ImVector<ImWchar> editorGlyphRanges_;

	// Particle loading request
	bool requestLoadParticle_ = false;
	std::string particleToLoad_ = "";
};
