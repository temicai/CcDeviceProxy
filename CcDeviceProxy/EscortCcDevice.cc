#include "EscortCcDevice.h"
#include "CcDeviceProxy.hpp"
#include <windows.h>
#include <map>

static char g_szDllDir[256] = { 0 };
typedef std::map<unsigned long long, CcDeviceProxy *> DeviceProxyList;
DeviceProxyList g_proxyList;
std::mutex g_mutex4ProxyList;


BOOL APIENTRY DllMain(void * hInst, unsigned long ulReason, void * pReserved)
{
	switch (ulReason) {
		case DLL_PROCESS_ATTACH: {
			g_szDllDir[0] = '\0';
			char szPath[256] = { 0 };
			if (GetModuleFileNameA((HMODULE)hInst, szPath, sizeof(szPath)) != 0) {
				char drive[32] = { 0 };
				char dir[256] = { 0 };
				_splitpath_s(szPath, drive, 32, dir, 256, NULL, 0, NULL, 0);
				snprintf(g_szDllDir, sizeof(g_szDllDir), "%s%s", drive, dir);
			}
			break;
		}
		case DLL_PROCESS_DETACH: {
			std::lock_guard<std::mutex> lk(g_mutex4ProxyList);
			if (!g_proxyList.empty()) {
				DeviceProxyList::iterator iter = g_proxyList.begin();
				do {
					CcDeviceProxy * pProxy = iter->second;
					if (pProxy) {
						pProxy->Stop();
						delete pProxy;
						pProxy = NULL;
					}
					iter = g_proxyList.erase(iter);
				} while (iter != g_proxyList.end());
			}
			break;
		}
		case DLL_THREAD_ATTACH: {
			break;
		}
		case DLL_THREAD_DETACH: {
			break;
		}
	}
	return TRUE;
}

unsigned long long __stdcall CCDP_Start(CcDeviceProxyParameterList paramList_)
{
	unsigned long long result = 0;
	CcDeviceProxy * pProxy = new CcDeviceProxy(g_szDllDir, paramList_.logType);
	if (pProxy) {
		if (pProxy->Start(paramList_.proxyHost, paramList_.recvPort, paramList_.msgIp, paramList_.repPort, 
			paramList_.msgEncrypt, paramList_.qryLbsType) == 0) {
			pProxy->SetLbsQryKey(paramList_.qryLbsKey);
			if (strlen(paramList_.qryLbsDomain)) {
				pProxy->SetLbsQryDomain(paramList_.qryLbsDomain);
			}
			//pProxy->SetLogType(paramList_.logType);
			unsigned long long ullInst = (unsigned long long)pProxy;
			std::lock_guard<std::mutex> lk(g_mutex4ProxyList);
			g_proxyList.emplace(ullInst, pProxy);
			result = ullInst;
		}
		else {
			delete pProxy;
			pProxy = NULL;
		}
	}
	return result;
}

int __stdcall CCDP_Stop(unsigned long long ullInst_)
{
	int result = -1;
	if (ullInst_ > 0) {
		std::lock_guard<std::mutex> lk(g_mutex4ProxyList);
		DeviceProxyList::iterator iter = g_proxyList.find(ullInst_);
		if (iter != g_proxyList.end()) {
			CcDeviceProxy * pProxy = iter->second;
			if (pProxy) {
				pProxy->Stop();
				delete pProxy;
				pProxy = NULL;
			}
			g_proxyList.erase(iter);
			result = 0;
		}
	}
	return result;
}

int __stdcall CCDP_SetLogType(unsigned long long ullInst_, unsigned short usLogType_)
{
	int result = -1;
	if (ullInst_ > 0) {
		std::lock_guard<std::mutex> lk(g_mutex4ProxyList);
		DeviceProxyList::iterator iter = g_proxyList.find(ullInst_);
		if (iter != g_proxyList.end()) {
			CcDeviceProxy * pProxy = iter->second;
			if (pProxy) {
				pProxy->SetLogType(usLogType_);
				result = 0;
			}
		}
	}
	return result;
}


