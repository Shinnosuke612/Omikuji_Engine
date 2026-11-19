// 役割: 編集、再生、一時停止の状態とシーン複製を管理する。
#pragma once

#include <string>
#include <vector>

#include "SceneDocument.h"
#include "SceneMenuLayout.h"
#include "SceneExecutionContext.h"
#include "SceneRuntimeDocumentState.h"

enum class EditorPlayState {
	Edit,
	Playing,
	Paused
};

class EditorSession final : public SceneExecutionContext {
public:
	bool Initialize(
		const std::string& sceneId,
		const std::string& sceneName,
		const std::string& sceneFilePath
	);
	bool OpenEditScene(
		const std::string& sceneId,
		const std::string& sceneName,
		const std::string& sceneFilePath,
		bool discardUnsavedChanges = false
	);
	bool LoadRuntimeScene(
		const std::string& sceneId,
		const std::string& sceneFilePath
	) override;
	void ExchangePreparedRuntime(std::unique_ptr<SceneRuntimeDocumentState>& state) noexcept override;

	void Play();
	void Pause();
	void Resume();
	void Stop();
	bool Save();
	void BeginEditFrame();
	void EndEditFrame(bool commit);
	bool Undo();
	bool Redo();
	bool CanUndo() const { return !undoStack_.empty(); }
	bool CanRedo() const { return !redoStack_.empty(); }

	EditorPlayState GetState() const { return state_; }
	bool IsPlaying() const override { return state_ == EditorPlayState::Playing; }
	bool IsPaused() const override { return state_ == EditorPlayState::Paused; }
	bool IsEditing() const override { return state_ == EditorPlayState::Edit; }

	// 明示Open/Reloadの識別。Undo/Redoや候補Documentの反映では変えない。
	uint64_t GetEditSceneEpoch() const { return editSceneEpoch_; }
	SceneDocument& GetEditDocument() { return editDocument_; }
	const SceneDocument& GetEditDocument() const { return editDocument_; }
	SceneDocument& GetActiveDocument() override;
	const SceneDocument& GetActiveDocument() const override;
	const std::string& GetEditSceneId() const { return editSceneId_; }
	const std::string& GetRuntimeSceneId() const { return runtime_->sceneId; }
	const std::string& GetActiveSceneId() const override;
	const std::string& GetSceneFilePath() const { return editSceneFilePath_; }
	const std::string& GetActiveSceneFilePath() const override;
	const std::string& GetLastLoadError() const { return lastLoadError_; }
	void RequestSceneReload() { reloadRequested_ = true; }

	bool ConsumeReloadRequest();

private:
	void PushUndoSnapshot(const SceneDocument& snapshot);

	uint64_t editSceneEpoch_ = 0;
	SceneDocument editDocument_;
	std::unique_ptr<SceneRuntimeDocumentState> runtime_ = std::unique_ptr<SceneRuntimeDocumentState>(
		new SceneRuntimeDocumentState{std::make_unique<SceneDocument>(), {}, {}});
	SceneDocument frameStartDocument_;
	SceneMenuLayout::EditorOrderCache menuOrderCache_;
	std::string editSceneId_;
	std::string editSceneFilePath_;
	std::string lastLoadError_;
	EditorPlayState state_ = EditorPlayState::Edit;
	bool reloadRequested_ = false;
	bool editFrameActive_ = false;
	uint64_t frameStartRevision_ = 0;
	std::vector<SceneDocument> undoStack_;
	std::vector<SceneDocument> redoStack_;
};
