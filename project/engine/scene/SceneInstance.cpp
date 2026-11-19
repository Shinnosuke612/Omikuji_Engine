// 役割: SceneInstanceが所有するScene本体のライフサイクルを管理する。
#include "SceneInstance.h"

#include <utility>

#include "BaseScene.h"
#include "SceneDocument.h"

SceneInstance::SceneInstance(
	SceneInstanceId id,
	std::string sceneId,
	std::string instanceKey,
	std::unique_ptr<BaseScene> scene
)
	: id_(id),
	  sceneId_(std::move(sceneId)),
	  instanceKey_(std::move(instanceKey)),
	  scene_(std::move(scene)) {
}

SceneInstance::~SceneInstance() {
	Finalize();
}

void SceneInstance::Initialize(SceneManager* sceneManager) {
	if (!scene_ || initialized_ || preparing_) {
		return;
	}

	scene_->SetSceneManager(sceneManager);
	scene_->SetSceneInstance(this);
	scene_->Initialize();
	initialized_ = true;
}

bool SceneInstance::BeginPreparedRuntime(SceneManager* sceneManager) {
	if (!scene_ || !document_ || initialized_ || preparing_) {
		return false;
	}
	scene_->SetSceneManager(sceneManager);
	scene_->SetSceneInstance(this);
	// Beginが途中で失敗しても、派生Sceneに部分資源の回収機会を与える。
	preparing_ = true;
	try {
		if (scene_->BeginPreparedRuntime()) {
			return true;
		}
	} catch (...) {
		Finalize();
		throw;
	}
	Finalize();
	return false;
}

RuntimePrepareStep SceneInstance::StepPreparedRuntime(
	const SceneRuntimePrepareBudget& budget, std::string& diagnostic) {
	if (!preparing_) {
		return initialized_ ? RuntimePrepareStep::Ready : RuntimePrepareStep::Unsupported;
	}
	const auto step = scene_->StepPreparedRuntime(budget, diagnostic);
	// Progress/素材待ちも部分所有のまま。全段Readyまで通常Finalizeへ切り替えない。
	if (step == RuntimePrepareStep::Ready) {
		preparing_ = false;
		initialized_ = true;
	}
	return step;
}

void SceneInstance::Finalize() {
	if (preparing_) {
		preparing_ = false;
		scene_->CancelPreparedRuntime();
		return;
	}
	if (!scene_ || !initialized_) {
		return;
	}

	scene_->Finalize();
	initialized_ = false;
}

void SceneInstance::BindDocument(SceneDocument* document) {
	ownedDocument_.reset();
	document_ = document;
}

void SceneInstance::OwnDocument(std::unique_ptr<SceneDocument> document) {
	ownedDocument_ = std::move(document);
	document_ = ownedDocument_.get();
}

void SceneInstance::SetPersistent(bool persistent) {
	persistent_ = persistent;
	if (persistent_) {
		DetachDocument();
	}
}

void SceneInstance::DetachDocument() {
	if (!document_ || document_ == ownedDocument_.get()) {
		return;
	}
	// 内容差替えではなく同じRuntimeの所有移管なので、演出の内容世代を保つ。
	OwnDocument(document_->CloneForRuntimeDetach());
}
