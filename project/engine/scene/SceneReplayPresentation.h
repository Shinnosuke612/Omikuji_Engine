// 役割: Replayの設定/表示tokenと、状態を持たない構図計算を共有する。Scene/GPU資源は所有しない。
#pragma once
#include <cstdint>
#include <string>
#include <cmath>
#include <vector>
#include "../math/Vector3.h"
#include "../3d/Camera.h"
#include "SceneSettings.h"
#include <algorithm>
#include <limits>

struct SceneReplayViewSettings {
    std::string mode = "Auto";
    float yaw = 45.0f;
    float distanceInHeights = 3.0f;
    float cameraHeightInHeights = 1.2f;
    float lookHeightInHeights = 0.5f;
    float fovYDegrees = 45.0f;
    Vector3 localOffset{};
    float targetHeight = 0.0f;
    bool operator==(const SceneReplayViewSettings& v) const {
        return mode==v.mode && yaw==v.yaw && distanceInHeights==v.distanceInHeights &&
            cameraHeightInHeights==v.cameraHeightInHeights && lookHeightInHeights==v.lookHeightInHeights &&
            fovYDegrees==v.fovYDegrees && localOffset.x==v.localOffset.x && localOffset.y==v.localOffset.y &&
            localOffset.z==v.localOffset.z && targetHeight==v.targetHeight;
    }
};

struct SceneReplayPresentationToken {
    uint64_t instance = 0, generation = 0, owner = 0, deathSerial = 0;
    uint32_t cut = 0;
    uint64_t poseSerial = 0, drawSerial = 0;
    bool operator==(const SceneReplayPresentationToken&) const = default;
};

// 保存時/Runtime/Inspectorで同じ拒否条件を使う。既存Camera UIの1〜179度を維持。
inline bool IsValidReplayView(const SceneReplayViewSettings& v) {
    return (v.mode == "Auto" || v.mode == "Manual") && std::isfinite(v.yaw) &&
        std::isfinite(v.distanceInHeights) && v.distanceInHeights > 0 &&
        std::isfinite(v.cameraHeightInHeights) && std::isfinite(v.lookHeightInHeights) &&
        std::isfinite(v.fovYDegrees) && v.fovYDegrees >= 1 && v.fovYDegrees <= 179 &&
        std::isfinite(v.localOffset.x) && std::isfinite(v.localOffset.y) && std::isfinite(v.localOffset.z) &&
        std::isfinite(v.targetHeight);
}
inline std::vector<SceneReplayViewSettings> DefaultReplayViews() {
    std::vector<SceneReplayViewSettings> result(3);
    result[1].yaw = 90; result[2].yaw = 135;
    return result;
}

struct SceneReplayGameFrame {
    SceneReplayPresentationToken token;
    Camera camera;
    ScenePostProcessSettings postProcess;
    float effectTime = 0;
    bool finishedHold = false;
};

// 対象の水平基準から固定構図を解決する。Scene/Camera Entityを変更しない。
// manualOutには解決済みoffsetを返す。自動→手動への明示操作だけが保存する。失敗時はout不変。
inline bool ResolveReplayCamera(const SceneReplayViewSettings& view, const Matrix4x4& ownerWorld,
    const Matrix4x4& fallbackCameraWorld, const Vector3& originMin, const Vector3& originMax,
    const Vector3& clipMin, const Vector3& clipMax, float aspect, Camera& out,
    SceneReplayViewSettings* manualOut = nullptr) {
    if (!IsValidReplayView(view) || !std::isfinite(aspect) || aspect <= 0) return false;
    auto finite = [](const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite(originMin) || !finite(originMax) || !finite(clipMin) || !finite(clipMax)) return false;
    Vector3 forward{ownerWorld.m[2][0],0,ownerWorld.m[2][2]};
    double length = std::hypot(double(forward.x),double(forward.z));
    if (!std::isfinite(length) || length <= std::numeric_limits<float>::epsilon()) {
        forward = {fallbackCameraWorld.m[2][0],0,fallbackCameraWorld.m[2][2]};
        length = std::hypot(double(forward.x),double(forward.z));
    }
    if (!std::isfinite(length) || length <= std::numeric_limits<float>::epsilon()) return false;
    forward.x = static_cast<float>(forward.x / length); forward.z = static_cast<float>(forward.z / length);
    const Vector3 right{forward.z,0,-forward.x};
    const Vector3 origin{ownerWorld.m[3][0],ownerWorld.m[3][1],ownerWorld.m[3][2]};
    if (!finite(origin)) return false;
    Vector3 offset = view.localOffset;
    float targetHeight = view.targetHeight;
    constexpr float radians = 3.14159265358979323846f / 180.0f;
    if (view.mode == "Auto") {
        const float height = originMax.y - originMin.y;
        if (!std::isfinite(height) || height <= 0) return false;
        const float yaw = view.yaw * radians;
        offset = {std::sin(yaw)*view.distanceInHeights*height, view.cameraHeightInHeights*height,
            std::cos(yaw)*view.distanceInHeights*height};
        targetHeight = view.lookHeightInHeights*height;
    }
    const Vector3 position{origin.x+right.x*offset.x+forward.x*offset.z, origin.y+offset.y,
        origin.z+right.z*offset.x+forward.z*offset.z};
    const Vector3 target{origin.x,origin.y+targetHeight,origin.z};
    const Vector3 direction{target.x-position.x,target.y-position.y,target.z-position.z};
    const double distance = std::hypot(double(direction.x),double(direction.y),double(direction.z));
    // LookAtの上方向と平行な構図も拒否し、非有限行列を描画へ渡さない。
    if (!finite(position) || !finite(target) || !std::isfinite(distance) || distance <= 0 ||
        std::hypot(direction.x,direction.z) <= std::numeric_limits<float>::epsilon()) return false;
    double minDepth = (std::numeric_limits<double>::max)(), maxDepth = 0;
    for (int corner=0;corner<8;++corner) {
        Vector3 point{(corner&1)?clipMax.x:clipMin.x,(corner&2)?clipMax.y:clipMin.y,(corner&4)?clipMax.z:clipMin.z};
        const double depth = ((double(point.x)-position.x)*direction.x+(double(point.y)-position.y)*direction.y+
            (double(point.z)-position.z)*direction.z)/distance;
        minDepth = (std::min)(minDepth,depth); maxDepth = (std::max)(maxDepth,depth);
    }
    if (!std::isfinite(maxDepth) || maxDepth <= 0) return false;
    Camera camera;
    // Cameraの既定clipを起点に、前方boundsへ近面を縮め、遠面にはfloat丸めの余地を取る。
    const float nearClip = minDepth > 0 ? (std::min)(camera.GetNearClip(), static_cast<float>(minDepth*0.5)) : camera.GetNearClip();
    const double farClip = (std::max)(double(camera.GetFarClip()),maxDepth+(std::max)(1.0,maxDepth)*std::numeric_limits<float>::epsilon()*16);
    if (nearClip <= 0 || farClip > (std::numeric_limits<float>::max)() || farClip <= nearClip) return false;
    camera.SetNearClip(nearClip); camera.SetFarClip(static_cast<float>(farClip));
    camera.SetFovY(view.fovYDegrees*radians); camera.SetAspectRatio(aspect);
    camera.SetLookAt(position,target); camera.UpdatePreviewMatrices();
    for (const auto& row : camera.GetViewProjectionMatrix().m) for (float x : row) if (!std::isfinite(x)) return false;
    if (manualOut) { *manualOut=view; manualOut->mode="Manual"; manualOut->localOffset=offset; manualOut->targetHeight=targetHeight; }
    out=camera; return true;
}

// Editorだけの構図確認request。Entity/Sceneへの可変参照を保持しない。
struct SceneReplayFramingRequest {
    uint64_t owner=0, generation=0, serial=0;
    uint32_t cut=0;
    bool prefab=false, preview=false, convertToManual=false;
    SceneReplayViewSettings view;
    std::string sourcePath;
};
struct SceneReplayFramingBasis {
    Matrix4x4 world{};
    Vector3 min{}, max{};
    bool valid=false;
};
