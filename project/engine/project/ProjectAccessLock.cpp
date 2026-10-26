// 役割: 正規化pathを識別子とする名前付きmutexを、資源ファイルを変更せず所有する。
#include "ProjectAccessLock.h"

#include <Windows.h>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
	thread_local ProjectAccessIssue lastIssue = ProjectAccessIssue::None;

	bool LockIdentity(const std::filesystem::path& path, std::wstring& identity) {
		if (path.empty() || !path.is_absolute() || path.native().find(L'\0') != std::wstring::npos) return false;
		std::error_code error;
		const auto normalized = path.lexically_normal();
		auto existing = normalized.root_path();
		for (const auto& part : normalized.relative_path()) {
			existing /= part;
			const DWORD attributes = GetFileAttributesW(existing.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES) {
				const DWORD status = GetLastError();
				if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) return false;
				break;
			}
			if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
		}
		// 存在する親までLongPathへ揃え、未作成Registryにも同じ識別規則を使う。
		auto ancestor = normalized;
		std::filesystem::path suffix;
		while (!std::filesystem::exists(ancestor, error)) {
			if (error || ancestor == ancestor.parent_path()) return false;
			suffix = suffix.empty() ? ancestor.filename() : ancestor.filename() / suffix;
			ancestor = ancestor.parent_path();
		}
		if (error) return false;
		const DWORD required = GetLongPathNameW(ancestor.c_str(), nullptr, 0);
		if (!required) return false;
		std::vector<wchar_t> buffer(required);
		const DWORD count = GetLongPathNameW(ancestor.c_str(), buffer.data(), required);
		if (!count || count >= required) return false;
		const auto full = (suffix.empty() ? std::filesystem::path(buffer.data()) : std::filesystem::path(buffer.data()) / suffix).lexically_normal().native();
		const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, full.data(), static_cast<int>(full.size()), nullptr, 0, nullptr, nullptr, 0);
		if (size <= 0) return false;
		identity.resize(static_cast<size_t>(size));
		return LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, full.data(), static_cast<int>(full.size()), identity.data(), size, nullptr, nullptr, 0) == size;
	}
}

ProjectAccessIssue GetProjectAccessIssue() { return lastIssue; }
void ClearProjectAccessIssue() { lastIssue = ProjectAccessIssue::None; }
void ReportProjectAccessIssue(ProjectAccessIssue issue) { lastIssue = issue; }

ProjectAccessLock::~ProjectAccessLock() {
	if (handle_) { ReleaseMutex(handle_); CloseHandle(handle_); }
}

bool ProjectAccessLock::TryProject(const std::filesystem::path& root, std::string& error) {
	return TryAcquire(L"Project", root, error);
}

bool ProjectAccessLock::TryRegistry(const std::filesystem::path& path, std::string& error) {
	return TryAcquire(L"Registry", path, error);
}

bool ProjectAccessLock::TryAcquire(const wchar_t* domain, const std::filesystem::path& path, std::string& error) {
	std::wstring identity;
	if (handle_ || !LockIdentity(path, identity)) {
		ReportProjectAccessIssue(ProjectAccessIssue::UnsafePath);
		error = "Project access requires a safe absolute path and an unused lease.";
		return false;
	}
	uint64_t hash = 14695981039346656037ull;
	for (wchar_t character : identity) { hash ^= static_cast<uint16_t>(character); hash *= 1099511628211ull; }
	std::wostringstream name;
	name << L"Global\\CG3.ProjectAccess." << domain << L'.' << std::hex << std::setw(16) << std::setfill(L'0') << hash;
	HANDLE handle = CreateMutexW(nullptr, FALSE, name.str().c_str());
	if (!handle) {
		ReportProjectAccessIssue(ProjectAccessIssue::Unavailable);
		error = "Project access mutex could not be opened (" + std::to_string(GetLastError()) + ").";
		return false;
	}
	const DWORD result = WaitForSingleObject(handle, 0);
	// 前ownerが終了していても、journal/hashの検証はcallerで必須のままにする。
	if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { handle_ = handle; return true; }
	CloseHandle(handle);
	ReportProjectAccessIssue(result == WAIT_TIMEOUT ? ProjectAccessIssue::Busy : ProjectAccessIssue::Unavailable);
	error = result == WAIT_TIMEOUT ? "Another Project operation or managed Build is still active. Retry after it finishes." : "Project access mutex could not be acquired.";
	return false;
}
