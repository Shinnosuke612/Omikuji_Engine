// 役割: Runtime専用起動で単一の実行用SceneDocumentを所有する。
#pragma once

#include "SceneDocument.h"
#include "SceneExecutionContext.h"
#include "SceneRuntimeDocumentState.h"

class RuntimeSession final : public SceneExecutionContext {
public:
	bool Initialize(
		const std::string& sceneId,
		const std::string& sceneFilePath
	);
	bool LoadRuntimeScene(
		const std::string& sceneId,
		const std::string& sceneFilePath
	) override;

	void ExchangePreparedRuntime(std::unique_ptr<SceneRuntimeDocumentState>& state) noexcept override;
	SceneDocument& GetActiveDocument() override { return *runtime_->document; }
	const SceneDocument& GetActiveDocument() const override { return *runtime_->document; }
	const std::string& GetActiveSceneId() const override { return runtime_->sceneId; }
	const std::string& GetActiveSceneFilePath() const override {
		return runtime_->filePath;
	}
	const std::string& GetLastLoadError() const { return lastLoadError_; }
	bool IsEditing() const override { return false; }
	bool IsPlaying() const override { return true; }
	bool IsPaused() const override { return false; }

private:
	std::unique_ptr<SceneRuntimeDocumentState> runtime_ = std::unique_ptr<SceneRuntimeDocumentState>(
		new SceneRuntimeDocumentState{std::make_unique<SceneDocument>(), {}, {}});
	std::string lastLoadError_;
};
