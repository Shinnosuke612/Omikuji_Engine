// 役割: SceneInstance集合とSceneFactoryを管理し、Active Sceneの遷移を実行する。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "SceneInstance.h"
#include "SceneUIEditorView.h"
#include "SceneTransitionRequest.h"
#include "ScenePreparationDiagnostic.h"
#include "SceneReplayHistory.h"
#include "SceneReplayPresentation.h"

class AbstractSceneFactory;
class BaseScene;
class SceneCatalog;
class SceneDocument;
struct SceneEntity;
struct ScenePostProcessSettings;
struct SceneViewEffectClock;
struct SceneDeathReplayStatus;
struct SceneEnemyPositioningStatus;
class SceneExecutionContext;
struct ScenePrepareToken;
struct ScenePrepareRequest;
struct ScenePrepareStatus;
struct SceneDestinationPresentation;
struct ScenePrepareBudget;
struct SceneTextureTransferKey;
struct SceneTextureUploadLoan;
struct SceneTextureUploadTicket;
struct SceneTextureUploadCompletion;
struct SceneTextureTransferObservation;
enum class ScenePreparePumpResult;

enum class SceneLoadMode {
	Single,
	Additive
};

// SceneInstanceを所有し、実行ContextのDocumentと同じタイミングで予約Sceneへ切り替える。
class SceneManager
{
public:
	SceneManager();
	~SceneManager();

	void Update(float deltaTime);
	// Gameはこの準備後、Audio停止理由を反映してからUpdateする。Update単独でも準備する。
	void PrepareGameUpdate(float deltaTime);
	// 全Instanceが表示専用Pause更新に対応する場合だけ許可。部分停止は行わない。
	bool CanPauseGame() const;
	bool IsGamePaused() const { return gamePaused_; }
	bool ShouldPauseBgm() const { return gamePaused_ && pauseBgm_; }
	// Gameが毎Frame渡す入力許可。Scene側はInput/Editorに依存しない。
	void SetMenuInputAllowed(bool allowed) { menuInputAllowed_ = allowed; }
	// 最初の一件を占有。Consume後もFinishまで追加要求を拒否し、Sceneは変更しない。
	// 現在の実行Sceneからのみ受付。遷移予約と排他、Gameが次Frameで一回消費する。
	bool RequestGameExit(SceneInstanceId sourceInstanceId, uint64_t generation);
	// 消費時もScene世代と実行状態を照合し、無効な要求も破棄する。
	bool ConsumeGameExitRequest();
	bool IsGameExitPending() const { return gameExitSource_ != kInvalidSceneInstanceId; }
	bool RequestPresentedTransition(const PresentedTransitionRequest& request);
	// false時outは不変。要求の所有をGameへ渡すが、占有は維持する。
	bool ConsumePresentedTransitionRequest(PresentedTransitionRequest& out);
	// 拒否/取消/完了を処理したGameだけがsafe Updateで占有を解除する。
	void FinishPresentedTransitionRequest();

	// main限定。準備中は現Sceneを保持し、表示証跡に対応する候補だけをCommitする。
	bool SetScenePreparationProjectEpoch(uint64_t epoch);
	bool BeginScenePreparation(const ScenePrepareRequest&, ScenePrepareToken& out);
	bool PollScenePreparation(ScenePrepareToken, ScenePrepareStatus& out) const;
	bool BeginPresentDestination(ScenePrepareToken);
	bool CommitScenePreparation(ScenePrepareToken, const SceneDestinationPresentation&);
	void CancelScenePreparation(ScenePrepareToken) noexcept;
	void NotifyDestinationFrameRetired(ScenePrepareToken, uint64_t recordingSerial) noexcept;
	SceneInstanceId GetPreparedDestinationRenderInstance() const noexcept;
	// 描画対象のPostProcessを取得。候補取消後は旧Sceneへ戻り、Active照会を変えない。
	bool TryGetRenderRuntimePostProcessSettings(ScenePostProcessSettings&, uint64_t& generation,
		SceneInstanceId& instanceId) const;

	// Gameの同一Update内で借用→受付→Commit。Busyなら借用元が所有を続ける。
	ScenePreparePumpResult PumpScenePreparation(const ScenePrepareBudget&);
	bool TryBorrowSceneTextureUpload(SceneTextureUploadLoan& out) noexcept;
	// Aの全COPY完了とDIRECT移管をGameが確認したUpdate内だけ貸す。
	bool TryBorrowMaterialCopyLookahead(const SceneTextureUploadTicket& head, SceneTextureUploadLoan& out) noexcept;
	void CommitMaterialCopyLookahead(const SceneTextureUploadTicket& ticket, uint64_t decodedBytes) noexcept;
	// Gameが未提出またはCOPY回収済みと確認してからBの予約を返す。
	void RetireMaterialCopyLookahead(const SceneTextureUploadTicket&) noexcept;
	// 通常依存処理がBに到達した後だけ、GameがDIRECTへ渡せる。
	bool IsSceneTextureUploadHead(const SceneTextureUploadTicket&) const noexcept;
	// TEMP: mainの診断専用。既存状態を観測するだけで、素材/搬送/Sceneの進行を変えない。
	void ObserveSceneTextureTransfer(const SceneTextureTransferObservation&) noexcept;
	void CommitSceneTextureUpload(const SceneTextureUploadTicket&) noexcept;
	void RejectSceneTextureUpload(const SceneTextureTransferKey&, std::string_view reason) noexcept;
	bool ShouldCancelSceneTextureUpload(const SceneTextureUploadTicket&) const noexcept;
	void AcceptSceneTextureUpload(SceneTextureUploadCompletion&&) noexcept;
	void RetireSceneTextureUploadForShutdown(const SceneTextureUploadTicket&) noexcept;
	void UpdatePaused();
	// Edit Mode/非対応Instanceでは失敗。既存Sceneの状態をResetせず全Instanceを保持する。
	bool BeginTransitionHold();
	// 再生時刻/開始EventはResetせず、次の通常Updateから進行を再開する。
	void EndTransitionHold();
	bool IsTransitionHeld() const { return transitionHeld_; }
	// 保持中だけ予約を確定し、描画同期を行う。共有Particleの時間は進めない。
	void UpdateTransitionHold();

	// Editor Edit状態かつActive Document一致時だけ有効。Monitorには渡さない。
	void SetEditorUIView(const SceneUIEditorView& view) { editorUIView_ = view; }
	const SceneUIEditorView* GetEditorUIView() const;
	// UI編集RTをBindしたcallerだけが呼ぶ。通常Draw/Monitorへフィルターを持ち越さない。
	void DrawEditorUIView(const char* renderSpace, uint32_t width = 0, uint32_t height = 0);
	bool IsDrawingEditorUIView() const { return drawingEditorUIView_; }
	// Editor構図確認のCameraはこのDraw中だけ借用。通常呼出しの選択Cameraは維持。
	// Scene描画を記録する前に、同フレームのserialを全描画対象へ渡す。
	void BeginRenderFrame(uint64_t serial);
	void Draw(Camera* editorCamera = nullptr);
	void DrawForegroundEffects(Camera* editorCamera = nullptr);
	bool HasScreenOverlay() const;
	void DrawScreenOverlay(uint32_t width, uint32_t height);
	void DrawShadow();
	void DrawOffscreenViews();
	void SetDeferForegroundEffects(bool defer);

	// ChangeSceneは予約のみ行い、次のUpdate先頭でDocument読込と初期化を確定する。
	void ChangeScene(const std::string& sceneId);
	SceneInstanceId LoadScene(
		const std::string& sceneId,
		SceneLoadMode loadMode,
		const std::string& instanceKey = {}
	);
	// 未確定の予約IDに一致した場合だけ取消。別予約や確定済みSceneを破棄しない。
	bool CancelPendingSceneLoad(SceneInstanceId instanceId);
	// 最後の一件を、ID一致時だけ一回消費する。LoadSceneの即時失敗は無効IDで返る。
	bool ConsumeLoadOutcome(SceneInstanceId instanceId, SceneLoadOutcome& outcome);
	SceneInstanceId LoadSceneAdditive(
		const std::string& sceneId,
		const std::string& instanceKey = {}
	) {
		return LoadScene(sceneId, SceneLoadMode::Additive, instanceKey);
	}
	// Unloadは安全のため予約し、次のUpdate先頭で実行する。
	bool UnloadScene(SceneInstanceId instanceId);
	bool SetActiveScene(SceneInstanceId instanceId);
	// Persistent InstanceはSingleロードでも破棄しない。明示Unloadは可能。
	bool SetScenePersistent(SceneInstanceId instanceId, bool persistent);
	void ReloadCurrentScene();
	// Active Instanceの保存済みDocumentも次Updateで再読込する。成功予約ID、拒否は無効ID。
	// Edit/Pause/遷移中/別予約中は拒否。読込失敗時は現在Sceneを維持してLoadOutcomeへ返す。
	SceneInstanceId RequestSceneRestart(SceneInstanceId sourceInstanceId);
	// requestSerial付きUI遷移は受理した予約IDを返す。拒否時は現Scene/選択を保持する。
	// Panelの生存は発行側、Instance/内容世代はManagerが再確認する。
	SceneInstanceId RequestUiSceneTransition(const SceneMenuSource& source, const std::string& destination);
	bool IsUiTransitionSourceValid(const SceneMenuSource& source) const;
	// 演出の実際の受理/拒否はGameが報告。結果は同じ要求が一回だけ消費する。
	void ReportUiTransitionOutcome(const SceneMenuSource& source, SceneUiTransitionStatus status, const std::string& diagnostic = {});
	bool ConsumeUiTransitionOutcome(const SceneMenuSource& source, SceneUiTransitionOutcome& outcome);
	void SetSceneFactory(AbstractSceneFactory* sceneFactory) {
		sceneFactory_ = sceneFactory;
	}
	void SetSceneCatalog(SceneCatalog* sceneCatalog) {
		sceneCatalog_ = sceneCatalog;
	}
	const std::string& GetCurrentSceneName() const {
		return currentSceneName_;
	}
	const std::string& GetCurrentSceneId() const {
		return currentSceneName_;
	}
	void SetExecutionContext(SceneExecutionContext* executionContext) {
		executionContext_ = executionContext;
	}
	SceneExecutionContext* GetExecutionContext() const {
		return executionContext_;
	}
	SceneDocument* GetActiveSceneDocument();
	const SceneDocument* GetActiveSceneDocument() const;
	// Active InstanceのDocumentが一致するときだけ実行値をコピーする。未対応/未検出時はfalse、outは不変。
	// Scene本体やRuntime状態の所有権は公開せず、問い合わせ中だけ参照する。
	// maxValue指定時は実行中の上限も返す。失敗時は出力を変更しない。
	bool TryGetRuntimeStat(const SceneDocument& document, uint64_t entityId,
		const std::string& statId, float& out, float* maxValue = nullptr) const;
	// Active Document一致時だけ値コピー。Scene本体を公開せず、失敗時out不変。
	bool TryGetEnemyPositioningStatus(const SceneDocument& document, uint64_t entityId, SceneEnemyPositioningStatus& out) const;
	// Active InstanceのDocument一致時だけ受付状態を返す。不一致/未対応は-1。
	int GetDeathResponseStatus(const SceneDocument& document, uint64_t id) const;
	// Active Document一致/成功時だけoutを更新する。
	// Active instance/document一致を照合。Editor Editでは採取しない。query失敗時はout不変。
    // Active Scene限定のReplay表示。通常/Monitor/UI描画経路とは分離する。
    bool TryGetReplayFraming(uint64_t owner, SceneReplayFramingBasis& out) const;
    void AdvanceReplayPresentation(float delta, bool allowed);
    bool PrepareReplayGameFrame(uint64_t drawSerial, float aspect, SceneReplayGameFrame& out);
    bool DrawReplayGameShadow(const SceneReplayPresentationToken& token);
    bool DrawReplayGameWorld(const SceneReplayPresentationToken& token);
    void RejectReplayGameFrame(const SceneReplayPresentationToken& token);
    void AcceptReplayPresentationReceipt(const SceneReplayPresentationToken& token);
    // Active Sceneの完了画面だけを合成し、その表示完了を中継する。
    bool DrawReplayMenu(const SceneReplayPresentationToken&, SceneMenuSource&);
    void AcceptReplayMenuReceipt(const SceneReplayPresentationToken&, const SceneMenuSource&);
	void NotifyReplayGpuCompleted();
	void CaptureReplayBeforeDraw(const SceneReplayCaptureRequest& request);
	bool TryGetReplayHistoryStatus(const SceneDocument& document, uint64_t id, SceneReplayHistoryStatus& out) const;
	bool TryGetDeathReplayStatus(const SceneDocument& document, uint64_t id, SceneDeathReplayStatus& out) const;
	bool TryGetRuntimeFlag(const SceneDocument& document, uint64_t entityId,
		const std::string& flagId, bool& out) const;
	// Active Sceneの準備診断を値で返す。Sceneなしは空診断で、Scene本体や所有権は公開しない。
	ScenePreparationDiagnostic GetActivePreparationDiagnostic() const;
	bool TryGetActiveRuntimePostProcessSettings(
		ScenePostProcessSettings& settings,
		uint64_t& generation
	) const;
	// Active Sceneの成功時だけ値とinstanceをコピーする。失敗時は両outとも不変。
	bool TryGetActiveViewEffectClock(SceneViewEffectClock& out, SceneInstanceId& instanceId) const;
	SceneInstanceId GetActiveSceneInstanceId() const {
		return activeSceneInstanceId_;
	}
	const SceneInstance* GetActiveSceneInstance() const;
	const SceneInstance* FindSceneInstance(SceneInstanceId instanceId) const;
	const SceneInstance* FindSceneInstanceByKey(
		const std::string& instanceKey
	) const;
	std::vector<const SceneInstance*> GetLoadedSceneInstances() const;
	SceneEntity* ResolveEntity(const SceneEntityHandle& handle);
	const SceneEntity* ResolveEntity(const SceneEntityHandle& handle) const;
	SceneEntityHandle ResolveEntityReference(
		SceneInstanceId sourceInstanceId,
		const SceneEntityReference& reference
	) const;

private:
	struct PreparationTransaction;
	std::unique_ptr<PreparationTransaction> preparation_;
	uint64_t preparationEpoch_ = 0;
	uint64_t nextPreparationGeneration_ = 0;
	bool IsPreparationSourceLive() const noexcept;
	BaseScene* GetPreparedRenderScene() const noexcept;
	void FailPreparation(std::string_view reason) noexcept;
	struct DocumentReadJob;
	// main限定。要求識別は将来の準備要求口が発行し、workerは採番しない。
	bool TryStartDocumentRead(SceneInstanceId instanceId, uint64_t generation, uint64_t projectEpoch,
		const std::string& sceneId, const std::string& resolvedPath);
	void PollDocumentRead();
	void CancelDocumentRead() noexcept;
	// callerの現要求と一致する終端だけ一回move。失敗時outは不変。
	bool TryTakeDocumentRead(SceneInstanceId instanceId, uint64_t generation, uint64_t projectEpoch,
		std::unique_ptr<SceneDocument>& out, bool& failed);
	std::unique_ptr<DocumentReadJob> documentReadJob_;
	void PrepareMenuInput(float deltaTime, bool allowed);
	bool menuInputAllowed_ = false;
	bool gameUpdatePrepared_ = false;
	bool gamePaused_ = false, pauseBgm_ = false;
	SceneUIEditorView editorUIView_;
	bool drawingEditorUIView_ = false;
	void ActivatePendingScene();
	void DiscardPendingScene(
		const std::string& diagnostic = "Scene load cancelled before activation"
	);
	void ProcessPendingSceneUnloads();
	BaseScene* GetActiveScene() const;
	bool UnloadNonPersistentSceneInstances(
		SceneInstanceId forcedUnloadInstanceId = kInvalidSceneInstanceId,
		bool forceUnloadAll = false,
		bool prepareForSceneTransition = false
	);
	void ReleaseSceneParticlesIfUnused(const std::string& sceneId);
	void UnloadAllSceneInstances();

	std::vector<std::unique_ptr<SceneInstance>> sceneInstances_;
	std::unique_ptr<SceneInstance> pendingSceneInstance_;
	SceneLoadMode pendingSceneLoadMode_ = SceneLoadMode::Single;
	SceneInstanceId pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	bool pendingForceUnloadAllSceneInstances_ = false;
	bool pendingRestartScene_ = false;
	// Event/演出の遷移予約だけ保存状態を読み直す。EditorのPlay開始/Reloadとは区別する。
	bool pendingReadSavedScene_ = false;
	SceneMenuSource pendingUiTransitionSource_;
	std::vector<SceneInstanceId> pendingUnloadSceneInstanceIds_;
	// Scene/Playの境界で破棄する非保存要求。Document pointerは保持しない。
	SceneInstanceId gameExitSource_ = kInvalidSceneInstanceId;
	uint64_t gameExitGeneration_ = 0;
	SceneInstanceId activeSceneInstanceId_ = kInvalidSceneInstanceId;
	SceneInstanceId nextSceneInstanceId_ = 1;
	AbstractSceneFactory* sceneFactory_ = nullptr;
	SceneCatalog* sceneCatalog_ = nullptr;
	std::string currentSceneName_;
	SceneExecutionContext* executionContext_ = nullptr;
	bool transitionHeld_ = false;
	bool presentedTransitionBusy_ = false;
	SceneMenuSource presentedUiTransitionSource_;
	std::optional<PresentedTransitionRequest> presentedTransitionRequest_;
	std::optional<SceneLoadOutcome> lastLoadOutcome_;
	std::optional<SceneUiTransitionOutcome> uiTransitionOutcome_;
};

