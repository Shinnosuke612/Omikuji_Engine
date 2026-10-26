// 役割: Prefab専用Documentの編集履歴をSceneのPlay状態から独立して保持する。
#include "PrefabEditorSession.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace {
	constexpr size_t kMaxPrefabUndoSnapshots = 100;

	bool HasSingleRoot(const SceneDocument& document) {
		return std::count_if(
			document.GetEntities().begin(),
			document.GetEntities().end(),
			[](const SceneEntity& entity) { return entity.parentId == 0; }
		) == 1;
	}
}

bool PrefabEditorSession::Open(const std::string& filePath) {
	if (open_ && filePath == filePath_) {
		return true;
	}
	if (filePath.empty() || IsDirty()) {
		lastError_ = IsDirty()
			? "Save or close the current Prefab before opening another one."
			: "Prefab path is empty.";
		return false;
	}

	SceneDocument loaded;
	if (!loaded.Load(filePath)) {
		lastError_ = loaded.GetLastLoadError();
		return false;
	}
	if (!HasSingleRoot(loaded)) {
		lastError_ = "A Prefab must contain exactly one root Entity.";
		return false;
	}
	document_ = std::move(loaded);
	document_.MarkClean();
	menuOrderCache_.Adopt(document_);
	frameStartDocument_ = document_;
	frameStartRevision_ = document_.GetRevision();
	filePath_ = filePath;
	lastError_.clear();
	open_ = true;
	editFrameActive_ = false;
	undoStack_.clear();
	redoStack_.clear();
	return true;
}

bool PrefabEditorSession::Save() {
	if (!open_ || filePath_.empty()) {
		lastError_ = "No Prefab is open.";
		return false;
	}
	if (!HasSingleRoot(document_)) {
		lastError_ = "A Prefab must contain exactly one root Entity.";
		return false;
	}
	BeginEditFrame();
	menuOrderCache_.Refresh(document_, true);
	EndEditFrame(true);
	if (!document_.Save(filePath_)) {
		lastError_ = document_.GetLastSaveError().empty()
			? "Failed to save Prefab: " + filePath_
			: document_.GetLastSaveError();
		return false;
	}
	menuOrderCache_.Adopt(document_);
	frameStartDocument_ = document_;
	frameStartRevision_ = document_.GetRevision();
	editFrameActive_ = false;
	// 保存以前の履歴は現在の保存地点とDirty状態を比較できないため切り離す。
	undoStack_.clear();
	redoStack_.clear();
	lastError_.clear();
	return true;
}

bool PrefabEditorSession::Reload() {
	if (!open_ || filePath_.empty()) {
		return false;
	}
	const std::string path = filePath_;
	document_.MarkClean();
	open_ = false;
	return Open(path);
}

bool PrefabEditorSession::Close(bool discardUnsavedChanges) {
	if (!open_) {
		return true;
	}
	if (IsDirty() && !discardUnsavedChanges) {
		lastError_ = "Prefab has unsaved changes.";
		return false;
	}
	document_.Clear();
	menuOrderCache_.Adopt(document_);
	frameStartDocument_.Clear();
	filePath_.clear();
	lastError_.clear();
	open_ = false;
	editFrameActive_ = false;
	frameStartRevision_ = 0;
	undoStack_.clear();
	redoStack_.clear();
	return true;
}

void PrefabEditorSession::BeginEditFrame() {
	if (!open_ || editFrameActive_) {
		return;
	}
	frameStartDocument_ = document_;
	frameStartRevision_ = document_.GetRevision();
	editFrameActive_ = true;
}

void PrefabEditorSession::EndEditFrame(bool commit) {
	if (!editFrameActive_) {
		return;
	}
	// 配置と派生した選択順を同じUndo snapshotへ収める。
	if (document_.GetRevision() != frameStartRevision_) menuOrderCache_.Refresh(document_);
	if (document_.GetRevision() == frameStartRevision_) {
		editFrameActive_ = false;
		return;
	}
	if (!commit) {
		return;
	}
	editFrameActive_ = false;
	PushUndoSnapshot(frameStartDocument_);
	redoStack_.clear();
}

bool PrefabEditorSession::Undo() {
	if (!open_ || undoStack_.empty()) {
		return false;
	}
	redoStack_.push_back(document_);
	document_ = undoStack_.back();
	undoStack_.pop_back();
	menuOrderCache_.Adopt(document_);
	frameStartDocument_ = document_;
	frameStartRevision_ = document_.GetRevision();
	editFrameActive_ = false;
	return true;
}

bool PrefabEditorSession::Redo() {
	if (!open_ || redoStack_.empty()) {
		return false;
	}
	undoStack_.push_back(document_);
	document_ = redoStack_.back();
	redoStack_.pop_back();
	menuOrderCache_.Adopt(document_);
	frameStartDocument_ = document_;
	frameStartRevision_ = document_.GetRevision();
	editFrameActive_ = false;
	return true;
}

void PrefabEditorSession::PushUndoSnapshot(const SceneDocument& snapshot) {
	undoStack_.push_back(snapshot);
	if (undoStack_.size() > kMaxPrefabUndoSnapshots) {
		undoStack_.erase(undoStack_.begin());
	}
}
