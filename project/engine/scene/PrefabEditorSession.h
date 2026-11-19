// 役割: Scene編集状態と分離してPrefabアセットのOpen/Save/Undoを管理する。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SceneDocument.h"
#include "SceneMenuLayout.h"

// 最後に観測したファイルの内容。現在のdiskとの一致/排他を保証するTokenではない。
struct PrefabDiskToken {
	std::string normalizedPath, contentBytes;
	bool valid = false;
};

class PrefabEditorSession final {
public:
	bool Open(const std::string& filePath);
	bool Save();
	bool Reload();
	bool Close(bool discardUnsavedChanges = false);

	void BeginEditFrame();
	void EndEditFrame(bool commit);
	bool Undo();
	bool Redo();

	bool IsOpen() const { return open_; }
	bool IsDirty() const { return open_ && document_.IsDirty(); }
	bool CanUndo() const { return !undoStack_.empty(); }
	bool CanRedo() const { return !redoStack_.empty(); }
	SceneDocument& GetDocument() { return document_; }
	const SceneDocument& GetDocument() const { return document_; }
	const std::string& GetFilePath() const { return filePath_; }
	const std::string& GetLastError() const { return lastError_; }
	// Open時の基準。通常Save/Undo/Redoでは進めず、Close/別Openで失効する。取得不能ならnullptr。
	const SceneDocument* GetOpenBaselineDocument() const { return open_ && openBaseline_ ? &*openBaseline_ : nullptr; }
	// Open/Save/Closeで更新。確認画面を跨ぐ呼出側は値コピーし、書込直前にdiskを再照合する。
	const PrefabDiskToken& GetLastKnownDiskToken() const { return diskToken_; }

private:
	void PushUndoSnapshot(const SceneDocument& snapshot);

	SceneDocument document_;
	SceneDocument frameStartDocument_;
	std::optional<SceneDocument> openBaseline_;
	PrefabDiskToken diskToken_;
	SceneMenuLayout::EditorOrderCache menuOrderCache_;
	std::string filePath_;
	std::string lastError_;
	bool open_ = false;
	bool editFrameActive_ = false;
	uint64_t frameStartRevision_ = 0;
	std::vector<SceneDocument> undoStack_;
	std::vector<SceneDocument> redoStack_;
};
