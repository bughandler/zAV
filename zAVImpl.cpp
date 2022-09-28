#include <functional>
#include <map>
#include <string>
#include <vector>
#include <windows.h>
#include <VersionHelpers.h>
#include <Wbemidl.h>
#include <comdef.h>
#include <iwscapi.h>
#include <shellapi.h>
#include "zAVImpl.h"

#pragma warning(disable : 4267)
#pragma comment(lib, "wbemuuid.lib")

static std::wstring gCoreDir, gCtrlFile;

enum class ServiceStatus {
	Failed,
	NotInstalled,
	Stopped,
	Running
};

//
// ScopeGuard implementation
//
class ScopeGuard {
public:
	explicit ScopeGuard(std::function<void()> fn) :
			exitFn_(fn), dismissed_(false) {}

	~ScopeGuard() {
		if (!dismissed_)
			exitFn_();
	}

	void dismiss() { dismissed_ = true; }

private:
	std::function<void()> exitFn_;
	bool dismissed_;

private:
	// not-copyable
	ScopeGuard(ScopeGuard const &);
	ScopeGuard &operator=(ScopeGuard const &);
};

#define SCOPEGUARD_LINENAME_CAT(name, line) name##line
#define SCOPEGUARD_NAME_BY_LINE(name, line) SCOPEGUARD_LINENAME_CAT(name, line)
#define ON_SCOPE_EXIT(callback) ScopeGuard SCOPEGUARD_NAME_BY_LINE(EXIT, __LINE__)(callback)

//
// WMI query utility
//
class WmiQuery {
public:
	using Row = std::map<std::wstring, VARIANT>;
	using Result = std::vector<Row>;

	WmiQuery(const std::wstring &wmiNamespace) {
		initializeWbemServices(wmiNamespace);
	}

	~WmiQuery() {
		if (servicesInf_) {
			servicesInf_->Release();
			servicesInf_ = nullptr;
		}
	}

	bool IsReady() {
		return servicesInf_ != nullptr;
	}

	Result Query(const std::wstring &wqlStr) {
		if (!servicesInf_) {
			return {};
		}

		HRESULT hr = 0;
		// perform queyring
		IEnumWbemClassObject *enumWbemInf = nullptr;
		hr = servicesInf_->ExecQuery((BSTR)L"WQL", (BSTR)wqlStr.c_str(), WBEM_FLAG_FORWARD_ONLY, nullptr, &enumWbemInf);
		if (FAILED(hr)) {
			return {};
		}
		ON_SCOPE_EXIT([enumWbemInf]() { enumWbemInf->Release(); });

		// extract result
		Result result;
		int count = 0;
		do {
			IWbemClassObject *resultObject = nullptr;
			ULONG resultCount = 0;

			hr = enumWbemInf->Next(WBEM_INFINITE, 1, &resultObject, &resultCount);
			if (FAILED(hr) || !resultObject) {
				break;
			}

			SAFEARRAY *propNameList = nullptr;
			hr = resultObject->GetNames(nullptr, WBEM_FLAG_ALWAYS | WBEM_FLAG_NONSYSTEM_ONLY, nullptr, &propNameList);
			if (FAILED(hr)) {
				break;
			}
			ON_SCOPE_EXIT([&]() { SafeArrayDestroy(propNameList); });

			long low, up;
			SafeArrayGetLBound(propNameList, 1, &low);
			SafeArrayGetUBound(propNameList, 1, &up);

			Row row;
			bool failed = false;
			for (long i = low; i <= up; ++i) {
				BSTR propName = nullptr;
				hr = SafeArrayGetElement(propNameList, &i, &propName);
				if (FAILED(hr)) {
					failed = true;
					break;
				}

				VARIANT var;
				CIMTYPE varType = 0;
				VariantInit(&var);
				hr = resultObject->Get(propName, 0, &var, &varType, nullptr);
				if (FAILED(hr)) {
					failed = true;
					break;
				}

				// save this key-value pair
				row[propName] = var;
			}

			// save this row
			if (!failed) {
				result.emplace_back(row);
				++count;
			}

		} while (true);

		return result;
	}

private:
	bool initializeWbemServices(const std::wstring &wmiNamespace) {
		bool succeed = false;
		HRESULT hr;

		IWbemLocator *locator = nullptr;
		hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *)&locator);
		if (FAILED(hr)) {
			return false;
		}
		ON_SCOPE_EXIT([&]() { locator->Release(); });

		IWbemServices *service = nullptr;
		hr = locator->ConnectServer((BSTR)wmiNamespace.c_str(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &service);
		if (FAILED(hr)) {
			return false;
		}
		ON_SCOPE_EXIT([&]() { if (!succeed) service->Release(); });

		hr = CoSetProxyBlanket(service, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
		if (FAILED(hr)) {
			return false;
		}

		// done
		servicesInf_ = service;
		return succeed = true;
	}

private:
	IWbemServices *servicesInf_ = nullptr;
};

static std::wstring GetModuleFileNameString(HMODULE mod) {
	wchar_t name[MAX_PATH] = { 0 };
	DWORD size = GetModuleFileNameW(mod, &name[0], sizeof(name) / sizeof(wchar_t));
	if (!size) {
		return {};
	}
	return { name, size };
}

std::string GetUserLang() {
	std::string lang;
	lang.resize(2);
	auto langid = GetUserDefaultLangID();
	GetLocaleInfoA(langid, LOCALE_SISO639LANGNAME, &lang[0], lang.size());
	return std::move(lang);
}

ServiceStatus CheckServiceStatus(const std::wstring &name) {
	auto hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!hSCM) {
		return ServiceStatus::Failed;
	}

	auto hSrv = OpenServiceW(hSCM, name.c_str(), SERVICE_QUERY_STATUS);
	if (!hSrv) {
		CloseServiceHandle(hSCM);
		return ServiceStatus::NotInstalled;
	}

	ServiceStatus result = ServiceStatus::Stopped;
	SERVICE_STATUS statusSrv = { 0 };
	if (QueryServiceStatus(hSrv, &statusSrv)) {
		if (statusSrv.dwCurrentState == SERVICE_RUNNING) {
			result = ServiceStatus::Running;
		}
	}

	CloseServiceHandle(hSrv);
	CloseServiceHandle(hSCM);
	return result;
}

bool IsFileExists(const std::wstring &filename) {
	auto v = GetFileAttributesW(filename.c_str());
	return v != INVALID_FILE_ATTRIBUTES && (v & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsDirectoryExists(const std::wstring &dirname) {
	auto v = GetFileAttributesW(dirname.c_str());
	return v != INVALID_FILE_ATTRIBUTES && (v & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring FilePathJoin(std::vector<std::wstring> paths) {
	std::wstring result;
	for (auto it = paths.begin(); it != paths.end(); it++) {
		result += *it;
		if (it + 1 != paths.end() && *result.rbegin() != L'\\') {
			result += '\\';
		}
	}
	return result;
}

static bool MatchWscProductRow(
		const WmiQuery::Row &row,
		const std::wstring &key,
		const std::wstring &desiredValue) {
	auto it = row.find(key);
	if (it == row.end() ||
			!it->second.bstrVal ||
			_wcsicmp(it->second.bstrVal, desiredValue.c_str()) != 0) {
		// match product name
		return false;
	}
	return true;
}

static bool ProbeWscProduct(
		const std::wstring &productType,
		const std::wstring &productName,
		const std::wstring &filePath) {
	WmiQuery wmi(LR"(root\securitycenter2)");
	if (!wmi.IsReady()) {
		return false;
	}

	wchar_t queryStr[512] = { 0 };
	_snwprintf_s(queryStr, sizeof(queryStr) - 1, LR"(SELECT * FROM %s)", productType.c_str());

	WmiQuery::Result result = wmi.Query(queryStr);
	if (result.empty()) {
		return false;
	}

	for (WmiQuery::Row &p : result) {
		if (MatchWscProductRow(p, L"displayName", productName) &&
				MatchWscProductRow(p, L"pathToSignedProductExe", filePath) &&
				MatchWscProductRow(p, L"pathToSignedReportingExe", filePath)) {
			// matched
			return true;
		}
	}
	return false;
}

static bool InstallElamSign(const std::wstring &elamDriverFile) {
	if (!IsWindows8OrGreater()) {
		// no need to do that
		return true;
	}

	using InstallELAMCertificateInfoFn = BOOL(WINAPI *)(HANDLE);
	static InstallELAMCertificateInfoFn InstallElamCertificateInfo = nullptr;
	if (!InstallElamCertificateInfo) {
		if (!(InstallElamCertificateInfo = (InstallELAMCertificateInfoFn)GetProcAddress(
					  GetModuleHandleW(L"kernel32"),
					  "InstallELAMCertificateInfo"))) {
			return false;
		}
	}

	HANDLE FileHandle = nullptr;
	FileHandle = CreateFileW(elamDriverFile.c_str(),
			FILE_READ_DATA,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
	ON_SCOPE_EXIT([&]() { CloseHandle(FileHandle); });

	return !!InstallElamCertificateInfo(FileHandle);
}

static bool PrepareControlEnv(bool isTakingover, bool flushToSystem = true) {
	bool succeed = false;

	auto modFile = GetModuleFileNameString(nullptr);
	if (modFile.empty()) {
		return false;
	}
	auto pos = modFile.rfind('\\');
	if (pos == std::string::npos) {
		return false;
	}
	auto coreDir = modFile.substr(0, pos + 1) + L"ctrl";

	// release control file
	auto ctrlFileName = FilePathJoin({ coreDir, L"wsctrlsvc.exe" });
	if (!IsFileExists(ctrlFileName)) {
		return false;
	}

	// copy hipsmain
	if (isTakingover) {
		auto binPath = FilePathJoin({ coreDir, L"bin" });
		if (!IsDirectoryExists(binPath)) {
			if (flushToSystem && !CreateDirectoryW(binPath.c_str(), nullptr)) {
				return false;
			}
		}

		auto mainEntryFile = FilePathJoin({ binPath, L"HipsMain.exe" });
		if (flushToSystem && !IsFileExists(mainEntryFile)) {
			wchar_t calcPath[MAX_PATH] = { 0 };
			ExpandEnvironmentStringsW(LR"(%SystemRoot%\system32\calc.exe)", calcPath, sizeof(calcPath) / sizeof(wchar_t));
			if (!CopyFileW(calcPath, mainEntryFile.c_str(), false)) {
				//return false;
			}
		}
	}

	// install ELAM certifcate
	if (isTakingover) {
		auto elamDriverFile = FilePathJoin({ coreDir, L"hrelam.sys" });
		if (!IsFileExists(elamDriverFile)) {
			return false;
		}

		if (!InstallElamSign(elamDriverFile)) {
			return false;
		}
	}

	// setup registy
	if (flushToSystem) {
		HKEY rootKey = nullptr;
		DWORD disposition = 0;
		if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Huorong\Sysdiag)", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &rootKey, &disposition)) {
			return false;
		}
		ON_SCOPE_EXIT([&]() { RegCloseKey(rootKey); });

		if (RegSetValueExW(rootKey,
					L"InstallPath",
					0,
					REG_SZ,
					(const BYTE *)coreDir.c_str(),
					coreDir.size() * sizeof(wchar_t))) {
			return false;
		}
	}

	// done
	gCoreDir = coreDir;
	gCtrlFile = ctrlFileName;
	return succeed = true;
}

DWORD ExecControlCmd(const std::wstring &args) {
	SHELLEXECUTEINFOW exec = { 0 };
	exec.cbSize = sizeof(exec);
	exec.fMask = SEE_MASK_NOCLOSEPROCESS;
	exec.lpFile = gCtrlFile.c_str();
	exec.lpParameters = args.c_str();
	exec.nShow = SW_HIDE;
	exec.lpVerb = L"runas";
	if (!ShellExecuteExW(&exec)) {
		return -1;
	}

	if (WaitForSingleObject(exec.hProcess, INFINITE) != WAIT_OBJECT_0) {
		return -1;
	}

	DWORD exitCode = -1;
	GetExitCodeProcess(exec.hProcess, &exitCode);
	return exitCode;
}

// Takeover WSC
bool Takeover() {
	if (!PrepareControlEnv(true)) {
		return false;
	}
	if (CheckServiceStatus(L"HRWSCCtrl") != ServiceStatus::Running) {
		ExecControlCmd(L"/regsvc");
	}
	ExecControlCmd(L"/enable");
	return true;
}

// Revert back
bool Revert() {
	if (!PrepareControlEnv(false)) {
		return false;
	}
	ExecControlCmd(L"/enable /disable");
	ExecControlCmd(L"/unwsc");

	int count = 0;
	while (count < 3 &&
			CheckServiceStatus(L"HRWSCCtrl") == ServiceStatus::Running) {
		++count;
		Sleep(500);
	}

	// done
	return true;
}

bool CheckStatus(bool &isRunning) {
	if (!PrepareControlEnv(false, false)) {
		return false;
	}
	isRunning = CheckServiceStatus(L"HRWSCCtrl") == ServiceStatus::Running;
	bool isZhCN = _stricmp(GetUserLang().c_str(), "zh") == 0;
	bool isWscProductPresent = ProbeWscProduct(L"AntiVirusProduct",
			isZhCN ? L"»ðÈÞ°²È«Èí¼þ" : L"Huorong Internet Security",
			gCtrlFile);
	return isWscProductPresent;
}
