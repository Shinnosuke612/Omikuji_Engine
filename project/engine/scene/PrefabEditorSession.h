// 役割: Scene編集状態と分離してPrefabアセットのOpen/Save/Undoを管理する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "SceneDocument.h"
#include "SceneMenuLayout.h"

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

private:
	void PushUndoSnapshot(const SceneDocument& snapshot);

	SceneDocument document_;
	SceneDocument frameStartDocument_;
	SceneMenuLayout::EditorOrderCache menuOrderCache_;
	std::string filePath_;
	std::string lastError_;
	bool open_ = false;
	bool editFrameActive_ = false;
	uint64_t frameStartRevision_ = 0;
	std::vector<SceneDocument> undoStack_;
	std::vector<SceneDocument> redoStack_;
};
