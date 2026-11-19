// 役割: Scene準備TransactionとGameの画像搬送を結ぶ値型を定義する。
#pragma once
#include "SceneEntityReference.h"
#include "SceneRuntimePreparation.h"
#include "../2d/TextureManager.h"
#include "../3d/Model.h"
#include <cstdint>
#include <string>

struct ScenePrepareToken {
    SceneInstanceId instanceId = 0;
    uint64_t generation = 0;
};

// TEMP: 診断後撤去。既存host観測を値で渡すだけで、Poll/送信/待機を要求しない。
struct SceneTextureTransferObservation {
    ScenePrepareToken token;
    uint64_t uploadId = 0;
    const char* event = "sample";
    int64_t atUs = 0, deadlineUs = 0;
    uint64_t signaledFence = 0, completedFence = 0, recordingSerial = 0;
    bool canPrepare = false, foreignGpuOwner = false;
    int outcome = -1;
};

enum class ScenePreparePhase {
    ReadingDocument, ResolvingDependencies, PreparingCpu, Uploading,
    ConstructingRuntime, Ready, PresentingDestination,
    AwaitingCommit, RetiringPrevious, Completed, Failed, Cancelling, Cancelled
};

struct ScenePrepareRequest {
    SceneInstanceId sourceInstanceId = 0;
    uint64_t projectEpoch = 0;
    std::string targetSceneId;
};

struct ScenePrepareStatus {
    ScenePrepareToken token;
    uint64_t projectEpoch = 0;
    ScenePreparePhase phase = ScenePreparePhase::ReadingDocument;
    bool commitSucceeded = false;
    std::string diagnostic;
    SceneRuntimePrepareMetrics runtime;
};

// Hostが候補の最終合成・Present・GPU完了を確認した同一提出の値を渡す。
struct SceneDestinationPresentation {
    ScenePrepareToken token;
    uint64_t projectEpoch = 0;
    SceneInstanceId drawnInstanceId = 0;
    uint64_t recordingSerial = 0;
    uint64_t outputGeneration = 0;
    uint64_t signaledFence = 0, completedFence = 0;
};

struct ScenePrepareBudget {
    SceneRuntimePrepareBudget runtime;
    ModelBuildBudget model;
    uint64_t maxTextureStagingBytes = 0;
    uint64_t maxDecodedImageBytes = 0;
    uint32_t softCpuMicroseconds = 0;
    bool allowGpuMips = false; // mainで初期化した能力の値snapshotだけをworkerへ渡す。
};

// 続行ループはGameが所有する。Yieldは描画へ戻す境界。
enum class ScenePreparePumpResult { ContinueNow, Yield, Terminal };

struct SceneTextureTransferKey {
    ScenePrepareToken token;
    SceneInstanceId sourceInstanceId = kInvalidSceneInstanceId;
    uint64_t projectEpoch = 0;
    uint64_t entryId = 0;
};

// Gameの単一Update内でだけ借用する。受理時だけ画像の所有を移す。
struct SceneTextureUploadLoan {
    SceneTextureTransferKey key;
    const std::string* textureKey = nullptr;
    TextureManager::DecodedTexture* texture = nullptr;
    SceneTextureMipPolicy mipPolicy = SceneTextureMipPolicy::CpuComplete;
};

struct SceneTextureUploadTicket {
    SceneTextureTransferKey key;
    uint64_t uploadId = 0;
};

enum class SceneTextureUploadOutcome { Ready, Failed, Cancelled, RetryCpuMips };

// Readyの資源はScene側が証跡を照合してから公開する。
struct SceneTextureUploadCompletion {
    SceneTextureUploadTicket ticket;
    SceneTextureUploadOutcome outcome = SceneTextureUploadOutcome::Failed;
    CompletedTextureInput texture;
    uint64_t recordingSerial = 0, signaledFence = 0, completedFence = 0;
    std::string diagnostic;
};

// 候補文字のCPU画像。一件のみSceneの搬送枠へ渡す。
struct PreparedSceneTextImage {
    uint64_t entityId = 0;
    std::string textureKey, contentSignature;
    TextureManager::DecodedTexture texture;
};
