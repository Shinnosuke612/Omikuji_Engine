// 役割: SceneInstance集合とSceneFactoryを管理し、Active Sceneの遷移を実行する。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SceneInstance.h"
#include "SceneTransitionRequest.h"

class AbstractSceneFactory;
class BaseScene;
class SceneCatalog;
class SceneDocument;
struct SceneEntity;
struct ScenePostProcessSettings;
class SceneExecutionContext;

enum class SceneLoadMode {
	Single,
	Additive
};

// SceneInstanceを所有し、実行ContextのDocumentと同じタイミングで予約Sceneへ切り替える。
class SceneManager
{
public:
	~SceneManager();

	void Update(float deltaTime);
	// 最初の一件を占有。Consume後もFinishまで追加要求を拒否し、Sceneは変更しない。
	bool RequestPresentedTransition(const PresentedTransitionRequest& request);
	// false時outは不変。要求の所有をGameへ渡すが、占有は維持する。
	bool ConsumePresentedTransitionRequest(PresentedTransitionRequest& out);
	// 拒否/取消/完了を処理したGameだけがsafe Updateで占有を解除する。
	void FinishPresentedTransitionRequest();
	void UpdatePaused();
	// Edit Mode/非対応Instanceでは失敗。既存Sceneの状態をResetせず全Instanceを保持する。
	bool BeginTransitionHold();
	// 再生時刻/開始EventはResetせず、次の通常Updateから進行を再開する。
	void EndTransitionHold();
	bool IsTransitionHeld() const { return transitionHeld_; }
	// 保持中だけ予約を確定し、描画同期を行う。共有Particleの時間は進めない。
	void UpdateTransitionHold();

	void Draw();
	void DrawForegroundEffects();
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
	bool TryGetActiveRuntimePostProcessSettings(
		ScenePostProcessSettings& settings,
		uint64_t& generation
	) const;
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
	std::vector<SceneInstanceId> pendingUnloadSceneInstanceIds_;
	SceneInstanceId activeSceneInstanceId_ = kInvalidSceneInstanceId;
	SceneInstanceId nextSceneInstanceId_ = 1;
	AbstractSceneFactory* sceneFactory_ = nullptr;
	SceneCatalog* sceneCatalog_ = nullptr;
	std::string currentSceneName_;
	SceneExecutionContext* executionContext_ = nullptr;
	bool transitionHeld_ = false;
	bool presentedTransitionBusy_ = false;
	std::optional<PresentedTransitionRequest> presentedTransitionRequest_;
	std::optional<SceneLoadOutcome> lastLoadOutcome_;
};

