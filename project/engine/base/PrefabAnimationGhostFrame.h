// 役割: Prefabキー虚像の評価済み姿勢と描画結果を、EditorとRenderer間で値として受け渡す。
// Document/GPU資源を所有・借用しない。Frameの借用は一回のRender呼出し中に限る。
#pragma once
#include "../scene/SceneDocument.h"
#include "../math/Matrix4x4.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace PrefabAnimationGhost {
    inline constexpr std::size_t kPosesPerPage = 32;
    inline constexpr std::size_t kMaxMeshes = 256;
    enum class Issue { None, InvalidClip, InvalidTrack, InvalidHierarchy, InvalidDraft,
        BudgetExceeded, NoKeys, Incompatible, MissingModel, Skinning, InvalidBounds };

    struct ContextToken {
        std::string assetPath, clipName;
        uint64_t generation = 0, revision = 0, owner = 0, target = 0, draftSerial = 0;
        int clip = -1;
        bool operator==(const ContextToken& other) const {
            return assetPath == other.assetPath && clipName == other.clipName && generation == other.generation &&
                revision == other.revision && owner == other.owner && target == other.target &&
                draftSerial == other.draftSerial && clip == other.clip;
        }
    };
    struct Mesh {
        uint64_t entity = 0;
        std::string modelPath, cullMode;
        Matrix4x4 world = MakeIdentity4x4(); // Entity World。Model RootNodeはRendererで一度だけ適用。
        std::vector<SceneMeshMaterialOverride> materials;
    };
    struct Pose {
        uint64_t id = 0; // poseSerial内だけで有効。時刻の表示文字列をIDにしない。
        float time = 0;
        Vector3 origin{};
        std::vector<Mesh> meshes;
    };
    struct Frame {
        ContextToken context;
        uint64_t poseSerial = 0, requestSerial = 0, selected = 0, framingSerial = 0;
        float opacity = .20f, time = 0, yaw = 0, pitch = 0, zoom = 1;
        uint32_t width = 0, height = 0;
        std::vector<Pose> poses;
    };
    struct Bounds { uint64_t entity = 0; Vector3 min{}, max{}; };
    struct PoseResult {
        uint64_t id = 0;
        float time = 0;
        Vector3 origin{};
        std::vector<Bounds> bounds;
        Issue issue = Issue::None;
        std::string subject;
    };
    struct Result {
        ContextToken context;
        uint64_t poseSerial = 0, requestSerial = 0;
        Matrix4x4 view = MakeIdentity4x4(), projection = MakeIdentity4x4();
        uint32_t width = 0, height = 0;
        std::vector<PoseResult> poses;
    };
}
