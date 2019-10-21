#ifndef ESCORT_CC_DEVICE_PROXY_H
#define ESCORT_CC_DEVICE_PROXY_H

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct ccdp_parameter_list
	{
		char proxyHost[32];
		char msgIp[32];
		unsigned short recvPort;
		unsigned short repPort;
		unsigned short logType;
		unsigned short msgEncrypt;
		unsigned short qryLbsType;
		char qryLbsKey[128];
	} CcDeviceProxyParameterList;

	unsigned long long __stdcall CCDP_Start(CcDeviceProxyParameterList paramList);
	int __stdcall CCDP_Stop(unsigned long long instance);
	int __stdcall CCDP_SetLogType(unsigned long long instance, unsigned short);

#ifdef __cplusplus
}
#endif

#endif 
