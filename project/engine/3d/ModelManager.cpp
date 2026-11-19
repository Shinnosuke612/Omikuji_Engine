// 役割: モデルファイルの非重複読み込みとModelキャッシュを実装する。
#include "ModelManager.h"
#include "../base/DirectXCommon.h"
#include "ModelCommon.h"
#include "Model.h"
#include "../utility/EditableResourcePath.h"
#include "../utility/StringUtility.h"
#include <filesystem>
#include <exception>

namespace {

std::string NormalizeModelKey(const std::string& filePath) {
	const std::filesystem::path requestedPath = StringUtility::ToPath(filePath);
	std::filesystem::path normalizedPath = requestedPath;
	if (requestedPath.is_absolute()) {
		normalizedPath = EditableResourcePath::ToProjectRelative(
			EditableResourcePath::ResolveResource(requestedPath)
		);
	}

	std::string key = StringUtility::ToUtf8(normalizedPath.lexically_normal());
	const std::string resourcePrefix = "resources/";
	if (key.rfind(resourcePrefix, 0) == 0) {
		key = key.substr(resourcePrefix.size());
	}
	return key;
}

} // namespace

ModelManager* ModelManager::instance = nullptr;

void ModelManager::Initialize(DirectXCommon* dxCommon){
	modelCommon = new ModelCommon;
	modelCommon->Initialize(dxCommon);
}

ModelManager* ModelManager::GetInstance(){
	if(instance == nullptr){
		instance = new ModelManager;
	}
	return instance;
}

void ModelManager::Finalize(){
	delete instance;
	instance = nullptr;
}

void ModelManager::LoadModel(const std::string& filePath){
	const std::string modelKey = NormalizeModelKey(filePath);
//読み込み済みモデルを検索
	if(models.contains(modelKey) || failedModels.contains(modelKey)){
		//読み込み済みなら早期return
		return;
	}
	//モデルの生成とファイル読み込み、初期化
	const std::filesystem::path modelFilePath =
		EditableResourcePath::ResolveResource(StringUtility::ToPath(modelKey));
	std::unique_ptr<Model> model = std::make_unique<Model>();
	if (!model->Initialize(
		modelCommon,
		modelFilePath
	)) {
		failedModels.insert(modelKey);
		return;
	}

	//モデルをmapコンテナに格納する
	failedModels.erase(modelKey);
	models.insert(std::make_pair(modelKey, std::move(model)));
}

void ModelManager::ClearFailedModelCache() {
	failedModels.clear();
}

bool ModelManager::TryBeginPrepared(Model::PreparedModelData& input,
	std::unique_ptr<ModelBuild>& out, std::string& diagnostic) {
	return Model::TryBeginPrepared(modelCommon, input, out, diagnostic);
}

PreparedPublishStatus ModelManager::TryPublishPreparedModel(const std::string& modelKey,
	std::unique_ptr<Model>& input, std::string& diagnostic) {
	using Status = PreparedPublishStatus;
	diagnostic.clear();
	if (!input || !input->preparedReady_ || !modelCommon || input->modelCommon != modelCommon ||
		modelKey.empty()) {
		diagnostic = "Prepared model or cache host is invalid.";
		return Status::Failed;
	}
	try {
		const auto key = NormalizeModelKey(modelKey);
		if (!key.empty() && key != ".") {
			const auto existing = models.find(key);
			if (existing != models.end() && existing->second) return Status::ReusedExisting;
			// 空nodeを先に確保し、挿入に成功するまでinputを移さない。
			decltype(models) staging;
			staging.try_emplace(key);
			auto inserted = models.insert(staging.extract(staging.begin()));
			if (inserted.inserted) {
				inserted.position->second = std::move(input);
				failedModels.erase(key);
				return Status::Published;
			}
		}
	} catch (const std::exception&) {
		// 正規化やnode確保の失敗時にinputと既存cacheを保持する。
	}
	diagnostic = "Failed to publish the prepared model.";
	return Status::Failed;
}

Model* ModelManager::FindModel(const std::string& filePath){
	const std::string modelKey = NormalizeModelKey(filePath);
	//読み込み済みモデルを検索
	if(models.contains(modelKey)){
		//読み込みモデルを戻り値としてreturn
		return models.at(modelKey).get();
	}

	//ファイル名一致なし
	return nullptr;
}
