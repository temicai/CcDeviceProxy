#ifndef DEVICE_RECEIVER_H_
#define DEVICE_RECEIVER_H_

#include "json.hpp"
#include "pf_log.h"
#include "zmq.h"
#include "czmq.h"
#include "escort_common.hpp"
#include "tcp_server.h"
#include "HttpQuery.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <queue>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <condition_variable>

#define BEGIN_TOKEN 0x5b
#define END_TOKEN 0x5d

namespace ccdp
{
	enum eLinkState
	{
		E_LS_OFFLINE = 0,
		E_LS_ONLINE = 1,
	};

	enum eLocateType
	{
		E_GPS = 1,
		E_LBS = 2,
	};

	enum 
	{
		E_HISTORY = 0,
		E_REALTIME = 1,
	};


	typedef struct tagLocateInfo
	{
		char szDeviceId[24];
		unsigned long long ullLocateTime;
		int nLocateType; //1:gps,2:lbs
		unsigned short usLatType;
		unsigned short usLngType;
		double dLatitude;
		double dLongitude;
		double dSpeed;
		double dDirection;
		int nElevation;
		int nGpsStatelliteCount;
		int nSignalIntensity;
		int nBattery;
		int nStatus;
		int nGsmDelay;
		int nNationCode;
		int nNetCode;
		int nBaseStationCount;
		int nDetectedWifiCount;
		escort::BaseStation * pBaseStationList;
		escort::Wifi * pDetectedWifiList;
		tagLocateInfo() noexcept
		{
			memset(szDeviceId, 0, sizeof(szDeviceId));
			ullLocateTime = 0L;
			nLocateType = 0;
			dLatitude = dLongitude = 0.0000f;
			usLatType = usLngType = 1;
			dSpeed = 0.0000;
			dDirection = 0.00;
			nElevation = nGpsStatelliteCount = 0;
			nSignalIntensity = nBattery = nStatus = nBaseStationCount = 0;
			nGsmDelay = nDetectedWifiCount = 0;
			nNationCode = 460;
			nNetCode = 0;
			pBaseStationList = NULL;
			pDetectedWifiList = NULL;
		}
	} LocateInfo;

	typedef struct tagDeviceInfo
	{
		char deviceId[24];
		char link[32];
		int loose; //0:normal,1:loose
		int online; //0:offline,1:online
		int battery;
		unsigned long long activeTime;
	} DeviceInfo;
}

class CcDeviceProxy
{
public:
	CcDeviceProxy(const char * pRoot, unsigned short usLogType);
	~CcDeviceProxy();
	int Start(const char * host, unsigned short recvPort, const char * msgIp , unsigned short repPort, 
		unsigned short usMsgEncrypt, unsigned short usLbsQry);
	int Stop();
	void SetLogType(unsigned short logType);
	void SetLbsQryKey(const char * key);
	void SetDeviceInterval(unsigned short, unsigned short, unsigned short); //unit: second

private:
	unsigned long long m_ullSrvInst;
	int m_nRun;
	unsigned short m_usRecvPort;
	unsigned short m_usIdelInterval;
	unsigned short m_usNormalInterval;
	unsigned short m_usEmergInterval;

	zloop_t * m_loop;
	unsigned int m_nTimerTickCount;
	std::thread m_thdSupervise;
	zsock_t * m_pipeline;
	std::mutex m_mutex4Pipeline;
	
	unsigned short m_usMsgEncrypt;
	unsigned short m_usLbsQryType; //0:false,1:true
	char m_szLbsQryKey[128] = { 0 };

	unsigned short m_usLogType;
	unsigned long long m_ullLogInst;
	

	char m_szHost[32];
	std::mutex m_mutex4DevMsgQue;
	std::condition_variable m_cond4DevMsgQue;
	std::queue<MessageContent *> m_devMsgQue;
	std::thread m_thdDealDevMsg;
	
	std::mutex m_mutex4LinkList;
	typedef std::map<std::string, std::string> DeviceLinkList;
	DeviceLinkList m_devLinkList;
	std::mutex m_mutex4DevList;
	typedef std::map<std::string, ccdp::DeviceInfo *> DeviceList;
	DeviceList m_devList;

	std::mutex m_mutex4PipeMsgQue;
	std::condition_variable m_cond4PipeMsgQue;
	std::queue<escort::DeviceInteractMessage *> m_pipeMsgQue;
	std::thread m_thdDealPipeMsg;

	std::queue<std::string> m_disLinkQue;
	std::mutex m_mutex4DisLinkQue;
	std::condition_variable m_cond4DisLinkQue;
	std::thread m_thdDisLink;

	unsigned int m_uiRepSeq;
	std::mutex m_mutex4RepSeq;

protected:
	void initLog(const char * pDir);
	bool addDeviceMsg(MessageContent * pMsg);
	void dealDeviceMsg();
	void parseDeviceMsg(MessageContent * pMsg);
	int getWholeMessage(const unsigned char * pData, unsigned int uiDataLen, unsigned int uiIndex,
		unsigned int & uiBeginIndex, unsigned int & uiEndIndex);
	void splitString(std::string strSource, std::string strDelimiter, std::vector<std::string> & strList);
	void splitString2(std::string strSrource, std::string strDelimiter, std::vector<std::string> & strList);
	void handleLink(const char *pLink);
	void handleDisLink(const char * pLink);
	void updateDevice(const char * pLink, const char * pDeviceId);
	void updateDeviceBattery(const char * pLink, const char * pDeviceId, unsigned short battery);
	void reportDeviceOnline(const char * pDeviceId);
	void handleLocate(ccdp::LocateInfo * pLocateInfo, int realtime = 1);
	void handleAlarm(ccdp::LocateInfo * pLocateInfo);
	int analyzeDeviceStatus(int status);

	bool addPipeMsg(escort::DeviceInteractMessage *);
	void handlePipeMsg();
	void handlePipeDeviceConfig(escort::DeviceConfigInformation *, unsigned int seq);
	
	void checkDeviceLink();
	std::string utf8ToAnsi(LPCSTR utf8);
	void clearDeviceList();
	void clearLinkList();

	unsigned int getNextSequence();
	void sendMsgByPipe(const char * pMsg, unsigned short msgType);
	void encryptMessage(unsigned char * pData, unsigned int begin, unsigned int end, unsigned short key);
	void decryptMessage(unsigned char * pData, unsigned int begin, unsigned int end, unsigned short key);
	void formatDatetime(unsigned long long, char *, size_t);
	unsigned long long makeDatetime(const char * pDatetime);
	void addDisLink(std::string);
	void dealDisLink();

	friend int readPipeline(zloop_t *loop, zsock_t *reader, void *arg);
	friend void dealDevMsgThread(void *);
	friend void __stdcall fMsgCb(int nType, void * pMsg, void * pUserData);
	friend void superviseThread(void *);
	friend void dealPipeMsgThread(void *);
	friend int timerCb(zloop_t * loop, int timer_id, void * arg);
	friend void dealDisLinkThread(void *);
};



#endif
