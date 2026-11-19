// 役割: Contextとの交換でDocumentのアドレスを保つ実行用所有単位を定義する。
#pragma once
#include "SceneDocument.h"
#include <memory>
#include <string>

struct SceneRuntimeDocumentState {
    std::unique_ptr<SceneDocument> document;
    std::string sceneId;
    std::string filePath;
};
