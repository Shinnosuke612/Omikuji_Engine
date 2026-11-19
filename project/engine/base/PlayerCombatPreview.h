// 役割: 選択Attackの姿勢・命中区間・限定移動をEditor専用コピーへ評価する。
// Session/Runtime/GPUを所有せず、原本・Undo・保存へ一時姿勢を書き戻さない。
#pragma once
#include "PlayerCombatEditor.h"
#include "../math/Matrix4x4.h"
#include <memory>

class PlayerCombatPreview final {
public:
	// Editor専用の値型。target/distanceはworld単位、yaw/pitchはrad。Assetへ保存しない。
	// GPU/Document参照や保存対象のCamera Componentを持たない。
	struct ViewState { Vector3 target{}; float distance = 5, yaw = .65f, pitch = .25f; bool initialized = false; };
	enum class Fit { None, All, Selected };
	struct EditVolume { bool valid = false, active = false; std::string shape; Vector3 offset{}, halfSize{}; float radius = 0; Matrix4x4 world{}; };
	struct ViewRequest {
		bool enabled = false;
		PlayerCombatEditor::DocumentToken token;
		uint64_t serial = 0, cameraSerial = 0, candidateSerial = 0;
		float time = 0;
		ViewState state;
		Fit fit = Fit::None;
		EditVolume volume;
	};
	struct ViewResult { ViewRequest request; bool valid = false; ViewState state; Matrix4x4 view{}, projection{}; uint32_t width = 0, height = 0; };
	struct Issue { std::string japanese, english, detail; };
	struct Window {
		uint64_t entityId = 0;
		std::string name;
		float start = 0, end = 0;
		bool active = false, visible = false, outside = false;
		Issue geometryIssue; // 新キーだけの不正ではPreview全体を終了しない。
	};
	// 秒単位の絶対時刻。失敗時は表示コピーを無効化し、原本は常に不変。
	// Hit Window対象へキー付きActive Trackも書き込むClipは、実行時と優先順が異なるため拒否する。
	bool RebuildAndSample(const SceneDocument& source, const PlayerCombatEditor::Subject& subject, float time);
	void Clear();
	// 非activeな選択Windowも独立評価。表示Documentの有効Windowを上書きしない。
	bool GetEditVolume(size_t window, float time, EditVolume& volume) const;
	// Render呼出し中だけ借用可能。次の評価/Clearを越えて保持しない。
	const SceneDocument* GetDocument() const { return display_.get(); }
	const Issue& GetIssue() const { return issue_; }
	const Issue& GetMotionIssue() const { return motionIssue_; }
	const SceneAttackDefinition& GetAttack() const { return attack_; }
	const std::vector<Window>& GetWindows() const { return windows_; }
	const std::string& GetClipName() const { return clipName_; }
	uint64_t GetRootId() const { return rootId_; }
	float GetDuration() const { return duration_; }
	bool HasActor() const { return hasActor_; }
	bool UsesAnimationFallback() const { return animationFallback_; }
private:
	std::unique_ptr<SceneDocument> display_;
	Issue issue_, motionIssue_;
	SceneAttackDefinition attack_;
	std::vector<Window> windows_;
	std::string clipName_;
	uint64_t rootId_ = 0;
	float duration_ = 0;
	bool hasActor_ = false, animationFallback_ = false;
};
