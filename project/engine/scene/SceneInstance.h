// 役割: 1回のロードで生成されたScene本体と、そのロード固有IDを所有する。
#pragma once

#include <memory>
#include <string>

#include "SceneEntityReference.h"
#include "SceneRuntimePreparation.h"

class BaseScene;
class SceneDocument;
class SceneManager;

// 同じSceneアセットを複数回ロードしても、SceneInstanceIdで別個に識別する。
class SceneInstance final
{
public:
	~SceneInstance();

	SceneInstance(const SceneInstance&) = delete;
	SceneInstance& operator=(const SceneInstance&) = delete;

	SceneInstanceId GetId() const {
		return id_;
	}
	const std::string& GetSceneId() const {
		return sceneId_;
	}
	const std::string& GetInstanceKey() const {
		return instanceKey_;
	}
	SceneDocument* GetDocument() {
		return document_;
	}
	const SceneDocument* GetDocument() const {
		return document_;
	}
	bool IsPersistent() const {
		return persistent_;
	}

private:
	friend class SceneManager;

	SceneInstance(
		SceneInstanceId id,
		std::string sceneId,
		std::string instanceKey,
		std::unique_ptr<BaseScene> scene
	);

	void Initialize(SceneManager* sceneManager);
	bool BeginPreparedRuntime(SceneManager* sceneManager);
	RuntimePrepareStep StepPreparedRuntime(const SceneRuntimePrepareBudget&, std::string& diagnostic);
	void Finalize();
	void BindDocument(SceneDocument* document);
	void OwnDocument(std::unique_ptr<SceneDocument> document);
	void SetPersistent(bool persistent);
	void DetachDocument();
	BaseScene* GetScene() const {
		return scene_.get();
	}

	SceneInstanceId id_ = kInvalidSceneInstanceId;
	std::string sceneId_;
	std::string instanceKey_;
	std::unique_ptr<BaseScene> scene_;
	std::unique_ptr<SceneDocument> ownedDocument_;
	SceneDocument* document_ = nullptr;
	bool initialized_ = false;
	bool preparing_ = false;
	bool persistent_ = false;
};
