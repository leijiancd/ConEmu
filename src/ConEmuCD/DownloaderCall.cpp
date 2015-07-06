﻿
/*
Copyright (c) 2013-2015 Maximus5
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define HIDE_USE_EXCEPTION_INFO
#include <Windows.h>
#include "../common/defines.h"
#include "../common/MAssert.h"
#include "../common/MSectionSimple.h"
#include "ConEmuC.h"
#include "ExitCodes.h"
#include "DownloaderCall.h"

class CDownloader
{
protected:
	bool mb_AsyncMode;
	
	struct {
		wchar_t* szProxy;
		wchar_t* szProxyUser;
		wchar_t* szProxyPassword;
	} m_Proxy;

	struct {
		wchar_t* szUser;
		wchar_t* szPassword;
	} m_Server;
	
	bool  mb_OTimeout;
	DWORD mn_OTimeout; // [-otimeout <ms>]
	bool  mb_Timeout;
	DWORD mn_Timeout;  // [-timeout <ms>]

	FDownloadCallback mfn_Callback[dc_LogCallback+1];
	LPARAM m_CallbackLParam[dc_LogCallback+1];

	bool  mb_RequestTerminate;

	// ConEmuC.exe
	STARTUPINFO m_SI;
	PROCESS_INFORMATION m_PI;

public:
	// asProxy = "" - autoconfigure
	// asProxy = "server:port"
	void SetProxy(LPCWSTR asProxy, LPCWSTR asProxyUser, LPCWSTR asProxyPassword)
	{
		SafeFree(m_Proxy.szProxy);
		SafeFree(m_Proxy.szProxyUser);
		if (m_Proxy.szProxyPassword)
			SecureZeroMemory(m_Proxy.szProxyPassword, lstrlen(m_Proxy.szProxyPassword)*sizeof(*m_Proxy.szProxyPassword));
		SafeFree(m_Proxy.szProxyPassword);

		if (asProxy)
			m_Proxy.szProxy = lstrdup(asProxy);
		if (asProxyUser)
			m_Proxy.szProxyUser = lstrdup(asProxyUser);
		if (asProxyPassword)
			m_Proxy.szProxyPassword = lstrdup(asProxyPassword);
	};

	void SetLogin(LPCWSTR asUser, LPCWSTR asPassword)
	{
		SafeFree(m_Server.szUser);
		if (m_Server.szPassword)
			SecureZeroMemory(m_Server.szPassword, lstrlen(m_Server.szPassword)*sizeof(*m_Server.szPassword));
		SafeFree(m_Server.szPassword);

		if (asUser)
			m_Server.szUser = lstrdup(asUser);
		if (asPassword)
			m_Server.szPassword = lstrdup(asPassword);
	};

	// Logging, errors, download progress
	void SetCallback(CEDownloadCommand cb, FDownloadCallback afnErrCallback, LPARAM lParam)
	{
		if (cb > dc_LogCallback)
		{
			_ASSERTE(cb <= dc_LogCallback && cb >= dc_ErrCallback);
			return;
		}
		mfn_Callback[cb] = afnErrCallback;
		m_CallbackLParam[cb] = lParam;
	};

	void SetAsync(bool bAsync)
	{
		//ReportMessage(dc_LogCallback, L"Change mode to %s was requested", at_Str, bAsync?L"Async":L"Sync", at_None);
		mb_AsyncMode = bAsync;
	};

	void SetTimeout(bool bOperation, DWORD nTimeout)
	{
		//ReportMessage(dc_LogCallback, L"Set %s timeout to %u was requested", at_Str, bOperation?L"operation":L"receive", at_Uint, nTimeout, at_None);
		if (bOperation)
		{
			mn_OTimeout = nTimeout;
		}
		else
		{
			mn_Timeout = nTimeout;
		}
	};

	void CloseInternet(bool bFull)
	{
		DWORD nWait = WAIT_OBJECT_0;
		if (m_PI.hProcess)
		{
			nWait = WaitForSingleObject(m_PI.hProcess, 0);
			if (nWait != WAIT_OBJECT_0)
			{
				TerminateProcess(m_PI.hProcess, CERR_DOWNLOAD_FAILED);
			}
		}

		CloseHandles();
	};

	void RequestTerminate()
	{
		mb_RequestTerminate = true;
		CloseInternet(true);
	};

protected:
	void CloseHandles()
	{
		SafeCloseHandle(m_PI.hProcess);
		SafeCloseHandle(m_PI.hThread);
	};

public:
	BOOL DownloadFile(LPCWSTR asSource, LPCWSTR asTarget, DWORD& crc, DWORD& size, BOOL abShowAllErrors = FALSE)
	{
		BOOL bRc = FALSE;
		DWORD nWait;
		wchar_t szConEmuC[MAX_PATH] = L"", *psz, *pszCommand = NULL;
		wchar_t szOTimeout[20] = L"", szTimeout[20] = L"";

		if (mb_OTimeout)
			_wsprintf(szOTimeout, SKIPCOUNT(szOTimeout) L"%u", mn_OTimeout);
		if (mb_Timeout)
			_wsprintf(szTimeout, SKIPCOUNT(szTimeout) L"%u", mn_Timeout);

		struct _Switches {
			LPCWSTR pszName, pszValue;
		} Switches[] = {
			{L"-login", m_Server.szUser},
			{L"-password", m_Server.szPassword},
			{L"-proxy", m_Proxy.szProxy},
			{L"-proxylogin", m_Proxy.szProxyUser},
			{L"-proxypassword", m_Proxy.szProxyPassword},
			{L"-async", mb_AsyncMode ? L"Y" : L"N"},
			{L"-otimeout", szOTimeout},
			{L"-timeout", szTimeout},
			{L"-fhandle", szDstFileHandle},
		};

		_ASSERTE(m_PI.hProcess==NULL);

		if (!asSource || !*asSource || !asTarget || !*asTarget)
		{
			goto wrap;
		}

		if (!GetModuleFileName(ghOurModule, szConEmuC, countof(szConEmuC)) || !(psz = wcsrchr(szConEmuC, L'\\')))
		{
			//ReportMessage(dc_LogCallback, L"GetModuleFileName(ghOurModule) failed, code=%u", at_Uint, GetLastError(), at_None);
			goto wrap;
		}
		psz[1] = 0;
		wcscat_c(szConEmuC, WIN3264TEST(L"ConEmuC.exe",L"ConEmuC64.exe"));

		/*
		ConEmuC /download [-login <name> -password <pwd>]
		        [-proxy <address:port> [-proxylogin <name> -proxypassword <pwd>]]
		        [-async Y|N] [-otimeout <ms>] [-timeout <ms>]
		        "full_url_to_file" "local_path_name"
		*/

		if (!(pszCommand = lstrmerge(L"\"", szConEmuC, L"\" -download ")))
			goto wrap;

		for (INT_PTR i = 0; i < countof(Switches); i++)
		{
			LPCWSTR pszValue = Switches[i].pszValue;
			if (pszValue && *pszValue && !lstrmerge(&pszCommand, Switches[i].pszName, L" \"", pszValue, L"\" "))
				goto wrap;
		}

		if (!lstrmerge(&pszCommand, asSource, L" \"", asTarget, L"\""))
			goto wrap;

		ZeroStruct(m_SI);
		ZeroStruct(m_PI);

		if (!CreateProcess(NULL, pszCommand, NULL, NULL, TRUE, NORMAL_PRIORITY_CLASS, NULL, NULL, &m_SI, &m_PI))
			goto wrap;

		nWait = WaitForSingleObject(m_PI.hProcess, INFINITE);
		if (GetExitCodeProcess(m_PI.hProcess, &nWait)
			&& (nWait == CERR_DOWNLOAD_SUCCEEDED))
		{
			_ASSERTE(FALSE && "Need to get size & crc!!!");
			bRc = TRUE;
		}

	wrap:
		SafeFree(pszCommand);
		CloseHandles();
		return bRc;
	};

public:
	CDownloader()
	{
		mb_AsyncMode = true;
		ZeroStruct(m_Server);
		ZeroStruct(m_Proxy);
		ZeroStruct(mfn_Callback);
		ZeroStruct(m_CallbackLParam);
		mb_Timeout = false; mn_Timeout = DOWNLOADTIMEOUT;
		mb_OTimeout = false; mn_OTimeout = 0;
		mb_RequestTerminate = false;
		ZeroStruct(m_SI); m_SI.cb = sizeof(m_SI);
		ZeroStruct(m_PI);
	};

	~CDownloader()
	{
		CloseInternet(true);
		SetProxy(NULL, NULL, NULL);
		SetLogin(NULL, NULL);
	};
};

static CDownloader* gpInet = NULL;

#if defined(__GNUC__)
extern "C"
#endif
DWORD_PTR WINAPI DownloadCommand(CEDownloadCommand cmd, int argc, CEDownloadErrorArg* argv)
{
	DWORD_PTR nResult = 0;

	if (!argv) argc = 0;

	switch (cmd)
	{
	case dc_Init:
		if (!gpInet)
			gpInet = new CDownloader;
		nResult = (gpInet != NULL);
		break;
	case dc_Reset:
		if (gpInet)
			gpInet->CloseInternet(false);
		nResult = TRUE;
		break;
	case dc_Deinit:
		SafeDelete(gpInet);
		nResult = TRUE;
		break;
	case dc_SetProxy: // [0]="Server:Port", [1]="User", [2]="Password"
		if (gpInet)
		{
			gpInet->SetProxy(
				(argc > 0 && argv[0].argType == at_Str) ? argv[0].strArg : NULL,
				(argc > 1 && argv[1].argType == at_Str) ? argv[1].strArg : NULL,
				(argc > 2 && argv[2].argType == at_Str) ? argv[2].strArg : NULL);
			nResult = TRUE;
		}
		break;
	case dc_SetLogin: // [0]="User", [1]="Password"
		if (gpInet)
		{
			gpInet->SetLogin(
				(argc > 0 && argv[0].argType == at_Str) ? argv[0].strArg : NULL,
				(argc > 1 && argv[1].argType == at_Str) ? argv[1].strArg : NULL);
			nResult = TRUE;
		}
		break;
	case dc_ErrCallback: // [0]=FDownloadCallback, [1]=lParam
	case dc_ProgressCallback: // [0]=FDownloadCallback, [1]=lParam
	case dc_LogCallback: // [0]=FDownloadCallback, [1]=lParam
		if (gpInet)
		{
			gpInet->SetCallback(
				cmd,
				(argc > 0) ? (FDownloadCallback)argv[0].uintArg : 0,
				(argc > 1) ? argv[1].uintArg : 0);
			nResult = TRUE;
		}
		break;
	case dc_DownloadFile: // [0]="http", [1]="DestLocalFilePath", [2]=abShowErrors
		if (gpInet && (argc >= 2))
		{
			DWORD crc = 0, size = 0;
			nResult = gpInet->DownloadFile(
				(argc > 0 && argv[0].argType == at_Str) ? argv[0].strArg : NULL,
				(argc > 1 && argv[1].argType == at_Str) ? argv[1].strArg : NULL,
				crc, size,
				(argc > 2) ? argv[2].uintArg : TRUE);
			// Succeeded?
			if (nResult)
			{
				argv[0].uintArg = size;
				argv[1].uintArg = crc;
			}
		}
		break;
	case dc_DownloadData: // [0]="http" -- not implemented yet
		_ASSERTE(FALSE && "dc_DownloadData not implemented yet");
		break;
	case dc_RequestTerminate:
		if (gpInet)
		{
			gpInet->RequestTerminate();
			nResult = TRUE;
		}
		break;
	case dc_SetAsync:
		if (gpInet && (argc > 0) && (argv[0].argType == at_Uint))
		{
			gpInet->SetAsync(argv[0].uintArg != 0);
			nResult = TRUE;
		}
		break;
	case dc_SetTimeout:
		if (gpInet && (argc > 1) && (argv[0].argType == at_Uint) && (argv[1].argType == at_Uint))
		{
			gpInet->SetTimeout(argv[0].uintArg == 0, argv[1].uintArg);
			nResult = TRUE;
		}
		break;

	#ifdef _DEBUG
	default:
		_ASSERTE(FALSE && "Unsupported command!");
	#endif
	}

	return nResult;
}