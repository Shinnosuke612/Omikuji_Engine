// 役割: モデルリソースの読み込み、キャッシュ、取得を管理する。
#pragma once
#include "Model.h"
#include "../base/PreparedResourceStatus.h"
#include <map>
#include <string>
#include <memory>
#include <unordered_set>

class Model;
class ModelCommon;
class DirectXCommon;

class ModelManager{
private:
	static ModelManager* instance;

	ModelManager() = default;
	~ModelManager() = default;
	ModelManager(ModelManager&) = delete;
	ModelManager& operator=(ModelManager&) = delete;

public:

	//初期化
	void Initialize(DirectXCommon* dxCommon);

	//シングルトンインスタンスの取得
	static ModelManager* GetInstance();
	//終了
	void Finalize();

	/// <summary>
	/// モデルファイルの読み込み
	/// </summary>
	/// <param name="filePath">モデルファイルのパス</param>
	void LoadModel(const std::string& filePath);
	void ClearFailedModelCache();
	// main専用。既存ModelCommonを渡し、準備処理の所有を呼出元へ返す。
	bool TryBeginPrepared(Model::PreparedModelData&, std::unique_ptr<ModelBuild>&, std::string& diagnostic);
	// Readyから取得したModelだけを公開。Publishedだけがinputを消費する。
	PreparedPublishStatus TryPublishPreparedModel(const std::string& modelKey,
		std::unique_ptr<Model>& input, std::string& diagnostic);

	/// <summary>
	/// モデルの検索
	/// </summary>
	/// <param name="filePath">モデルファイルのパス</param>
	/// <returns>モデル</returns>
	Model* FindModel(const std::string& filePath);

private:
	//モデルデータ
	std::map < std::string, std::unique_ptr<Model> > models;
	// 未対応形式や破損ファイルの毎フレーム再読み込みを防ぐ
	std::unordered_set<std::string> failedModels;
	//モデル共通部
	ModelCommon* modelCommon = nullptr;
};
