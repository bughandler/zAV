// MainDlg.cpp : implementation of the CMainDlg class
//
/////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "resource.h"

#include "aboutdlg.h"
#include "MainDlg.h"
#include "../zAVImpl.h"
#pragma warning(disable: 4996)

#define CHECK_STATUS_TIMER (0x10013)

BOOL CMainDlg::PreTranslateMessage(MSG *pMsg) {
	return CWindow::IsDialogMessage(pMsg);
}

BOOL CMainDlg::OnIdle() {
	UIUpdateChildWindows();
	return FALSE;
}

LRESULT CMainDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL & /*bHandled*/) {
	// center the dialog on the screen
	CenterWindow();

	// set icons
	HICON hIcon = AtlLoadIconImage(IDR_MAINFRAME, LR_DEFAULTCOLOR, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON));
	SetIcon(hIcon, TRUE);
	HICON hIconSmall = AtlLoadIconImage(IDR_MAINFRAME, LR_DEFAULTCOLOR, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON));
	SetIcon(hIconSmall, FALSE);

	// register object for message filtering and idle updates
	CMessageLoop *pLoop = _Module.GetMessageLoop();
	ATLASSERT(pLoop != NULL);
	pLoop->AddMessageFilter(this);
	pLoop->AddIdleHandler(this);

	UIAddChildWindowContainer(m_hWnd);

	UpdateStatus();
	statusTimer_ = SetTimer(CHECK_STATUS_TIMER, 2000, nullptr);

	return TRUE;
}

LRESULT CMainDlg::OnDestroy(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL & /*bHandled*/) {
	// unregister message filtering and idle updates
	CMessageLoop *pLoop = _Module.GetMessageLoop();
	ATLASSERT(pLoop != NULL);
	pLoop->RemoveMessageFilter(this);
	pLoop->RemoveIdleHandler(this);
	KillTimer(statusTimer_);
	return 0;
}

LRESULT CMainDlg::OnAppAbout(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL & /*bHandled*/) {
	CAboutDlg dlg;
	dlg.DoModal();
	return 0;
}

LRESULT CMainDlg::OnOK(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL & /*bHandled*/) {
	// TODO: Add validation code
	CloseDialog(wID);
	return 0;
}

LRESULT CMainDlg::OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL & /*bHandled*/) {
	CloseDialog(wID);
	return 0;
}

LRESULT CMainDlg::OnTimer(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL &bHandled) {
	if (wParam == CHECK_STATUS_TIMER) {
		UpdateStatus();
		bHandled = TRUE;
	}
	return 0;
}

void CMainDlg::CloseDialog(int nVal) {
	DestroyWindow();
	::PostQuitMessage(nVal);
}

LRESULT CMainDlg::OnBnClickedTakeover(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL & /*bHandled*/) {
	if (!Takeover()) {
		MessageBoxW(L"Unable to takeover windows security center", L"Error", MB_OK);
	}
	UpdateStatus();
	return 0;
}

LRESULT CMainDlg::OnBnClickedRevert(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL & /*bHandled*/) {
	if (!Revert()) {
		MessageBoxW(L"Unable to revert default settings for windows security center", L"Error", MB_OK);
	}
	UpdateStatus();
	return 0;
}

void CMainDlg::UpdateStatus() {
	bool isRunning = false;
	bool isInstalled = CheckStatus(isRunning);

	wchar_t installedMsg[64] = { 0 }, runningMsg[64] = { 0 };

	CStatic installedStatus = GetDlgItem(IDC_INSTALLED_STATUS);
	_snwprintf(installedMsg, sizeof(installedMsg) / sizeof(wchar_t) - 1, L"  Installed:    %s", isInstalled ? L"Yes" : L"No");
	installedStatus.SetWindowText(installedMsg);

	CStatic runningStatus = GetDlgItem(IDC_RUNING_STATUS);
	_snwprintf(runningMsg, sizeof(runningMsg) / sizeof(wchar_t) - 1, L"  Running:    %s", isRunning ? L"Yes" : L"No");
	runningStatus.SetWindowText(runningMsg);
}
