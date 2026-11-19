// 役割: Scene実行に必要なDocumentと実行状態をEditor/Runtime共通で公開する。
#pragma once

#include <memory>
#include <string>

class SceneDocument;
struct SceneRuntimeDocumentState;

class SceneExecutionContext {
public:
	virtual ~SceneExecutionContext() = default;

	virtual SceneDocument& GetActiveDocument() = 0;
	virtual const SceneDocument& GetActiveDocument() const = 0;
	virtual const std::string& GetActiveSceneId() const = 0;
	virtual const std::string& GetActiveSceneFilePath() const = 0;
	virtual bool IsEditing() const = 0;
	virtual bool IsPlaying() const = 0;
	virtual bool IsPaused() const = 0;
	virtual bool LoadRuntimeScene(
		const std::string& sceneId,
		const std::string& sceneFilePath
	) = 0;
	// mainのCommit専用。検証済み所有単位と交換し、旧Documentをcallerへ返す。
	virtual void ExchangePreparedRuntime(std::unique_ptr<SceneRuntimeDocumentState>& state) noexcept = 0;
};
