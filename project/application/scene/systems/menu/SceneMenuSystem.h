// 役割: Menuの開閉stack・選択・決定通知と、Menuに対応するゲーム停止理由を所有する。
#pragma once
#include "../../../../engine/scene/SceneMenuRuntime.h"
class SceneDocument;
struct SceneComponent;

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
	// 背後のPanelもOpen serialで参照し、確認画面を開いてもPause rootを保持する。
	SceneMenuSource GetOpenSource(uint64_t panelId) const;
	// 失効/互換Resume専用。指定rootと上の確認画面をまとめて除去する。
	void RemoveBranch(const SceneDocument& document, uint64_t panelId);
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

// Menu用停止理由だけを所有する。Entity/ComponentのpointerやAuthoring値は保持しない。
// 同じファイルに実装を置き、既存のビルド登録を利用する。
class SceneGamePauseSystem {
public:
	// nullは通常Menu、error非空は不正構成。既存外部GamePause参照も共通解決する。
	static const SceneComponent* FindSettings(const SceneDocument& document, uint64_t panelId, std::string& error);
	// 開いているrootが不正ならそのIDを返す。callerがbranchを除去し診断する。
	uint64_t Sync(const SceneDocument& document, const SceneMenuSystem& menus, bool supported, std::string& error);
	bool IsPaused() const { return root_.panelId != 0; }
	bool PausesBgm() const { return IsPaused() && pauseBgm_; }
	void Clear() { root_ = {}; pauseBgm_ = false; }
private:
	SceneMenuSource root_;
	bool pauseBgm_ = false;
};
