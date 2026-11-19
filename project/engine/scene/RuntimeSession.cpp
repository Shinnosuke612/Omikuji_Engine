// 役割: Runtime専用のSceneDocument読込と遷移時の差し替えを実装する。
#include "RuntimeSession.h"

#include <utility>

bool RuntimeSession::Initialize(
	const std::string& sceneId,
	const std::string& sceneFilePath
) {
	return LoadRuntimeScene(sceneId, sceneFilePath);
}

bool RuntimeSession::LoadRuntimeScene(
	const std::string& sceneId,
	const std::string& sceneFilePath
) {
	if (sceneId.empty() || sceneFilePath.empty()) {
		return false;
	}

	SceneDocument loadedDocument;
	if (!loadedDocument.Load(sceneFilePath)) {
		lastLoadError_ = loadedDocument.GetLastLoadError();
		return false;
	}
	loadedDocument.MarkClean();
	*runtime_->document = std::move(loadedDocument);
	runtime_->sceneId = sceneId;
	runtime_->filePath = sceneFilePath;
	lastLoadError_.clear();
	return true;
}

void RuntimeSession::ExchangePreparedRuntime(std::unique_ptr<SceneRuntimeDocumentState>& state) noexcept {
	runtime_.swap(state);
	lastLoadError_.clear();
}
