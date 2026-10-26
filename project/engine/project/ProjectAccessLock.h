// 役割: Project生成/BuildとRegistry更新をプロセス間で排他し、呼出threadの拒否理由を保持する。
#pragma once

#include <filesystem>
#include <string>

enum class ProjectAccessIssue { None, Busy, UnsafePath, Unavailable, RegistryConflict, InvalidRegistry, PendingRecovery };

// 診断は同一threadの一操作だけで使う。表示層はenumを翻訳し、原文で動作分岐しない。
ProjectAccessIssue GetProjectAccessIssue();
void ClearProjectAccessIssue();
void ReportProjectAccessIssue(ProjectAccessIssue issue);

class ProjectAccessLock {
public:
	ProjectAccessLock() = default;
	~ProjectAccessLock();
	ProjectAccessLock(const ProjectAccessLock&) = delete;
	ProjectAccessLock& operator=(const ProjectAccessLock&) = delete;
	// 同一thread内の再入を許す。取得したthreadで破棄すること。待機せずBusyを返す。
	// 将来のBuild入口は外側のProject leaseを子Build終了まで保持する。
	bool TryProject(const std::filesystem::path& projectRoot, std::string& errorMessage);
	// Project→Registryの順で取得し、Registryはread-modify-writeの間だけ保持する。
	bool TryRegistry(const std::filesystem::path& registryPath, std::string& errorMessage);

private:
	bool TryAcquire(const wchar_t* domain, const std::filesystem::path& path, std::string& errorMessage);
	void* handle_ = nullptr;
};
