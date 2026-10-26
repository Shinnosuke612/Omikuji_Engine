// 役割: Menuの開閉stack・選択・決定通知と描画snapshotを所有する。
#pragma once
#include "../../../../engine/scene/SceneMenuRuntime.h"
class SceneDocument;

// RuntimeScene所有。入力/描画/Document変更を行わず、IDだけを保持する。
class SceneMenuSystem {
public:
	// 内容置換/Instance変更は初期化する。Revisionだけの変更は選択を再検証する。
	void Sync(const SceneDocument& document, uint64_t instanceId);
	// 同一snapshotの値要求だけを適用。一Frame分の両方向要求は相殺する。
	void Apply(const SceneDocument& document, const SceneMenuMove& move);
	// 失効した出所/対象は拒否し、Documentには書き込まない。
	bool Apply(const SceneDocument& document, const SceneMenuCommand& command);
	bool IsSourceValid(const SceneDocument& document, const SceneMenuSource& source) const;
	SceneMenuSource GetSource() const;
	// 通知は一回消費。Cancel(Close)は通知評価後に同じtopだけを閉じる。
	std::vector<SceneMenuPulse> ConsumePulses();
	void CompleteCancel(const SceneDocument& document, const SceneMenuPulse& pulse);
	std::string ConsumeDiagnostic();
	void Clear();
	const SceneMenuFrame& GetFrame() const { return frame_; }
	bool Matches(const SceneDocument& document, uint64_t instanceId) const;
private:
	struct PanelState {
		uint64_t id = 0, serial = 0, selected = 0;
		std::vector<uint64_t> order;
	};
	// 既存の単一Panel検証/強調表示をstack各層で共有する。
	bool RefreshPanel(const SceneDocument& document, uint64_t instanceId, uint64_t panelId, bool opening);
	bool Selectable(const SceneDocument& document, uint64_t panelId, uint64_t itemId) const;
	bool Open(const SceneDocument& document, uint64_t panelId);
	void Rebuild(const SceneDocument& document);
	std::vector<PanelState> stack_;
	std::vector<SceneMenuPulse> pulses_;
	std::string diagnostic_;
	uint64_t nextOpenSerial_ = 1;
	bool initialized_ = false;
	SceneMenuFrame frame_;
	std::vector<uint64_t> order_;
	std::vector<uint64_t> eligible_;
	bool initialRejected_ = false;
};
