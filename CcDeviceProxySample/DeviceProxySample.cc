#include "EscortCcDevice.h"
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <Windows.h>
#include <stdio.h>

typedef std::map<std::string, std::string> KVStringPair;

bool loadCfg(const char * pFileName_, KVStringPair & kvList_)
{
	bool result = false;
	char szLine[256] = { 0 };
	std::fstream cfgFile;
	cfgFile.open(pFileName_, std::ios::in);
	if (cfgFile.is_open()) {
		while (!cfgFile.eof()) {
			cfgFile.getline(szLine, sizeof(szLine), '\n');
			std::string str = szLine;
			if (str[0] == '#') {
				continue;
			}
			size_t n = str.find_first_of('=');
			if (n != std::string::npos) {
				std::string keyStr = str.substr(0, n);
				std::string valueStr = str.substr(n + 1);
				kvList_.emplace(keyStr, valueStr);
				result = true;
			}
		}
	}
	cfgFile.close();
	return result;
}

char * readItem(KVStringPair kvList_, const char * pItem_)
{
	if (!kvList_.empty()) {
		if (pItem_ && strlen(pItem_)) {
			KVStringPair::iterator iter = kvList_.find(pItem_);
			if (iter != kvList_.end()) {
				std::string strValue = iter->second;
				size_t nSize = strValue.size();
				if (nSize) {
					char * value = new char [nSize + 1];
					memset(value, 0, nSize + 1);
					if (value) {
						strcpy_s(value, nSize + 1, strValue.c_str());
						value[nSize] = '\0';
						return value;
					}
				}
			}
		}
	}
	return NULL;
}

int main(int argc, char ** argv)
{
	char szCfgFileName[256] = { 0 };
	if (argc > 1) {
		strcpy_s(szCfgFileName, sizeof(szCfgFileName), argv[1]);
	}
	else {
		char szExePath[256] = { 0 };
		GetModuleFileNameA(NULL, szExePath, sizeof(szExePath));
		char szDrive[32] = { 0 };
		char szDir[256] = { 0 };
		_splitpath_s(szExePath, szDrive, sizeof(szDrive), szDir, sizeof(szDir), NULL, 0, NULL, 0);
		sprintf_s(szCfgFileName, sizeof(szCfgFileName), "%s%sconf\\device.data", szDrive, szDir);
	}
	char szProxyHost[32] = { 0 };
	char szMsgIp[32] = { 0 };
	char szLbsQryKey[128] = { 0 };
	char szLbsQryDomain[128] = { 0 };
	unsigned short usRecvPort = 0;
	unsigned short usRepPort = 0;
	unsigned short usLogType = 0;
	unsigned short usLbsQryType = 0;
	unsigned short usMsgEncrypt = 0;
	KVStringPair kvPair;
	if (loadCfg(szCfgFileName, kvPair)) {
		char * host = readItem(kvPair, "proxy_host");
		if (host) {
			strcpy_s(szProxyHost, sizeof(szProxyHost), host);
			delete[] host;
			host = NULL;
		}
		char * device_recv_port = readItem(kvPair, "device_port");
		if (device_recv_port) {
			usRecvPort = (unsigned short)strtol(device_recv_port, NULL, 10);
			delete[] device_recv_port;
			device_recv_port = NULL;
		}
		char * message_ip = readItem(kvPair, "message_ip");
		if (message_ip) {
			strcpy_s(szMsgIp, sizeof(szMsgIp), message_ip);
			delete [] message_ip;
			message_ip = NULL;
		}
		char * report_port = readItem(kvPair, "report_port");
		if (report_port) {
			usRepPort = (unsigned short)strtol(report_port, NULL, 10);
			delete[] report_port;
			report_port = NULL;
		}
		char * log_type = readItem(kvPair, "log_type");
		if (log_type) {
			usLogType = (unsigned short)strtol(log_type, NULL, 10);
			delete[] log_type;
			log_type = NULL;
		}
		char * lbs_query = readItem(kvPair, "lbs_query");
		if (lbs_query) {
			usLbsQryType = (unsigned short)strtol(lbs_query, NULL, 10);
			delete[] lbs_query;
			lbs_query = NULL;
		}
		char * lbs_key = readItem(kvPair, "lbs_key");
		if (lbs_key) {
			strcpy_s(szLbsQryKey, sizeof(szLbsQryKey), lbs_key);
			delete[] lbs_key;
			lbs_key = NULL;
		}
		char * report_encrypt = readItem(kvPair, "report_encrypt");
		if (report_encrypt) {
			usMsgEncrypt = (unsigned short)strtol(report_encrypt, NULL, 10);
			delete [] report_encrypt;
			report_encrypt = NULL;
		}
		char* lbs_domain = readItem(kvPair, "lbs_domain");
		if (lbs_domain) {
			strcpy_s(szLbsQryDomain, sizeof(szLbsQryDomain), lbs_domain);
			delete[] lbs_domain;
			lbs_domain = NULL;
		}
		CcDeviceProxyParameterList paramList;
		memset(&paramList, 0, sizeof(CcDeviceProxyParameterList));
		paramList.logType = usLogType;
		paramList.msgEncrypt = usMsgEncrypt;
		paramList.qryLbsType = usLbsQryType;
		paramList.recvPort = usRecvPort;
		paramList.repPort = usRepPort;
		strcpy_s(paramList.proxyHost, sizeof(paramList.proxyHost), szProxyHost);
		strcpy_s(paramList.qryLbsKey, sizeof(paramList.qryLbsKey), szLbsQryKey);
		strcpy_s(paramList.msgIp, sizeof(paramList.msgIp), szMsgIp);
		strcpy_s(paramList.qryLbsDomain, sizeof(paramList.qryLbsDomain), szLbsQryDomain);
		unsigned long long ullInst = CCDP_Start(paramList);
		if (ullInst) {
			printf("start device proxy [%s:%hu] ok, connect to message service [%s:%hu]\n",
				szProxyHost, usRecvPort, szMsgIp, usRepPort);
			printf("message encrypt: %hu, log type: %hu\n", usMsgEncrypt, usLogType);
			printf("open LBS query: %hu, query key: %s\n", usLbsQryType, szLbsQryKey);
			while (1) {
#ifdef _DEBUG
				char szLine[64] = { 0 };
				std::fgets(szLine, sizeof(szLine), stdin);
				if (strncmp(szLine, "q\n", 2) == 0) {
					break;
				}
#endif
				Sleep(1000);
			}
			CCDP_Stop(ullInst);
			printf("stop device proxy\n");
		}
		else {
			printf("start device proxy [%s:%hu] failed\n", szProxyHost, usRecvPort);
		}
	}
	else {
		printf("load config file: %s failed\n", szCfgFileName);
	}
	return 0;
}