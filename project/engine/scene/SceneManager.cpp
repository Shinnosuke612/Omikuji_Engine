// 役割: シーン生成、更新、描画、遷移の実行を実装する。
#include "SceneManager.h"

#include <cassert>
#include <algorithm>
#include <memory>
#include <utility>

#include "AbstractSceneFactory.h"
#include "BaseScene.h"
#include "SceneCatalog.h"
#include "SceneDocument.h"
#include "SceneExecutionContext.h"
#include "../particle/ParticleManager.h"
#include "../utility/Logger.h"

bool SceneManager::RequestPresentedTransition(const PresentedTransitionRequest& request) {
	if (request.uiSource.requestSerial && (request.uiSource.instanceId != request.sourceInstanceId ||
		!IsUiTransitionSourceValid(request.uiSource) || (executionContext_ && !executionContext_->IsPlaying()))) return false;
	if (presentedTransitionBusy_ || pendingSceneInstance_ || transitionHeld_ ||
		!FindSceneInstance(request.sourceInstanceId) || request.destinationSceneId.empty() ||
		request.transitionAssetPath.empty() || !sceneCatalog_ || !sceneCatalog_->Find(request.destinationSceneId)) {
		Logger::Log("Presented SceneTransition rejected (busy or invalid reference), source:"+
			std::to_string(request.sourceInstanceId)+", destination:"+request.destinationSceneId+
			", asset:"+request.transitionAssetPath+"\n");
		return false;
	}
	presentedTransitionRequest_ = request;
	presentedUiTransitionSource_ = request.uiSource;
	presentedTransitionBusy_ = true;
	return true;
}

bool SceneManager::ConsumePresentedTransitionRequest(PresentedTransitionRequest& out) {
	if (!presentedTransitionRequest_) return false;
	out = std::move(*presentedTransitionRequest_);
	presentedTransitionRequest_.reset();
	return true;
}

void SceneManager::FinishPresentedTransitionRequest() {
	presentedUiTransitionSource_ = {};
	presentedTransitionRequest_.reset();
	presentedTransitionBusy_ = false;
}

void SceneManager::ChangeScene(const std::string& sceneId) {
	if (presentedTransitionBusy_) {
		Logger::Log("SceneTransition rejected while a presented transition is occupied\n");
		return;
	}
	LoadScene(sceneId, SceneLoadMode::Single);
}

SceneInstanceId SceneManager::LoadScene(
	const std::string& sceneId,
	SceneLoadMode loadMode,
	const std::string& instanceKey
) {
	assert(sceneFactory_);
	if (!sceneFactory_ || pendingSceneInstance_) {
		return kInvalidSceneInstanceId;
	}

	const SceneDescriptor* descriptor = sceneCatalog_
		? sceneCatalog_->Find(sceneId)
		: nullptr;
	if (loadMode == SceneLoadMode::Additive) {
		if (!descriptor) {
			Logger::Log("Additive Scene is not registered: " + sceneId + "\n");
			return kInvalidSceneInstanceId;
		}
		if (executionContext_ && executionContext_->IsEditing()) {
			Logger::Log("Additive Scene loading is available in Play/Runtime mode only\n");
			return kInvalidSceneInstanceId;
		}
	}
	const std::string& runtimeProfile = descriptor
		? descriptor->runtimeProfile
		: sceneId;
	BaseScene* newScene = sceneFactory_->CreateScene(runtimeProfile);
	if (!newScene) {
		Logger::Log("Scene runtime profile is not registered: " + runtimeProfile + "\n");
		return kInvalidSceneInstanceId;
	}
	if (transitionHeld_ && !newScene->SupportsTransitionHold()) {
		Logger::Log("Scene runtime profile does not support transition hold: " + runtimeProfile + "\n");
		delete newScene;
		return kInvalidSceneInstanceId;
	}

	if (descriptor && executionContext_) {
		if (executionContext_->IsEditing()) {
			if (executionContext_->GetActiveSceneId() != descriptor->id) {
				Logger::Log(
					"Edit SceneDocument must be opened before changing Scene: " +
					descriptor->id + "\n"
				);
				delete newScene;
				return kInvalidSceneInstanceId;
			}
		}
	}

	// Documentはまだ切り替えず、次のUpdate先頭でSceneInstanceと同時に確定する。
	const SceneInstanceId instanceId = nextSceneInstanceId_++;
	if (nextSceneInstanceId_ == kInvalidSceneInstanceId) {
		++nextSceneInstanceId_;
	}
	std::string resolvedInstanceKey = instanceKey.empty()
		? (descriptor ? descriptor->id : sceneId)
		: instanceKey;
	if (const SceneInstance* existing = FindSceneInstanceByKey(
		resolvedInstanceKey
	)) {
		if (!instanceKey.empty()) {
			Logger::Log(
				"Scene instance key is already loaded: " +
				resolvedInstanceKey + "\n"
			);
			delete newScene;
			return kInvalidSceneInstanceId;
		}
		(void)existing;
		resolvedInstanceKey += "#" + std::to_string(instanceId);
		while (FindSceneInstanceByKey(resolvedInstanceKey)) {
			resolvedInstanceKey += "#";
		}
	}
	pendingSceneInstance_.reset(new SceneInstance(
		instanceId,
		descriptor ? descriptor->id : sceneId,
		std::move(resolvedInstanceKey),
		std::unique_ptr<BaseScene>(newScene)
	));
	pendingSceneLoadMode_ = loadMode;
	pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	pendingRestartScene_ = false;
	pendingUiTransitionSource_ = loadMode == SceneLoadMode::Single && presentedTransitionBusy_ ? presentedUiTransitionSource_ : SceneMenuSource{};
	return instanceId;
}

bool SceneManager::CancelPendingSceneLoad(SceneInstanceId instanceId) {
	if (instanceId == kInvalidSceneInstanceId || !pendingSceneInstance_ ||
		pendingSceneInstance_->GetId() != instanceId) {
		return false;
	}
	DiscardPendingScene();
	return true;
}

bool SceneManager::ConsumeLoadOutcome(
	SceneInstanceId instanceId, SceneLoadOutcome& outcome
) {
	if (!lastLoadOutcome_ || lastLoadOutcome_->instanceId != instanceId) {
		return false;
	}
	outcome = std::move(*lastLoadOutcome_);
	lastLoadOutcome_.reset();
	return true;
}

bool SceneManager::UnloadScene(SceneInstanceId instanceId) {
	if (instanceId == kInvalidSceneInstanceId) {
		return false;
	}
	if (
		pendingSceneInstance_ &&
		pendingSceneInstance_->GetId() == instanceId
	) {
		DiscardPendingScene();
		return true;
	}
	if (!FindSceneInstance(instanceId)) {
		return false;
	}
	if (
		std::find(
			pendingUnloadSceneInstanceIds_.begin(),
			pendingUnloadSceneInstanceIds_.end(),
			instanceId
		) == pendingUnloadSceneInstanceIds_.end()
	) {
		pendingUnloadSceneInstanceIds_.push_back(instanceId);
	}
	return true;
}

bool SceneManager::SetActiveScene(SceneInstanceId instanceId) {
	const SceneInstance* instance = FindSceneInstance(instanceId);
	if (!instance) {
		return false;
	}
	if (activeSceneInstanceId_ != instanceId) uiTransitionOutcome_.reset();
	activeSceneInstanceId_ = instanceId;
	currentSceneName_ = instance->GetSceneId();
	return true;
}

bool SceneManager::SetScenePersistent(
	SceneInstanceId instanceId,
	bool persistent
) {
	if (
		persistent &&
		executionContext_ &&
		executionContext_->IsEditing()
	) {
		Logger::Log("Persistent Scene is available in Play/Runtime mode only\n");
		return false;
	}
	if (
		pendingSceneInstance_ &&
		pendingSceneInstance_->GetId() == instanceId
	) {
		pendingSceneInstance_->SetPersistent(persistent);
		return true;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() == instanceId) {
			instance->SetPersistent(persistent);
			return true;
		}
	}
	return false;
}

void SceneManager::ReloadCurrentScene() {
	// Reload/Editor Stopは演出の取消境界。旧保持を新しい予約へ持ち越さない。
	EndTransitionHold();
	if (pendingSceneInstance_) {
		if (currentSceneName_.empty()) {
			return;
		}
		DiscardPendingScene();
	}
	const std::string targetSceneId =
		executionContext_ && executionContext_->IsEditing()
			? executionContext_->GetActiveSceneId()
			: currentSceneName_;
	if (targetSceneId.empty()) {
		return;
	}

	if (
		LoadScene(targetSceneId, SceneLoadMode::Single) !=
		kInvalidSceneInstanceId
	) {
		// ReloadだけはPersistent指定に関係なく現在Instanceを置き換える。
		pendingReloadSceneInstanceId_ = activeSceneInstanceId_;
		// Play終了後のEdit ModeにはRuntimeで追加したPersistent Instanceを残さない。
		pendingForceUnloadAllSceneInstances_ =
			executionContext_ && executionContext_->IsEditing();
	}
}

SceneInstanceId SceneManager::RequestSceneRestart(SceneInstanceId sourceInstanceId) {
	if (sourceInstanceId == kInvalidSceneInstanceId || sourceInstanceId != activeSceneInstanceId_ ||
		pendingSceneInstance_ || presentedTransitionBusy_ || transitionHeld_ ||
		(executionContext_ && !executionContext_->IsPlaying()) || !sceneCatalog_ ||
		!sceneCatalog_->Find(currentSceneName_)) return kInvalidSceneInstanceId;
	const SceneInstanceId id = LoadScene(currentSceneName_, SceneLoadMode::Single);
	if (id != kInvalidSceneInstanceId) {
		pendingRestartScene_ = true;
		pendingReloadSceneInstanceId_ = sourceInstanceId;
	}
	return id;
}

bool SceneManager::IsUiTransitionSourceValid(const SceneMenuSource& source) const {
	const auto* document = GetActiveSceneDocument();
	return source.instanceId && source.instanceId == activeSceneInstanceId_ && document &&
		source.generation == document->GetContentGeneration() && (!executionContext_ || !executionContext_->IsEditing());
}
SceneInstanceId SceneManager::RequestUiSceneTransition(const SceneMenuSource& source, const std::string& destination) {
	if (!source.requestSerial || !IsUiTransitionSourceValid(source) || (executionContext_ && !executionContext_->IsPlaying()) ||
		pendingSceneInstance_ || transitionHeld_ || presentedTransitionBusy_ || !sceneCatalog_ || !sceneCatalog_->Find(destination)) return kInvalidSceneInstanceId;
	const auto id = LoadScene(destination, SceneLoadMode::Single);
	if (id != kInvalidSceneInstanceId) pendingUiTransitionSource_ = source;
	return id;
}
void SceneManager::ReportUiTransitionOutcome(const SceneMenuSource& source, SceneUiTransitionStatus status, const std::string& diagnostic) {
	if (source.requestSerial && IsUiTransitionSourceValid(source)) uiTransitionOutcome_ = SceneUiTransitionOutcome{source, status, diagnostic};
}
bool SceneManager::ConsumeUiTransitionOutcome(const SceneMenuSource& source, SceneUiTransitionOutcome& outcome) {
	if (!uiTransitionOutcome_ || !IsUiTransitionSourceValid(source) || uiTransitionOutcome_->source.instanceId != source.instanceId ||
		uiTransitionOutcome_->source.generation != source.generation || uiTransitionOutcome_->source.requestSerial != source.requestSerial) return false;
	outcome = *uiTransitionOutcome_; uiTransitionOutcome_.reset(); return true;
}

SceneDocument* SceneManager::GetActiveSceneDocument() {
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() == activeSceneInstanceId_) {
			if (SceneDocument* document = instance->GetDocument()) {
				return document;
			}
			break;
		}
	}
	return executionContext_ ? &executionContext_->GetActiveDocument() : nullptr;
}

const SceneDocument* SceneManager::GetActiveSceneDocument() const {
	const SceneInstance* instance = GetActiveSceneInstance();
	if (instance && instance->GetDocument()) {
		return instance->GetDocument();
	}
	return executionContext_ ? &executionContext_->GetActiveDocument() : nullptr;
}

const SceneInstance* SceneManager::GetActiveSceneInstance() const {
	return FindSceneInstance(activeSceneInstanceId_);
}

const SceneInstance* SceneManager::FindSceneInstance(SceneInstanceId instanceId) const {
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() == instanceId) {
			return instance.get();
		}
	}
	return nullptr;
}

const SceneInstance* SceneManager::FindSceneInstanceByKey(
	const std::string& instanceKey
) const {
	if (instanceKey.empty()) {
		return nullptr;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetInstanceKey() == instanceKey) {
			return instance.get();
		}
	}
	return nullptr;
}

std::vector<const SceneInstance*> SceneManager::GetLoadedSceneInstances() const {
	std::vector<const SceneInstance*> result;
	result.reserve(sceneInstances_.size());
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		result.push_back(instance.get());
	}
	return result;
}

SceneEntity* SceneManager::ResolveEntity(const SceneEntityHandle& handle) {
	if (!handle.IsValid()) {
		return nullptr;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (instance->GetId() != handle.sceneInstanceId) {
			continue;
		}
		SceneDocument* document = instance->GetDocument();
		return document ? document->FindEntity(handle.entityId) : nullptr;
	}
	return nullptr;
}

const SceneEntity* SceneManager::ResolveEntity(
	const SceneEntityHandle& handle
) const {
	if (!handle.IsValid()) {
		return nullptr;
	}
	const SceneInstance* instance = FindSceneInstance(
		handle.sceneInstanceId
	);
	const SceneDocument* document = instance
		? instance->GetDocument()
		: nullptr;
	return document ? document->FindEntity(handle.entityId) : nullptr;
}

SceneEntityHandle SceneManager::ResolveEntityReference(
	SceneInstanceId sourceInstanceId,
	const SceneEntityReference& reference
) const {
	if (reference.entityId == 0) {
		return {};
	}

	const SceneInstance* targetInstance = nullptr;
	if (reference.sceneId.empty()) {
		if (!reference.instanceKey.empty()) {
			return {};
		}
		targetInstance = FindSceneInstance(sourceInstanceId);
	} else if (!reference.instanceKey.empty()) {
		targetInstance = FindSceneInstanceByKey(reference.instanceKey);
		if (
			targetInstance &&
			targetInstance->GetSceneId() != reference.sceneId
		) {
			targetInstance = nullptr;
		}
	} else {
		for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
			if (instance->GetSceneId() != reference.sceneId) {
				continue;
			}
			if (targetInstance) {
				// 同じSceneアセットが複数ある場合はinstanceKeyが必須。
				return {};
			}
			targetInstance = instance.get();
		}
	}
	if (!targetInstance) {
		return {};
	}

	const SceneEntityHandle handle{
		targetInstance->GetId(),
		reference.entityId
	};
	return ResolveEntity(handle) ? handle : SceneEntityHandle{};
}

BaseScene* SceneManager::GetActiveScene() const {
	const SceneInstance* instance = GetActiveSceneInstance();
	return instance ? instance->GetScene() : nullptr;
}

ScenePreparationDiagnostic SceneManager::GetActivePreparationDiagnostic() const {
	const BaseScene* scene = GetActiveScene();
	return scene ? scene->GetPreparationDiagnostic() : ScenePreparationDiagnostic{};
}

bool SceneManager::TryGetActiveRuntimePostProcessSettings(
	ScenePostProcessSettings& settings,
	uint64_t& generation
) const {
	BaseScene* scene = GetActiveScene();
	return scene && scene->TryGetRuntimePostProcessSettings(settings, generation);
}

void SceneManager::PrepareMenuInput(float deltaTime, bool allowed) {
	const bool running = !executionContext_ || executionContext_->IsPlaying();
	for (const auto& instance : sceneInstances_) if (auto* scene = instance->GetScene())
		scene->PrepareMenuInput(deltaTime, allowed && running && scene == GetActiveScene());
	const auto* active = GetActiveScene();
	const auto control = active ? active->GetUiControlFrame() : SceneUiControlFrame{};
	const auto* document = GetActiveSceneDocument();
	const bool blocked = running && document && control.instanceId == activeSceneInstanceId_ &&
		control.generation == document->GetContentGeneration() && control.blockGameplayInput;
	for (const auto& instance : sceneInstances_) if (auto* scene = instance->GetScene()) scene->SetUiGameplayInputBlocked(blocked);
}

void SceneManager::Update(float deltaTime)
{
	if (transitionHeld_) {
		UpdateTransitionHold();
		return;
	}
	ProcessPendingSceneUnloads();
	ActivatePendingScene();
	PrepareMenuInput(deltaTime, menuInputAllowed_);

	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->Update(deltaTime);
		}
	}
	if (!sceneInstances_.empty()) {
		if (BaseScene* activeScene = GetActiveScene()) {
			ParticleManager::GetInstance()->SetCamera(
				activeScene->GetRenderCamera()
			);
		}
		ParticleManager::GetInstance()->Update();
	}
}

bool SceneManager::BeginTransitionHold() {
	if (executionContext_ && executionContext_->IsEditing()) {
		return false;
	}
	if (sceneInstances_.empty()) {
		return false;
	}
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		const BaseScene* scene = instance->GetScene();
		if (!scene || !scene->SupportsTransitionHold()) {
			return false;
		}
	}
	if (pendingSceneInstance_) {
		const BaseScene* pendingScene = pendingSceneInstance_->GetScene();
		if (!pendingScene || !pendingScene->SupportsTransitionHold()) {
			return false;
		}
	}
	transitionHeld_ = true;
	return true;
}

void SceneManager::EndTransitionHold() {
	transitionHeld_ = false;
}

void SceneManager::UpdateTransitionHold() {
	if (!transitionHeld_ || (executionContext_ && executionContext_->IsPaused())) {
		PrepareMenuInput(0.0f, false);
		return;
	}
	ProcessPendingSceneUnloads();
	ActivatePendingScene();
	PrepareMenuInput(0.0f, false);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->UpdateTransitionHold();
		}
	}
	if (BaseScene* activeScene = GetActiveScene()) {
		ParticleManager::GetInstance()->SetCamera(activeScene->GetRenderCamera());
	}
}

void SceneManager::ActivatePendingScene()
{
	if (!pendingSceneInstance_) {
		return;
	}
	if (pendingUiTransitionSource_.requestSerial && !IsUiTransitionSourceValid(pendingUiTransitionSource_)) {
		DiscardPendingScene("MenuRuntime:InvalidSource"); return;
	}
	// 予約後に実行状態/Activeが変わった場合も、編集用Documentを読み替えない。
	if (pendingRestartScene_ && (transitionHeld_ || presentedTransitionBusy_ ||
		(executionContext_ && !executionContext_->IsPlaying()) ||
		activeSceneInstanceId_ != pendingReloadSceneInstanceId_)) {
		DiscardPendingScene("SceneRestart:Rejected");
		return;
	}
	const SceneDescriptor* descriptor = sceneCatalog_
		? sceneCatalog_->Find(pendingSceneInstance_->GetSceneId())
		: nullptr;
	if ((pendingRestartScene_ || pendingUiTransitionSource_.requestSerial) && !descriptor) {
		DiscardPendingScene(pendingRestartScene_ ? "SceneRestart:InvalidScene" : "MenuRuntime:InvalidScene");
		return;
	}
	// Editor状態が予約後に変わっていても、異なるDocumentとの組み合わせを許可しない。
	if (
		pendingSceneLoadMode_ == SceneLoadMode::Single &&
		descriptor &&
		executionContext_ &&
		executionContext_->IsEditing() &&
		executionContext_->GetActiveSceneId() != descriptor->id
	) {
		Logger::Log(
			"Pending Scene does not match the active Edit SceneDocument: " +
			descriptor->id + "\n"
		);
		DiscardPendingScene(
			"Pending Scene does not match the active Edit SceneDocument: " + descriptor->id
		);
		return;
	}
	// UI遷移/再読込成功後の旧cleanupが、新Documentの同IDを書き換えないよう隔離する。
	SceneInstance* restartingInstance = nullptr;
	std::unique_ptr<SceneDocument> restartOldDocument;
	if ((pendingRestartScene_ || pendingUiTransitionSource_.requestSerial) && executionContext_) {
		const auto sourceId = pendingRestartScene_ ? pendingReloadSceneInstanceId_ : pendingUiTransitionSource_.instanceId;
		for (const auto& instance : sceneInstances_) {
			if (instance->GetId() == sourceId &&
				instance->GetDocument() == &executionContext_->GetActiveDocument()) {
				restartingInstance = instance.get();
				restartOldDocument = instance->GetDocument()->CloneForRuntimeDetach();
				break;
			}
		}
	}
	if (
		pendingSceneLoadMode_ == SceneLoadMode::Single &&
		descriptor &&
		executionContext_ &&
		!executionContext_->IsEditing() &&
		(pendingRestartScene_ || executionContext_->GetActiveSceneId() != descriptor->id) &&
		!executionContext_->LoadRuntimeScene(
			descriptor->id,
			descriptor->filePath
		)
	) {
		Logger::Log(
			"Runtime SceneDocument could not be loaded: " +
			descriptor->filePath + "\n"
		);
		DiscardPendingScene("Runtime SceneDocument could not be loaded: " + descriptor->filePath);
		return;
	}
	if (restartingInstance) restartingInstance->OwnDocument(std::move(restartOldDocument));
	if (pendingSceneLoadMode_ == SceneLoadMode::Additive) {
		if (!descriptor) {
			DiscardPendingScene("Additive Scene is not registered");
			return;
		}
		auto document = std::make_unique<SceneDocument>();
		if (!document->Load(descriptor->filePath)) {
			Logger::Log(
				"Additive SceneDocument could not be loaded: " +
				descriptor->filePath + "\n" + document->GetLastLoadError() + "\n"
			);
			DiscardPendingScene(
				"Additive SceneDocument could not be loaded: " + descriptor->filePath +
				": " + document->GetLastLoadError()
			);
			return;
		}
		document->MarkClean();
		pendingSceneInstance_->OwnDocument(std::move(document));
	} else if (executionContext_) {
		pendingSceneInstance_->BindDocument(
			&executionContext_->GetActiveDocument()
		);
	} else if (descriptor) {
		auto document = std::make_unique<SceneDocument>();
		if (!document->Load(descriptor->filePath)) {
			Logger::Log(
				"SceneDocument could not be loaded: " +
				descriptor->filePath + "\n" + document->GetLastLoadError() + "\n"
			);
			DiscardPendingScene(
				"SceneDocument could not be loaded: " + descriptor->filePath +
				": " + document->GetLastLoadError()
			);
			return;
		}
		document->MarkClean();
		pendingSceneInstance_->OwnDocument(std::move(document));
	}
	if (pendingSceneInstance_->IsPersistent()) {
		// Context所有Documentを次のSingleロードから隔離する。
		pendingSceneInstance_->SetPersistent(true);
	}

	if (
		pendingSceneLoadMode_ == SceneLoadMode::Single &&
		!sceneInstances_.empty()
	) {
		// 新Documentの検証に成功してから、現在Sceneの所有物を破棄する。
		const bool persistentInstancesRemain =
			UnloadNonPersistentSceneInstances(
				pendingReloadSceneInstanceId_,
				pendingForceUnloadAllSceneInstances_,
				true
			);

		if (!persistentInstancesRemain) {
			// Persistent Sceneがなければ、従来どおり共有演出も完全に切り替える。
			ParticleManager::GetInstance()->ClearActiveParticles();
			ParticleManager::GetInstance()->ClearGpuParticles();
		}
	}

	const SceneLoadMode loadMode = pendingSceneLoadMode_;
	const SceneInstanceId instanceId = pendingSceneInstance_->GetId();
	sceneInstances_.push_back(std::move(pendingSceneInstance_));
	if (
		loadMode == SceneLoadMode::Single ||
		activeSceneInstanceId_ == kInvalidSceneInstanceId
	) {
		SetActiveScene(instanceId);
	}

	sceneInstances_.back()->Initialize(this);
	lastLoadOutcome_ = SceneLoadOutcome{ instanceId, SceneLoadStatus::Activated, {} };
	pendingUiTransitionSource_ = {};
	pendingSceneLoadMode_ = SceneLoadMode::Single;
	pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	pendingForceUnloadAllSceneInstances_ = false;
	pendingRestartScene_ = false;
}

void SceneManager::DiscardPendingScene(const std::string& diagnostic)
{
	pendingUiTransitionSource_ = {};
	if (pendingSceneInstance_) {
		lastLoadOutcome_ = SceneLoadOutcome{
			pendingSceneInstance_->GetId(), SceneLoadStatus::Failed, diagnostic
		};
	}
	pendingSceneInstance_.reset();
	pendingSceneLoadMode_ = SceneLoadMode::Single;
	pendingReloadSceneInstanceId_ = kInvalidSceneInstanceId;
	pendingForceUnloadAllSceneInstances_ = false;
	pendingRestartScene_ = false;
}

void SceneManager::ProcessPendingSceneUnloads()
{
	for (SceneInstanceId instanceId : pendingUnloadSceneInstanceIds_) {
		auto instance = std::find_if(
			sceneInstances_.begin(),
			sceneInstances_.end(),
			[instanceId](const std::unique_ptr<SceneInstance>& candidate) {
				return candidate->GetId() == instanceId;
			}
		);
		if (instance == sceneInstances_.end()) {
			continue;
		}
		const std::string sceneId = (*instance)->GetSceneId();
		(*instance)->Finalize();
		sceneInstances_.erase(instance);
		ReleaseSceneParticlesIfUnused(sceneId);
	}
	pendingUnloadSceneInstanceIds_.clear();

	if (!FindSceneInstance(activeSceneInstanceId_)) {
		if (sceneInstances_.empty()) {
			activeSceneInstanceId_ = kInvalidSceneInstanceId;
			currentSceneName_.clear();
		} else {
			SetActiveScene(sceneInstances_.front()->GetId());
		}
	}
}

void SceneManager::UpdatePaused()
{
	PrepareMenuInput(0.0f, false);
	if (transitionHeld_) {
		return;
	}
	// Pause中は遷移予約も進めず、Resume後の通常Updateで反映する。
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->UpdatePaused();
		}
	}
}

const SceneUIEditorView* SceneManager::GetEditorUIView() const {
	const auto* document = GetActiveSceneDocument();
	return executionContext_ && executionContext_->IsEditing() && document && editorUIView_.Matches(*document)
		? &editorUIView_ : nullptr;
}

void SceneManager::DrawEditorUIView(const char* renderSpace, uint32_t width, uint32_t height) {
	const auto* view = GetEditorUIView();
	auto* scene = GetActiveScene();
	if (!view || !scene || !renderSpace) return;
	// この呼出しの間だけ有効。描画packet保持と描画先の選択を分離する。
	struct ViewScope {
		bool& active;
		~ViewScope() { active = false; }
	} scope{drawingEditorUIView_};
	drawingEditorUIView_ = true;
	const std::string space(renderSpace);
	if (space == "Scene2D") {
		if (view->Isolated()) scene->DrawEditorUI(*view, renderSpace);
		else Draw();
	} else if (space == "Foreground") {
		if (!view->Isolated()) DrawForegroundEffects();
	} else if (space == "ScreenOverlay") {
		if (view->Isolated()) scene->DrawEditorUI(*view, renderSpace);
		else DrawScreenOverlay(width, height);
	}
}

void SceneManager::Draw()
{
	BaseScene* activeScene = GetActiveScene();
	if (!activeScene) {
		return;
	}
	Camera* renderCamera = activeScene->GetRenderCamera();
	activeScene->DrawEnvironment(renderCamera);
	auto drawSceneContent = [activeScene, renderCamera](BaseScene* scene) {
		scene->PrepareSceneContent(renderCamera);
		// Prepareが設定したRoot SignatureへActive Lightingを再Bindする。
		activeScene->BindLighting();
		scene->DrawPreparedSceneContent(renderCamera);
	};
	drawSceneContent(activeScene);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			if (scene != activeScene) {
				drawSceneContent(scene);
			}
		}
	}
}

void SceneManager::DrawForegroundEffects()
{
	BaseScene* activeScene = GetActiveScene();
	if (!activeScene) {
		return;
	}
	Camera* renderCamera = activeScene->GetRenderCamera();
	activeScene->DrawForegroundEffectsWithCamera(renderCamera);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			if (scene != activeScene) {
				scene->DrawForegroundEffectsWithCamera(renderCamera);
			}
		}
	}
}

bool SceneManager::HasScreenOverlay() const
{
	if (const auto* activeScene = GetActiveScene(); activeScene && activeScene->HasMenuOverlay()) return true;
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (const BaseScene* scene = instance->GetScene()) {
			if (scene->HasScreenOverlay()) {
				return true;
			}
		}
	}
	return false;
}

void SceneManager::DrawScreenOverlay(uint32_t width, uint32_t height)
{
	BaseScene* activeScene = GetActiveScene();
	if (!activeScene) {
		return;
	}
	activeScene->DrawScreenOverlay(width, height);
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			if (scene != activeScene) {
				scene->DrawScreenOverlay(width, height);
			}
		}
	}
	// Additive HUDの後へActive Menuを重ね、後ろのSceneに覆われないようにする。
	activeScene->DrawMenuOverlay();
}

void SceneManager::DrawShadow()
{
	if (BaseScene* activeScene = GetActiveScene()) {
		std::vector<Object3d*> shadowCasters;
		for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
			if (BaseScene* scene = instance->GetScene()) {
				scene->CollectShadowCasters(shadowCasters);
			}
		}
		// Shadow設定とMapはActive Scene、Casterは全Loaded Sceneから集約する。
		activeScene->RenderShadowCasters(shadowCasters);
	}
}

void SceneManager::SetDeferForegroundEffects(bool defer)
{
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->SetDeferForegroundEffects(defer);
		}
	}
}

void SceneManager::DrawOffscreenViews()
{
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		if (BaseScene* scene = instance->GetScene()) {
			scene->DrawOffscreenViews();
		}
	}
}

bool SceneManager::UnloadNonPersistentSceneInstances(
	SceneInstanceId forcedUnloadInstanceId,
	bool forceUnloadAll,
	bool prepareForSceneTransition
)
{
	std::vector<std::string> unloadedSceneIds;
	for (auto instance = sceneInstances_.begin();
		instance != sceneInstances_.end();) {
		if (
			!forceUnloadAll &&
			(*instance)->IsPersistent() &&
			(*instance)->GetId() != forcedUnloadInstanceId
		) {
			++instance;
			continue;
		}
		const std::string sceneId = (*instance)->GetSceneId();
		if (prepareForSceneTransition) {
			if (BaseScene* scene = (*instance)->GetScene()) {
				scene->PrepareForSceneTransition();
			}
		}
		(*instance)->Finalize();
		instance = sceneInstances_.erase(instance);
		if (
			std::find(
				unloadedSceneIds.begin(),
				unloadedSceneIds.end(),
				sceneId
			) == unloadedSceneIds.end()
		) {
			unloadedSceneIds.push_back(sceneId);
		}
	}
	for (const std::string& sceneId : unloadedSceneIds) {
		ReleaseSceneParticlesIfUnused(sceneId);
	}
	return !sceneInstances_.empty();
}

void SceneManager::ReleaseSceneParticlesIfUnused(const std::string& sceneId)
{
	const bool sameSceneStillLoaded = std::any_of(
		sceneInstances_.begin(),
		sceneInstances_.end(),
		[&sceneId](const std::unique_ptr<SceneInstance>& candidate) {
			return candidate->GetSceneId() == sceneId;
		}
	);
	if (!sameSceneStillLoaded) {
		ParticleManager::GetInstance()->ReleaseSceneParticles(sceneId);
	}
}

void SceneManager::UnloadAllSceneInstances()
{
	pendingUiTransitionSource_ = {}; presentedUiTransitionSource_ = {};
	uiTransitionOutcome_.reset();
	for (const std::unique_ptr<SceneInstance>& instance : sceneInstances_) {
		instance->Finalize();
	}
	sceneInstances_.clear();
	pendingUnloadSceneInstanceIds_.clear();
	activeSceneInstanceId_ = kInvalidSceneInstanceId;
	currentSceneName_.clear();
}

SceneManager::~SceneManager()
{
	UnloadAllSceneInstances();
	DiscardPendingScene();
}
