#include "CcDeviceProxy.hpp"

void dealDevMsgThread(int index_, void * param_)
{
	CcDeviceProxy * pService = (CcDeviceProxy *)param_;
	if (pService) {
		pService->dealDeviceMsg(index_);
	}
}

int readPipeline(zloop_t * loop_, zsock_t * reader_, void * param_)
{
	auto pProxy = (CcDeviceProxy *)param_;
	if (pProxy) {
		if (pProxy->m_nRun) {
			zmsg_t * msg = zmsg_new();
			zsock_recv(reader_, "m", &msg);
			if (msg) {
				if (zmsg_size(msg) >= 3) {
					zframe_t * frame_type = zmsg_pop(msg);
					zframe_t * frame_encrypt = zmsg_pop(msg);
					zframe_t * frame_data = zmsg_pop(msg);
					char szMsgType[16] = { 0 };
					char szMsgEncrypt[16] = { 0 };
					memcpy_s(szMsgType, sizeof(szMsgType), zframe_data(frame_type), zframe_size(frame_type));
					memcpy_s(szMsgEncrypt, sizeof(szMsgEncrypt), zframe_data(frame_encrypt), zframe_size(frame_encrypt));
					unsigned int nDataSize = (unsigned int)zframe_size(frame_data);
					unsigned char * pData = new unsigned char[nDataSize + 1];
					memcpy_s(pData, nDataSize + 1, zframe_data(frame_data), nDataSize);
					pData[nDataSize] = '\0';

					escort::DeviceInteractMessage * pMsg = new escort::DeviceInteractMessage();
					memset(pMsg, 0, sizeof(escort::DeviceInteractMessage));

					pMsg->msgType = (unsigned short)strtol(szMsgType, NULL, 10);
					pMsg->msgEncrypt = (unsigned short)strtol(szMsgEncrypt, NULL, 10);
					pMsg->msgSize = nDataSize;
					pMsg->msgContent = new char[pMsg->msgSize + 1];
					if (pMsg->msgEncrypt > 0) {
						pProxy->decryptMessage(pData, 0, nDataSize, pMsg->msgEncrypt);
					}
					memcpy_s(pMsg->msgContent, nDataSize + 1, pData, nDataSize);
					pMsg->msgContent[nDataSize] = '\0';

					if (!pProxy->addPipeMsg(pMsg)) {
						if (pMsg) {
							if (pMsg->msgContent && pMsg->msgSize) {
								delete[] pMsg->msgContent;
								pMsg->msgContent = NULL;
								pMsg->msgSize = 0;
							}
							delete pMsg;
							pMsg = NULL;
						}
					}

					zframe_destroy(&frame_type);
					zframe_destroy(&frame_encrypt);
					zframe_destroy(&frame_data);
					delete[] pData;
					pData = NULL;
				}
				zmsg_destroy(&msg);
			}
		}
	}
	return 0;
}

void __stdcall fMsgCb(int nType_, void * pMsg_, void * pUserData_)
{
	CcDeviceProxy * pService = (CcDeviceProxy *)pUserData_;
	if (pService) {
		switch (nType_) {
			case MSG_LINK_CONNECT: {
				const char * pStrLink = (char *)pMsg_;
				pService->handleLink(pStrLink);
				break;
			}
			case MSG_LINK_DISCONNECT: {
				const char * pStrLink = (char *)pMsg_;
				pService->handleDisLink(pStrLink);
				break;
			}
			case MSG_DATA: {
				MessageContent * pMsgCtx = (MessageContent *)pMsg_;
				MessageContent * pMsgCtxCopy = new MessageContent();
				size_t nSize = sizeof(MessageContent);
				memset(pMsgCtxCopy, 0, nSize);
				strcpy_s(pMsgCtxCopy->szEndPoint, sizeof(pMsgCtxCopy->szEndPoint), pMsgCtx->szEndPoint);
				pMsgCtxCopy->uiMsgDataLen = pMsgCtx->uiMsgDataLen;
				pMsgCtxCopy->ulMsgTime = pMsgCtx->ulMsgTime;
				if (pMsgCtxCopy->uiMsgDataLen) {
					pMsgCtxCopy->pMsgData = new unsigned char[pMsgCtxCopy->uiMsgDataLen + 1];
					memcpy_s(pMsgCtxCopy->pMsgData, pMsgCtxCopy->uiMsgDataLen, pMsgCtx->pMsgData, pMsgCtx->uiMsgDataLen);
					pMsgCtxCopy->pMsgData[pMsgCtxCopy->uiMsgDataLen] = '\0';
				}
				if (!pService->addDeviceMsg(pMsgCtxCopy)) {
					if (pMsgCtxCopy->pMsgData) {
						delete[] pMsgCtxCopy->pMsgData;
						pMsgCtxCopy->pMsgData = NULL;
					}
					delete pMsgCtxCopy;
					pMsgCtxCopy = NULL;
				}
				break;
			}
		}
	}
}

void superviseThread(void * param_)
{
	CcDeviceProxy * pService = (CcDeviceProxy *)param_;
	if (pService) {
		zloop_start(pService->m_loop);
	}
}

void dealPipeMsgThread(void * param_)
{
	CcDeviceProxy * pProxy = (CcDeviceProxy *)param_;
	if (pProxy) {
		pProxy->handlePipeMsg();
	}
}

int timerCb(zloop_t * loop_, int timerId_, void * arg_)
{
	int result = 0;
	CcDeviceProxy * pProxy = (CcDeviceProxy *)arg_;
	if (pProxy) {
		if (!pProxy->m_nRun) {
			zloop_reader_end(loop_, pProxy->m_pipeline);
			zloop_timer_end(loop_, timerId_);
			result = -1;
		}
		else {
			if (pProxy->m_nTimerTickCount % 15 == 0) {
				char szMsg[256] = { 0 };
				sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"datetime\":%llu}", pProxy->getNextSequence(), time(NULL));
				pProxy->sendMsgByPipe(szMsg, escort::MSG_DEV_ALIVE);
			}
			if (pProxy->m_nTimerTickCount % 30 == 0) { //1 min
				pProxy->checkDeviceLink();
			}
			pProxy->m_nTimerTickCount++;
			if (pProxy->m_nTimerTickCount == 72001) {
				pProxy->m_nTimerTickCount = 1;
			}
			Sleep(100);
		}
	}
	return result;
}

void dealDisLinkThread(void * param_)
{
	CcDeviceProxy * pProxy = (CcDeviceProxy *)param_;
	if (pProxy) {
		pProxy->dealDisLink();
	}
}

CcDeviceProxy::CcDeviceProxy(const char * pDllRoot_, unsigned short usLogType_)
{
	srand((unsigned int)time(NULL));
	zsys_init();
	
	m_nRun = 0;
	m_usRecvPort = 0;

	m_pipeline = NULL;
	memset(m_szHost, 0, sizeof(m_szHost));

	m_ullLogInst = 0;
	m_usLogType = usLogType_;

	m_usEmergInterval = 10;
	m_usNormalInterval = 30;
	m_usIdelInterval = 60;

	m_ullSrvInst = 0;
	m_loop = NULL;
	m_nTimerTickCount = 0;
	initLog(pDllRoot_);
}

CcDeviceProxy::~CcDeviceProxy()
{
	if (m_nRun) {
		Stop();
	}
	if (m_ullLogInst) {
		LOG_Release(m_ullLogInst);
		m_ullLogInst = 0;
	}
	zsys_shutdown();
}

int CcDeviceProxy::Start(const char * pHost_, unsigned short usDevPort_, const char * pMidwareHost_, 
	unsigned short usRepPort_, unsigned short usMsgEncrypt_, unsigned short usLbsQry_)
{
	if (m_nRun) {
		return 0;
	}
	unsigned long long ullSrvInst = TS_StartServer(usDevPort_, fMsgCb, this, 480);
	if (ullSrvInst > 0) {
		m_ullSrvInst = ullSrvInst;
		m_usRecvPort = usDevPort_;
		m_nRun = 1;
		m_pipeline = zsock_new(ZMQ_DEALER);
		char szUuid[64] = { 0 };
		sprintf_s(szUuid, sizeof(szUuid), "devproxy-%s-%04x-01-%04x", pHost_, usDevPort_, (rand() % 0xffff));
		zsock_set_identity(m_pipeline, szUuid);
		zsock_connect(m_pipeline, "tcp://%s:%u", pMidwareHost_, usRepPort_); 
		m_nTimerTickCount = 0;
		m_loop = zloop_new();
		zloop_reader(m_loop, m_pipeline, readPipeline, this);
		zloop_reader_set_tolerant(m_loop, m_pipeline);
		zloop_timer(m_loop, 2000, 0, timerCb, this);
		if (usMsgEncrypt_ > 0) {
			m_usMsgEncrypt = escort::DEV_ENC_CODE;
		}
		else {
			m_usMsgEncrypt = 0;
		}
		m_usLbsQryType = usLbsQry_;
		strcpy_s(m_szLbsDomain, sizeof(m_szLbsDomain), DEFAULT_LBS_DOMAIN);

		for (int i = 0; i < 4; i++) {
			m_thdDealDevMsg[i] = std::thread(dealDevMsgThread, i, this);
		}

		m_thdSupervise = std::thread(superviseThread, this);
		m_thdDealPipeMsg = std::thread(dealPipeMsgThread, this);
		m_thdDisLink = std::thread(dealDisLinkThread, this);

		char szLog[256] = { 0 };
		sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]start device receiver service at %u\n", 
			__FUNCTION__, __LINE__, m_usRecvPort);
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);

		return 0;
	}

	return -1;
}

int CcDeviceProxy::Stop()
{
	if (m_nRun) {
		char szLog[256] = { 0 };
		snprintf(szLog, sizeof(szLog), "[device]%s[%d]stop receiver service\n", __FUNCTION__, __LINE__);
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
		m_nRun = 0;
		if (m_ullSrvInst) {
			TS_StopServer(m_ullSrvInst);
			m_ullSrvInst = 0;
		}
		m_cond4DevMsgQue.notify_all();
		for (int i = 0; i < 4; i++) {
			m_thdDealDevMsg[i].join();
		}

		//if (m_thdDealDevMsg.joinable()) {
		//		m_cond4DevMsgQue.notify_all();
		//	m_thdDealDevMsg.join();
		//}
		if (m_thdSupervise.joinable()) {
			m_thdSupervise.join();
		}
		if (m_thdDealPipeMsg.joinable()) {
			m_cond4PipeMsgQue.notify_all();
			m_thdDealPipeMsg.join();
		}
		if (m_thdDisLink.joinable()) {
			m_cond4DisLinkQue.notify_all();
			m_thdDisLink.join();
		}
		if (m_loop) {
			zloop_destroy(&m_loop);
		}
		if (m_pipeline) {
			zsock_destroy(&m_pipeline);
		}
		clearLinkList();
		clearDeviceList();
	}
	return 0;
}

void CcDeviceProxy::SetLogType(unsigned short usLogType_)
{
	if (m_ullLogInst) {
		if (m_usLogType != usLogType_) {
			pf_logger::LogConfig logConf;
			LOG_GetConfig(m_ullLogInst, &logConf);
			if (logConf.usLogType != usLogType_) {
				logConf.usLogType = usLogType_;
				LOG_SetConfig(m_ullLogInst, logConf);
			}
			m_usLogType = usLogType_;
		}
	}
}

void CcDeviceProxy::SetLbsQryKey(const char * pKey_)
{
	strcpy_s(m_szLbsQryKey, sizeof(m_szLbsQryKey), pKey_);
}

void CcDeviceProxy::SetLbsQryDomain(const char * pDomain_)
{
	strcpy_s(m_szLbsDomain, sizeof(m_szLbsDomain), pDomain_);
}

void CcDeviceProxy::SetDeviceInterval(unsigned short usIdleInterval_, unsigned short usNormalInterval_,
	unsigned short usEmergInterval_)
{
	if (usEmergInterval_ < 10) {
		m_usEmergInterval = 10;
	}
	else if (usEmergInterval_ >= 60) {
		m_usEmergInterval = 30;
	}
	else {
		m_usEmergInterval = (usEmergInterval_ / 10) * 10;
	}
	if (usNormalInterval_ < 10) {
		m_usNormalInterval = 10;
	}
	else if (usNormalInterval_ > 90) {
		m_usNormalInterval = 90;
	}
	else {
		m_usNormalInterval = (usNormalInterval_ / 10) * 10;
	}
	if (usIdleInterval_ < 50) {
		m_usIdelInterval = 50;
	}
	else if (usIdleInterval_ > 300) {
		m_usIdelInterval = 300;
	}
	else {
		m_usIdelInterval = (usIdleInterval_ / 10) * 10;
	}
}

void CcDeviceProxy::initLog(const char * pLogRoot_)
{
	if (m_ullLogInst == 0) {
		m_ullLogInst = LOG_Init();
		if (m_ullLogInst) {
			pf_logger::LogConfig logConf;
			logConf.usLogType = m_usLogType;
			logConf.usLogPriority = pf_logger::eLOGPRIO_ALL;
			char szLogDir[256] = { 0 };
			sprintf_s(szLogDir, 256, "%slog\\", pLogRoot_);
			CreateDirectoryExA(".\\log\\", szLogDir, NULL);
			strcat_s(szLogDir, 256, "cc_device\\");
			CreateDirectoryExA(".\\log\\cc_device", szLogDir, NULL);
			strcpy_s(logConf.szLogPath, sizeof(logConf.szLogPath), szLogDir);
			LOG_SetConfig(m_ullLogInst, logConf);
		}
	}
}

bool CcDeviceProxy::addDeviceMsg(MessageContent * pMsg_)
{
	bool result = false;
	if (pMsg_ && pMsg_->pMsgData && pMsg_->uiMsgDataLen && strlen(pMsg_->szEndPoint)) {
		std::lock_guard<std::mutex> lk(m_mutex4DevMsgQue);
		m_devMsgQue.push(pMsg_);
		if (m_devMsgQue.size() == 1) {
			m_cond4DevMsgQue.notify_all();
		}
		char szLog[1024] = { 0 };
		sprintf_s(szLog, sizeof(szLog), "%s[%u]from %s, data=%s, queue size=%zd\n", 
			__FUNCTION__, __LINE__, pMsg_->szEndPoint, pMsg_->pMsgData, m_devMsgQue.size());
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
		result = true;
	}
	return result;
}

void CcDeviceProxy::dealDeviceMsg(int index_)
{
	do {
		std::unique_lock<std::mutex> lk(m_mutex4DevMsgQue);
		m_cond4DevMsgQue.wait(lk, [&] {
			return (!m_nRun || !m_devMsgQue.empty());
		});
		if (!m_nRun && m_devMsgQue.empty()) {
			break;
		}
		MessageContent * pMsg = m_devMsgQue.front();
		m_devMsgQue.pop();
		lk.unlock();
		if (pMsg) {
			if (pMsg->pMsgData && pMsg->uiMsgDataLen) {
				size_t nLogLen = pMsg->uiMsgDataLen + 256;
				char * pLog = new char[nLogLen];
				memset(pLog, 0, nLogLen);
				sprintf_s(pLog, nLogLen, "%s[%d], %d, recv from %s, len=%u, %s\n", __FUNCTION__, __LINE__,
					index_, pMsg->szEndPoint, pMsg->uiMsgDataLen, (char *)pMsg->pMsgData);
				LOG_Log(m_ullLogInst, pLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
				parseDeviceMsg(pMsg);
				delete[] pMsg->pMsgData;
				pMsg->pMsgData = NULL;
				pMsg->uiMsgDataLen = 0;
				delete[] pLog;
				pLog = NULL;
			}
			delete pMsg;
			pMsg = NULL;
		}
	} while (1);
}

void CcDeviceProxy::parseDeviceMsg(MessageContent * pMsg_)
{
	char szLog[1024] = { 0 };
	if (pMsg_) {
		std::string strEndPoint = pMsg_->szEndPoint;
		unsigned char * pData = pMsg_->pMsgData;
		unsigned int uiDataLen = pMsg_->uiMsgDataLen;
		if (pData && uiDataLen) {
			unsigned int uiIndex = 0;
			unsigned int uiDataCellBeginIndex = 0;
			unsigned int uiDataCellEndIndex = 0;
			unsigned int uiUnitLen = 0;
			do {
				int rc = getWholeMessage(pData, uiDataLen, uiIndex, uiDataCellBeginIndex, uiDataCellEndIndex);
				if (rc == 0) {
					break;
				}
				else if (rc == 1) {
					break;
				}
				else if (rc == 2) {
					uiUnitLen = uiDataCellEndIndex - uiDataCellBeginIndex - 1;
					uiIndex = uiDataCellEndIndex + 1;
					auto pBuf = new unsigned char[uiUnitLen + 1];
					memcpy_s(pBuf, uiUnitLen + 1, pData + uiDataCellBeginIndex + 1, uiUnitLen);
					pBuf[uiUnitLen] = '\0';
					unsigned long long now = time(NULL);
					std::vector<std::string> strList;
					std::string strCell = utf8ToAnsi((char *)pBuf);
					sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]receive from=%s, data=%s\n", 
						__FUNCTION__, __LINE__, pMsg_->szEndPoint, strCell.c_str());
					LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
					splitString2(strCell, "*", strList);
					strCell.clear();
					std::string strDeviceId;
					std::string strContentLength;
					std::string strContent;
					if (strList.size() >= 4) {
						strDeviceId = strList[1];
						updateDevice(pMsg_->szEndPoint, strDeviceId.c_str());
						strContentLength = strList[2];
						strContent = strList[3];
						int nContentLen = 0;
						sscanf_s(strContentLength.c_str(), "%x", &nContentLen);
						std::vector<std::string> strContentList;
						splitString2(strContent, ",", strContentList);
						if (!strContentList.empty()) {
							if (strcmp(strContentList[0].c_str(), "LK") == 0) {
								char szReply[256] = { 0 };
								snprintf(szReply, sizeof(szReply), "[SG*%s*0002*LK]", strDeviceId.c_str());
								TS_SendData(m_ullSrvInst, pMsg_->szEndPoint, szReply, (unsigned int)strlen(szReply));
								size_t nListCount = strContentList.size();
								if (nListCount == 4) {
									std::string strBattery = strContentList[3];
									unsigned short battery = 0;
									if (strcmp(strBattery.c_str(), "null") != 0) {
										battery = (unsigned short)strtol(strBattery.c_str(), NULL, 10);
									}
									updateDeviceBattery(pMsg_->szEndPoint, strDeviceId.c_str(), battery);		
									strBattery.clear();
								}
							}
							else if (strcmp(strContentList[0].c_str(), "UD") == 0) {
								const size_t nListCount = strContentList.size();
								if (nListCount >= 20) {
									ccdp::LocateInfo locateInfo;
									strcpy_s(locateInfo.szDeviceId, sizeof(locateInfo.szDeviceId), strDeviceId.c_str());
									std::string strDate = strContentList[1];
									std::string strTime = strContentList[2];
									unsigned short usDay = 0; 
									unsigned short usMonth = 0; 
									unsigned short usYear = 0;
									unsigned short usHour = 0;
									unsigned short usMinute = 0;
									unsigned short usSecond = 0;
									if (!strDate.empty() && !strTime.empty()) {
										sscanf_s(strDate.c_str(), "%02hu%02hu%02hu", &usDay, &usMonth, &usYear);
										usYear += 2000;
										sscanf_s(strTime.c_str(), "%02hu%02hu%02hu", &usHour, &usMinute, &usSecond);
										usHour += 8;
										if (usHour >= 24) {
											usHour -= 24;
											usDay += 1;
										}
										char szLocateTime[20] = { 0 };
										sprintf_s(szLocateTime, sizeof(szLocateTime), "%04d%02d%02d%02d%02d%02d", usYear, usMonth, usDay,
											usHour, usMinute, usSecond);
										locateInfo.ullLocateTime = makeDatetime(szLocateTime);
										if (locateInfo.ullLocateTime > now) {
											if (locateInfo.ullLocateTime - now > 600) {
												locateInfo.ullLocateTime = now;
											}
										}
										else if (locateInfo.ullLocateTime < now) {
											if (now - locateInfo.ullLocateTime > 300) {
												locateInfo.ullLocateTime = now;
											}
										}
									}
									std::string strLocateFlag = strContentList[3];
									if (strLocateFlag == "A" || strLocateFlag == "a") {
										locateInfo.nLocateType = ccdp::E_GPS;
									}
									else {
										locateInfo.nLocateType = ccdp::E_LBS;
									}
									std::string strLatitude = strContentList[4];
									locateInfo.dLatitude = atof(strLatitude.c_str());
									std::string strLatType = strContentList[5];
									if (strLatType == "N" || strLatType == "n") {
										locateInfo.usLatType = 1;
									}
									else {
										locateInfo.usLatType = 0;
									}
									std::string strLngitude = strContentList[6];
									locateInfo.dLongitude = atof(strLngitude.c_str());
									std::string strLngType = strContentList[7];
									if (strLngType == "E" || strLngType == "e") {
										locateInfo.usLngType = 1;
									}
									else {
										locateInfo.usLngType = 0;
									}
									std::string strSpeed = strContentList[8];
									locateInfo.dSpeed = atof(strSpeed.c_str());
									std::string strDirect = strContentList[9];
									locateInfo.dDirection = atof(strDirect.c_str());
									std::string strHeight = strContentList[10];
									locateInfo.nElevation = atoi(strHeight.c_str());
									std::string strSatelliteCount = strContentList[11];
									locateInfo.nGpsStatelliteCount = atoi(strSatelliteCount.c_str());
									std::string strSignalIntensity = strContentList[12];
									locateInfo.nSignalIntensity = atoi(strSignalIntensity.c_str());
									std::string strBattery = strContentList[13];
									locateInfo.nBattery = atoi(strBattery.c_str());
									std::string strStatus = strContentList[16];
									sscanf_s(strStatus.c_str(), "%x", &locateInfo.nStatus);
									if (locateInfo.nLocateType == ccdp::E_LBS) {
										std::string strBaseStationCount = strContentList[17];
										locateInfo.nBaseStationCount = atoi(strBaseStationCount.c_str());
										std::string strGsmDelay = strContentList[18];
										locateInfo.nGsmDelay = atoi(strGsmDelay.c_str());
										std::string strNationCode = strContentList[19];
										locateInfo.nNationCode = atoi(strNationCode.c_str());
										std::string strNetCode = strContentList[20];
										locateInfo.nNetCode = atoi(strNetCode.c_str());
										if (locateInfo.nBaseStationCount) {
											locateInfo.pBaseStationList = new escort::BaseStation[locateInfo.nBaseStationCount];
											for (int i = 0; i < locateInfo.nBaseStationCount; i++) {
												std::string strLocationCode = strContentList[21 + i * 3];
												std::string strBaseStationId = strContentList[21 + (i * 3 + 1)];
												std::string strBsIntensity = strContentList[21 + (i * 3 + 2)];
												if (!strLocationCode.empty() && isdigit(strLocationCode.c_str()[0])) {
													locateInfo.pBaseStationList[i].lac = atoi(strLocationCode.c_str());
												}
												if (!strBaseStationId.empty() && isdigit(strBaseStationId.c_str()[0])) {
													locateInfo.pBaseStationList[i].cellId = atoi(strBaseStationId.c_str());
												}
												if (!strSignalIntensity.empty()) {
													if (isdigit(strSignalIntensity.c_str()[0])
														|| isdigit(strSignalIntensity.c_str()[0] == '-')) {
														locateInfo.pBaseStationList[i].intensity = atoi(strBsIntensity.c_str());
													}
												}
											}
										}
										if (nListCount > (size_t)(21 + locateInfo.nBaseStationCount * 3)) {
											std::string strDetectedWifiCount = strContentList[21 + locateInfo.nBaseStationCount * 3];
											int nDetectedWifiCount = atoi(strDetectedWifiCount.c_str());
											if (nDetectedWifiCount > 0 && nListCount >= (size_t)(21 + locateInfo.nBaseStationCount * 3
												+ nDetectedWifiCount * 3)) {
												locateInfo.nDetectedWifiCount = nDetectedWifiCount;
												locateInfo.pDetectedWifiList = new escort::Wifi[nDetectedWifiCount];
												for (int i = 0; i < nDetectedWifiCount; i++) {
													//std::string strWifiTagName = strContentList[22 + locateInfo.nBaseStationCount * 3 + (i * 3)];
													std::string strWifiMacAddress = strContentList[22 + locateInfo.nBaseStationCount * 3 + (i * 3 + 1)];
													std::string strWifiSignalIntensity = strContentList[22 + locateInfo.nBaseStationCount * 3 + (i * 3 + 2)];
													locateInfo.pDetectedWifiList[i].name[0] = '\0';
													strcpy_s(locateInfo.pDetectedWifiList[i].mac, sizeof(locateInfo.pDetectedWifiList[i].mac),
														strWifiMacAddress.c_str());
													if (!strWifiSignalIntensity.empty()) {
														if (isdigit(strWifiSignalIntensity.c_str()[0]) || (strWifiSignalIntensity.c_str()[0] == '-'
															&& isdigit(strWifiSignalIntensity.c_str()[1]))) {
															locateInfo.pDetectedWifiList[i].intensity = atoi(strWifiSignalIntensity.c_str());
														}
													}
												}
											}
										}
									}
									handleLocate(&locateInfo, ccdp::E_REALTIME);
									if (locateInfo.pBaseStationList && locateInfo.nBaseStationCount) {
										delete[] locateInfo.pBaseStationList;
										locateInfo.pBaseStationList = NULL;
										locateInfo.nBaseStationCount = 0;
									}
									if (locateInfo.pDetectedWifiList && locateInfo.nDetectedWifiCount) {
										delete[] locateInfo.pDetectedWifiList;
										locateInfo.pDetectedWifiList = NULL;
										locateInfo.nDetectedWifiCount = 0;
									}
								}
							}
							else if (strcmp(strContentList[0].c_str(), "UD2") == 0) {
								size_t nListCount = strContentList.size();
								if (nListCount >= 20) {
									ccdp::LocateInfo locateInfo;
									strcpy_s(locateInfo.szDeviceId, sizeof(locateInfo.szDeviceId), strDeviceId.c_str());
									std::string strDate = strContentList[1];
									std::string strTime = strContentList[2];
									unsigned short usDay = 0;
									unsigned short usMonth = 0;
									unsigned short usYear = 0;
									unsigned short usHour = 0;
									unsigned short usMinute = 0;
									unsigned short usSecond = 0;
									if (!strDate.empty() && !strTime.empty()) {
										sscanf_s(strDate.c_str(), "%02hu%02hu%02hu", &usDay, &usMonth, &usYear);
										usYear += 2000;
										sscanf_s(strTime.c_str(), "%02hu%02hu%02hu", &usHour, &usMinute, &usSecond);
										usHour += 8;
										if (usHour >= 24) {
											usHour -= 24;
											usDay += 1;
										}
										char szLocateTime[20] = { 0 };
										sprintf_s(szLocateTime, sizeof(szLocateTime), "%04d%02d%02d%02d%02d%02d",
											usYear, usMonth, usDay, usHour, usMinute, usSecond);
										locateInfo.ullLocateTime = makeDatetime(szLocateTime);
										if (locateInfo.ullLocateTime > now) {
											locateInfo.ullLocateTime = now;
										}
									}
									std::string strLatitude = strContentList[4];
									locateInfo.dLatitude = atof(strLatitude.c_str());
									std::string strLatType = strContentList[5];
									if (strLatType == "N" || strLatType == "n") {
										locateInfo.usLatType = 1;
									}
									else {
										locateInfo.usLatType = 0;
									}
									std::string strLngitude = strContentList[6];
									locateInfo.dLongitude = atof(strLngitude.c_str());
									std::string strLngType = strContentList[7];
									if (strLngType == "E" || strLngType == "e") {
										locateInfo.usLngType = 1;
									}
									else {
										locateInfo.usLngType = 0;
									}
									std::string strSpeed = strContentList[8];
									locateInfo.dSpeed = atof(strSpeed.c_str());
									std::string strDirect = strContentList[9];
									locateInfo.dDirection = atof(strDirect.c_str());
									std::string strHeight = strContentList[10];
									locateInfo.nElevation = atoi(strHeight.c_str());
									std::string strSatelliteCount = strContentList[11];
									locateInfo.nGpsStatelliteCount = atoi(strSatelliteCount.c_str());
									std::string strSignalIntensity = strContentList[12];
									locateInfo.nSignalIntensity = atoi(strSignalIntensity.c_str());
									std::string strBattery = strContentList[13];
									locateInfo.nBattery = atoi(strBattery.c_str());
									std::string strStatus = strContentList[16];
									sscanf_s(strStatus.c_str(), "%x", &locateInfo.nStatus);
									std::string strBaseStationCount = strContentList[17];
									locateInfo.nBaseStationCount = atoi(strBaseStationCount.c_str());
									std::string strGsmDelay = strContentList[18];
									locateInfo.nGsmDelay = atoi(strGsmDelay.c_str());
									std::string strNationCode = strContentList[19];
									locateInfo.nNationCode = atoi(strNationCode.c_str());
									std::string strNetCode = strContentList[20];
									locateInfo.nNetCode = atoi(strNetCode.c_str());
									if (locateInfo.nBaseStationCount) {
										locateInfo.pBaseStationList = new escort::BaseStation[locateInfo.nBaseStationCount];
										for (int i = 0; i < locateInfo.nBaseStationCount; i++) {
											std::string strLocationCode = strContentList[21 + i * 3];
											std::string strBaseStationId = strContentList[21 + (i * 3 + 1)];
											std::string strBsIntensity = strContentList[21 + (i * 3 + 2)];
											locateInfo.pBaseStationList[i].lac = atoi(strLocationCode.c_str());
											locateInfo.pBaseStationList[i].cellId = atoi(strBaseStationId.c_str());
											locateInfo.pBaseStationList[i].intensity = atoi(strBsIntensity.c_str());
										}
									}
									if (nListCount > (size_t)(21 + locateInfo.nBaseStationCount * 3)) {
										std::string strDetectedWifiCount = strContentList[21 + locateInfo.nBaseStationCount * 3];
										int nDetectedWifiCount = atoi(strDetectedWifiCount.c_str());
										if (nDetectedWifiCount > 0 && nListCount >= (size_t)(21 + locateInfo.nBaseStationCount * 3
											+ nDetectedWifiCount * 3)) {
											locateInfo.nDetectedWifiCount = nDetectedWifiCount;
											locateInfo.pDetectedWifiList = new escort::Wifi[nDetectedWifiCount];
											for (int i = 0; i < nDetectedWifiCount; i++) {
												std::string strWifiMacAddress = strContentList[22+locateInfo.nBaseStationCount*3+(i*3+1)];
												std::string strWifiSignalIntensity = strContentList[22+locateInfo.nBaseStationCount*3+(i*3+2)];
												locateInfo.pDetectedWifiList[i].name[0] = '\0';
												strcpy_s(locateInfo.pDetectedWifiList[i].mac, sizeof(locateInfo.pDetectedWifiList[i].mac),
													strWifiMacAddress.c_str());
												if (!strWifiSignalIntensity.empty()) {
													if (isdigit(strWifiSignalIntensity.c_str()[0]) || (strWifiSignalIntensity.c_str()[0] == '-' 
															&& isdigit(strWifiSignalIntensity.c_str()[1]))) {
														locateInfo.pDetectedWifiList[i].intensity = atoi(strWifiSignalIntensity.c_str());
													}
												}
											}
										}
									}
									handleLocate(&locateInfo, ccdp::E_HISTORY);
									if (locateInfo.pBaseStationList && locateInfo.nBaseStationCount) {
										delete[] locateInfo.pBaseStationList;
										locateInfo.pBaseStationList = NULL;
										locateInfo.nBaseStationCount = 0;
									}
									if (locateInfo.pDetectedWifiList && locateInfo.nDetectedWifiCount) {
										delete[] locateInfo.pDetectedWifiList;
										locateInfo.pDetectedWifiList = NULL;
										locateInfo.nDetectedWifiCount = 0;
									}
								}
							}
							else if (strcmp(strContentList[0].c_str(), "AL") == 0) {
								char szReply[256] = { 0 };
								sprintf_s(szReply, sizeof(szReply), "[SG*%s*0002*AL]", strDeviceId.c_str());
								TS_SendData(m_ullSrvInst, pMsg_->szEndPoint, szReply, (unsigned int)strlen(szReply));
								size_t const nListCount = strContentList.size();
								if (nListCount >= 20) {
									time_t const now = time(NULL);
									struct tm tm_now;
									localtime_s(&tm_now, &now);
									tm_now.tm_year += 1900;
									tm_now.tm_mon += 1;

									ccdp::LocateInfo locateInfo;
									strcpy_s(locateInfo.szDeviceId, sizeof(locateInfo.szDeviceId), strDeviceId.c_str());
									std::string strDate = strContentList[1];
									std::string strTime = strContentList[2];
									unsigned short usDay = 0;
									unsigned short usMonth = 0;
									unsigned short usYear = 0;
									unsigned short usHour = 0;
									unsigned short usMinute = 0;
									unsigned short usSecond = 0;
									if (!strDate.empty() && !strTime.empty()) {
										sscanf_s(strDate.c_str(), "%02hu%02hu%02hu", &usDay, &usMonth, &usYear);
										usYear += 2000;
										sscanf_s(strTime.c_str(), "%02hu%02hu%02hu", &usHour, &usMinute, &usSecond);
										usHour += 8;
										if (usHour >= 24) {
											usHour -= 24;
											usDay += 1;
										}
										char szLocateTime[20] = { 0 };
										sprintf_s(szLocateTime, sizeof(szLocateTime), "%04d%02d%02d%02d%02d%02d",
											usYear, usMonth, usDay, usHour, usMinute, usSecond);
										locateInfo.ullLocateTime = makeDatetime(szLocateTime);
									}
									bool bContinue = true;
									if (usYear < tm_now.tm_year) {
										bContinue = false;
									}
									else if (usYear == tm_now.tm_year) {
										if (usMonth < tm_now.tm_mon) {
											bContinue = false;
										}
									}
									if (bContinue) {
										std::string strLocateFlag = strContentList[3];
										if (strLocateFlag == "A" || strLocateFlag == "a") {
											locateInfo.nLocateType = ccdp::E_GPS;
										}
										else {
											locateInfo.nLocateType = ccdp::E_LBS;
										}
										std::string strLatitude = strContentList[4];
										locateInfo.dLatitude = atof(strLatitude.c_str());
										std::string strLatType = strContentList[5];
										if (strLatType == "N" || strLatType == "n") {
											locateInfo.usLatType = 1;
										}
										else {
											locateInfo.usLatType = 0;
										}
										std::string strLngitude = strContentList[6];
										locateInfo.dLongitude = atof(strLngitude.c_str());
										std::string strLngType = strContentList[7];
										if (strLngType == "E" || strLngType == "e") {
											locateInfo.usLngType = 1;
										}
										else {
											locateInfo.usLngType = 0;
										}
										std::string strBattery = strContentList[13];
										locateInfo.nBattery = atoi(strBattery.c_str());
										std::string strStatus = strContentList[16];
										sscanf_s(strStatus.c_str(), "%x", &locateInfo.nStatus);
										handleAlarm(&locateInfo);
									}
								}
							} 
							else if (strcmp(strContentList[0].c_str(), "TKQ") == 0) {
								char szReply[256] = { 0 };
								sprintf_s(szReply, sizeof(szReply), "[SG*%s*0003*TKQ]", strDeviceId.c_str());
								TS_SendData(m_ullSrvInst, pMsg_->szEndPoint, szReply, (unsigned int)strlen(szReply));
							}
							else if (strcmp(strContentList[0].c_str(), "TKQ2") == 0) {
								char szReply[256] = { 0 };
								sprintf_s(szReply, sizeof(szReply), "[SG*%s*0004*TKQ2]", strDeviceId.c_str());
								TS_SendData(m_ullSrvInst, pMsg_->szEndPoint, szReply, (unsigned int)strlen(szReply));
							}
							else if (strcmp(strContentList[0].c_str(), "ZJ") == 0) {
								//recv
							} 
							else if (strcmp(strContentList[0].c_str(), "ZJSJ") == 0) {
								//recv
							}
							else if (strcmp(strContentList[0].c_str(), "SBLJ") == 0) {
								//device reply
							} 
							else if (strcmp(strContentList[0].c_str(), "RWZX") == 0) {
								//device reply
							}
							else if (strcmp(strContentList[0].c_str(), "WSTF") == 0) {
								//device reply
							}
							else if (strcmp(strContentList[0].c_str(), "VERNO") == 0) {
								reportDeviceOnline(strDeviceId.c_str());
							}
							else if (strcmp(strContentList[0].c_str(), "RESET") == 0) {
								//
							}
							else if (strcmp(strContentList[0].c_str(), "CR") == 0) {
								//
							}
							else if (strcmp(strContentList[0].c_str(), "CHRSTU") == 0) {
								char szReply[256] = { 0 };
								sprintf_s(szReply, sizeof(szReply), "[SG*%s*0006*CHRSTU]", strDeviceId.c_str());
								TS_SendData(m_ullSrvInst, pMsg_->szEndPoint, szReply, (unsigned int)strlen(szReply));
								size_t nListCount = strContentList.size();
								if (nListCount == 1) {
									char szMsg[256] = { 0 };
									sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"charge\":0,\"datetime\":%llu}",
										getNextSequence(), strDeviceId.c_str(), now);
									sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_CHARGE);
								}
								else if (nListCount >= 2) {
									std::string strState = strContentList[1];
									int charge = (int)strtol(strState.c_str(), NULL, 10);
									char szMsg[256] = { 0 };
									sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"charge\":%d,\"datetime\":%llu}",
										getNextSequence(), strDeviceId.c_str(), charge > 0 ? 1 : 0,  now);
									sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_CHARGE);
								}
							}
						}
						strContentList.clear();
						strDeviceId.clear();
					}
					delete[] pBuf;
					pBuf = NULL;
				}
			} while (1);
		}
	}
}

int CcDeviceProxy::getWholeMessage(const unsigned char * pData_, unsigned int uiDataLen_, 
	unsigned int uiIndex_, unsigned int & uiBeginIndex_, unsigned int & uiEndIndex_)
{
	int result = 0;
	unsigned int i = uiIndex_;
	char bc = BEGIN_TOKEN;
	char ec = END_TOKEN;
	bool bFIndFirst = false;
	uiBeginIndex_ = 0;
	uiEndIndex_ = 0;
	do {
		if (i >= uiDataLen_) {
			break;
		}
		if (!bFIndFirst) {
			if (pData_[i] == bc) {
				result = 1;
				bFIndFirst = true;
				uiBeginIndex_ = i;
			}
			i++;
		}
		else {
			if (pData_[i] == ec) {
				result = 2;
				uiEndIndex_ = i;
				break;
			}
			else {
				i++;
			}
		}
	} while (1);
	return result; 
}

void CcDeviceProxy::splitString(std::string strSrc_, std::string strDelimiter_, std::vector<std::string> & strList_)
{
	strList_.clear();
	std::string strCell = strSrc_;
	if (!strCell.empty()) {
		size_t n = strCell.find_first_of(strDelimiter_);
		while (n != std::string::npos) {
			std::string strUnit = strCell.substr(0, n);
			strList_.push_back(strUnit);
			strCell = strCell.substr(n + 1);
			if (strCell.empty()) {
				break;
			}
			n = strCell.find_first_of(strDelimiter_);
		}
		if (!strCell.empty()) {
			strList_.push_back(strCell);
		}
	}
}

void CcDeviceProxy::splitString2(std::string strSrc_, std::string strDelimiter_, std::vector<std::string> & strList_)
{
	strList_.clear();
	if (!strSrc_.empty()) {
		std::size_t from = 0;
		std::size_t n = strSrc_.find_first_of(strDelimiter_, 0);
		while (n != std::string::npos) {
			if (n != from) {
				std::string strTmp = strSrc_.substr(from, n - from);
				strList_.emplace_back(strTmp);
			}
			else {
				strList_.emplace_back("null");
			}
			from = n + strDelimiter_.size();
			n = strSrc_.find_first_of(strDelimiter_, from);
		}
		if (from < strSrc_.size()) {
			std::string strTmp = strSrc_.substr(from);
			strList_.emplace_back(strTmp);
		}
	}
}

void CcDeviceProxy::handleLink(const char * pLink_)
{
	char szLog[256] = { 0 };
	snprintf(szLog, sizeof(szLog), "[device]%s[%d]link=%s connect\n", __FUNCTION__, __LINE__, pLink_);
	LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
}

void CcDeviceProxy::handleDisLink(const char * pLink_)
{
	char szLog[256] = { 0 };
	char szDeviceId[24] = { 0 };
	if (pLink_ && strlen(pLink_)) {
		std::lock_guard<std::mutex> lk(m_mutex4LinkList);
		DeviceLinkList::iterator iter = m_devLinkList.find(pLink_);
		if (iter != m_devLinkList.end()) {
			std::string strDeviceId = iter->second;
			if (!strDeviceId.empty()) {
				strcpy_s(szDeviceId, sizeof(szDeviceId), strDeviceId.c_str());
			}
			m_devLinkList.erase(iter);
		}
		sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]link=%s disconnect, deviceId=%s\n", 
			__FUNCTION__, __LINE__, pLink_, szDeviceId);
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
	}
	if (strlen(szDeviceId) > 0) {
		std::lock_guard<std::mutex> lk(m_mutex4DevList);
		DeviceList::iterator iter = m_devList.find(szDeviceId);
		if (iter != m_devList.end()) {
			ccdp::DeviceInfo * pDevInfo = iter->second;
			if (pDevInfo) {
				if (strcmp(pDevInfo->link, pLink_) == 0) {
					sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]remove link=%s for deviceId=%s\n",
						__FUNCTION__, __LINE__, pLink_, szDeviceId);
					LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
					pDevInfo->online = ccdp::E_LS_OFFLINE;
					memset(pDevInfo->link, 0, sizeof(pDevInfo->link));
					//notify offline 
					char szMsg[256] = { 0 };
					sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"battery\":%d,\"online\":0,"
						"\"loose\":%d,\"datetime\":%llu}", getNextSequence(), pDevInfo->deviceId, pDevInfo->battery,
						pDevInfo->loose, time(NULL));
					sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_INFO);
				}
			}
		}
	}
}

void CcDeviceProxy::updateDevice(const char * pLink_, const char * pDeviceId_)
{
	char szLog[256] = { 0 };
	if (pLink_ && strlen(pLink_) && pDeviceId_ && strlen(pDeviceId_)) {
		unsigned long long now = time(NULL);
		//1
		{
			std::lock_guard<std::mutex> lk(m_mutex4LinkList);
			DeviceLinkList::iterator iter = m_devLinkList.find(pLink_);
			if (iter != m_devLinkList.end()) {
				std::string strDeviceId = iter->second;
				if (!strDeviceId.empty()) {
					if (strcmp(pDeviceId_, strDeviceId.c_str()) != 0) {
						sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]modify pair %s-%s to %s-%s\n",
							__FUNCTION__, __LINE__, pLink_, strDeviceId.c_str(), pLink_, pDeviceId_);
						LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
						m_devLinkList[pLink_] = pDeviceId_;
					}
				}
				else {
					m_devLinkList[pLink_] = pDeviceId_;
					sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]add new pair %s-%s\n",
						__FUNCTION__, __LINE__, pLink_, pDeviceId_);
					LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
				}
			}
			else {
				m_devLinkList.emplace(pLink_, pDeviceId_);
				snprintf(szLog, sizeof(szLog), "[device]%s[%d]add new pair %s-%s\n",
					__FUNCTION__, __LINE__, pDeviceId_, pLink_);
				LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
			}
		}
		//2
		{
			std::lock_guard<std::mutex> lk(m_mutex4DevList);
			DeviceList::iterator iter = m_devList.find(pDeviceId_);
			if (iter != m_devList.end()) {
				ccdp::DeviceInfo * pDevInfo = iter->second;
				if (pDevInfo) {
					pDevInfo->online = ccdp::E_LS_ONLINE;
					pDevInfo->activeTime = now;
					strcpy_s(pDevInfo->link, sizeof(pDevInfo->link), pLink_);
				}
			}
			else {
				ccdp::DeviceInfo * pDevInfo = new ccdp::DeviceInfo();
				pDevInfo->loose = -1;
				pDevInfo->online = 1;
				pDevInfo->battery = 0;
				pDevInfo->recordPos = 0;
				pDevInfo->lastLat = .0;
				pDevInfo->lastLng = .0;
				strcpy_s(pDevInfo->deviceId, sizeof(pDevInfo->deviceId), pDeviceId_);
				strcpy_s(pDevInfo->link, sizeof(pDevInfo->link), pLink_);
				pDevInfo->activeTime = now;
				m_devList.emplace(pDeviceId_, pDevInfo);
			}
		}
	}
}

void CcDeviceProxy::updateDeviceBattery(const char * pLink_, const char * pDeviceId_, unsigned short battery_)
{
	char szMsg[256] = { 0 };
	if (pDeviceId_ && strlen(pDeviceId_)) {
		unsigned long long now = time(NULL);
		std::lock_guard<std::mutex> lk(m_mutex4DevList);
		DeviceList::iterator iter = m_devList.find(pDeviceId_);
		if (iter != m_devList.end()) {
			auto pDevice = iter->second;
			if (pDevice) {
				pDevice->online = ccdp::E_LS_ONLINE;
				pDevice->battery = battery_;
				pDevice->activeTime = now;
				strcpy_s(pDevice->link, sizeof(pDevice->link), pLink_);
				sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"battery\":%d,\"online\":1,"
					"\"loose\":%d,\"datetime\":%llu}", getNextSequence(), pDeviceId_, battery_, pDevice->loose, now);
			}
		}
		else {
			auto pDevice = new ccdp::DeviceInfo();
			pDevice->loose = -1;
			pDevice->online = ccdp::E_LS_ONLINE;
			pDevice->battery = battery_;
			pDevice->recordPos = 0;
			pDevice->lastLat = .0;
			pDevice->lastLng = .0;
			strcpy_s(pDevice->deviceId, sizeof(pDevice->deviceId), pDeviceId_);
			strcpy_s(pDevice->link, sizeof(pDevice->link), pLink_);
			pDevice->activeTime = now;
			m_devList.emplace(pDeviceId_, pDevice);
			sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"battery\":%d,\"online\":1,"
				"\"datetime\":%llu}", getNextSequence(), pDeviceId_, battery_, now);
		}
	}
	if (strlen(szMsg)) {
		sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_INFO);
	}
	char szLog[256] = { 0 };
	sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]update device=%s, battery=%d, link=%s\n",
		__FUNCTION__, __LINE__, pDeviceId_, battery_, pLink_);
	LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
}

void CcDeviceProxy::reportDeviceOnline(const char * pDeviceId_)
{
	char szMsg[256] = { 0 };
	if (pDeviceId_ && strlen(pDeviceId_)) {
		unsigned long long now = time(NULL);
		std::lock_guard<std::mutex> lk(m_mutex4DevList);
		DeviceList::iterator iter = m_devList.find(pDeviceId_);
		if (iter != m_devList.end()) {
			auto pDevice = iter->second;
			if (pDevice) {
				if (pDevice->online == ccdp::E_LS_OFFLINE) {
					pDevice->online = ccdp::E_LS_ONLINE;
					sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"battery\":%d,\"online\":1,"
						"\"loose\":%d,\"datetime\":%llu}", getNextSequence(), pDeviceId_, pDevice->battery, pDevice->loose, now);
				}
				pDevice->activeTime = now;
			}
		}
	}
	if (strlen(szMsg)) {
		sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_CONF);
		char szLog[256] = { 0 };
		sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]update device=%s, online=1\n", __FUNCTION__, __LINE__, pDeviceId_);
		LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
	}
}

void CcDeviceProxy::handleLocate(ccdp::LocateInfo * pLocateInfo_, int nRealtime_)
{
	if (pLocateInfo_ && strlen(pLocateInfo_->szDeviceId)) {
		char szMsg[512] = { 0 };
		int loose = analyzeDeviceStatus(pLocateInfo_->nStatus);
		if (nRealtime_) { //realtime
			if (pLocateInfo_->nLocateType == ccdp::E_GPS) { //gps
				sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"realtime\":1,\"locateType\":%d,"
					"\"latitude\":%.06f,\"longitude\":%.06f,\"elevation\":%d,\"coordinate\":%d,\"speed\":%.03f,"
					"\"direction\":%0.2f,\"locateTime\":%llu,\"battery\":%d,\"loose\":%d,\"accuracy\":1}", 
					getNextSequence(), pLocateInfo_->szDeviceId, ccdp::E_GPS, pLocateInfo_->dLatitude, 
					pLocateInfo_->dLongitude, pLocateInfo_->nElevation, escort::COORDTYPE_WGS84, pLocateInfo_->dSpeed, 
					pLocateInfo_->dDirection, pLocateInfo_->ullLocateTime, pLocateInfo_->nBattery, loose);
				sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_LOCATE);
				std::lock_guard<std::mutex> lk(m_mutex4DevList);
				DeviceList::iterator iter = m_devList.find(pLocateInfo_->szDeviceId);
				if (iter != m_devList.end()) {
					ccdp::DeviceInfo* pDevice = iter->second;
					if (pDevice) {
						pDevice->activeTime = time(NULL);
						pDevice->battery = pLocateInfo_->nBattery;
						pDevice->loose = loose;
						pDevice->online = ccdp::E_LS_ONLINE;
						if (pDevice->recordPos == 0) {
							pDevice->recordPos = 1;
						}
						pDevice->lastLat = pLocateInfo_->dLatitude;
						pDevice->lastLng = pLocateInfo_->dLongitude;
					}
				}
			}
			else { //lbs
				int coordinate = escort::COORDTYPE_WGS84;
				int nAccuracy = 0;
				if (m_usLbsQryType && strlen(m_szLbsQryKey)) {
					char szBts[32] = { 0 };
					char szNearBts[256] = { 0 };
					char szWifis[256] = { 0 };
					if (pLocateInfo_->nBaseStationCount && pLocateInfo_->pBaseStationList) {
						sprintf_s(szBts, sizeof(szBts), "460,01,%d,%d,%d", pLocateInfo_->pBaseStationList[0].lac,
							pLocateInfo_->pBaseStationList[0].cellId, pLocateInfo_->pBaseStationList[0].intensity);
						for (int i = 1; i < pLocateInfo_->nBaseStationCount; i++) {
							char szBtsCell[32] = { 0 };
							sprintf_s(szBtsCell, sizeof(szBtsCell), "460,01,%d,%d,%d", pLocateInfo_->pBaseStationList[i].lac,
								pLocateInfo_->pBaseStationList[i].cellId, pLocateInfo_->pBaseStationList[i].intensity);
							if (strlen(szNearBts) == 0) {
								strncpy_s(szNearBts, sizeof(szNearBts), szBtsCell, strlen(szBtsCell));
							}
							else {
								strcat_s(szNearBts, sizeof(szNearBts), "|");
								strcat_s(szNearBts, sizeof(szNearBts), szBtsCell);
							}
						}
					}
					if (pLocateInfo_->nDetectedWifiCount && pLocateInfo_->pDetectedWifiList) {
						for (int i = 0; i < pLocateInfo_->nDetectedWifiCount; i++) {
							if (pLocateInfo_->pDetectedWifiList[i].mac[0] != 0) {
								char szCell[32] = { 0 };
								sprintf_s(szCell, sizeof(szCell), "%s,%d,%d", pLocateInfo_->pDetectedWifiList[i].mac,
									pLocateInfo_->pDetectedWifiList[i].intensity, i + 1);
								if (strlen(szWifis) == 0) {
									strncpy_s(szWifis, sizeof(szWifis), szCell, strlen(szCell));
								}
								else {
									strcat_s(szWifis, sizeof(szWifis), "|");
									strcat_s(szWifis, sizeof(szWifis), szCell);
								}
							}
						}
					}
					char szUrl[1024] = { 0 };
					sprintf_s(szUrl, sizeof(szUrl), "http://%s/position?accesstype=0&imei=&cdma=0&bts=%s&nearbts=%s"
						"&network=GPRS&macs=%s&output=json&key=%s", m_szLbsDomain, szBts, szNearBts, szWifis, m_szLbsQryKey);
					LbsQueryResult lbsQry;
					memset(&lbsQry, 0, sizeof(LbsQueryResult));
					char szLog[1408] = { 0 };
					if (LbsGeoQuery(szUrl, QRY_OBJ_AMAP, &lbsQry) == 0) {
						if (lbsQry.nRetCode == 0 && lbsQry.dLat != 0.00 && lbsQry.dLng != 0.00) {
							coordinate = escort::COORDTYPE_GCJ02;
							pLocateInfo_->dLatitude = lbsQry.dLat;
							pLocateInfo_->dLongitude = lbsQry.dLng;
							nAccuracy = 1;
							std::lock_guard<std::mutex> lk(m_mutex4DevList);
							DeviceList::iterator iter = m_devList.find(pLocateInfo_->szDeviceId);
							if (iter != m_devList.end()) {
								ccdp::DeviceInfo* pDevice = iter->second;
								if (pDevice) {
									pDevice->activeTime = time(NULL);
									pDevice->battery = pLocateInfo_->nBattery;
									pDevice->loose = loose;
									pDevice->online = ccdp::E_LS_ONLINE;
									if (pDevice->recordPos == 0) {
										pDevice->recordPos = 1;
									}
									pDevice->lastLat = pLocateInfo_->dLatitude;
									pDevice->lastLng = pLocateInfo_->dLongitude;
								}
							}
						}
						sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]lbs url=%s, result=%d, latitude=%.06f, longitude=%.06f,"
							" coordinate=%d\n", __FUNCTION__, __LINE__, szUrl, lbsQry.nRetCode, lbsQry.dLat, lbsQry.dLng, 
							lbsQry.nCoordinate);
						LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
					}
					else {
						std::lock_guard<std::mutex> lk(m_mutex4DevList);
						DeviceList::iterator iter = m_devList.find(pLocateInfo_->szDeviceId);
						if (iter != m_devList.end()) {
							ccdp::DeviceInfo* pDevice = iter->second;
							if (pDevice) {
								pDevice->activeTime = time(NULL);
								pDevice->battery = pLocateInfo_->nBattery;
								pDevice->loose = loose;
								pDevice->online = ccdp::E_LS_ONLINE;
								if (pDevice->recordPos == 1) {
									pLocateInfo_->dLatitude = pDevice->lastLat;
									pLocateInfo_->dLongitude = pDevice->lastLng;
								}
							}
						}
						sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]lbs url=%s, failed, record lat=%.6f, lng=%.6f\n",
							__FUNCTION__, __LINE__, szUrl, pLocateInfo_->dLatitude, pLocateInfo_->dLongitude);
						LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
					}
				} 
				sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"realtime\":1,\"locateType\":%d,"
					"\"latitude\":%.06f,\"longitude\":%.06f,\"coordinate\":%d,\"elevation\":%d,\"speed\":%.03f,"
					"\"direction\":%0.2f,\"locateTime\":%llu,\"battery\":%d,\"loose\":%d,\"accuracy\":%d}", 
					getNextSequence(), pLocateInfo_->szDeviceId, ccdp::E_LBS, pLocateInfo_->dLatitude, 
					pLocateInfo_->dLongitude, coordinate, pLocateInfo_->nElevation, pLocateInfo_->dSpeed, 
					pLocateInfo_->dDirection, pLocateInfo_->ullLocateTime, pLocateInfo_->nBattery, loose, nAccuracy);
				sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_LOCATE);
			}		
		}
		else { //history
			sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"realtime\":0,\"locateType\":%d,"
				"\"latitude\":%.06f,\"longitude\":%.06f,\"elevation\":%d,\"coordinate\":%d,\"speed\":%.03f,"
				"\"direction\":%0.2f,\"locateTime\":%llu,\"battery\":%d,\"loose\":%d,\"accuracy\":%d}", 
				getNextSequence(), pLocateInfo_->szDeviceId,pLocateInfo_->nLocateType, pLocateInfo_->dLatitude, 
				pLocateInfo_->dLongitude, pLocateInfo_->nElevation, escort::COORDTYPE_WGS84, pLocateInfo_->dSpeed, 
				pLocateInfo_->dDirection, pLocateInfo_->ullLocateTime, pLocateInfo_->nBattery, loose, 
				(pLocateInfo_->nLocateType == ccdp::E_GPS) ? 1 : 0);
			sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_LOCATE);
		}
	}
}

void CcDeviceProxy::handleAlarm(ccdp::LocateInfo * pLocateInfo_)
{
	if (pLocateInfo_) {
		unsigned long long now = time(NULL);
		if (strlen(pLocateInfo_->szDeviceId)) {
			int loose = analyzeDeviceStatus(pLocateInfo_->nStatus);
			std::lock_guard<std::mutex> lk(m_mutex4DevList);
			DeviceList::iterator iter = m_devList.find(pLocateInfo_->szDeviceId);
			if (iter != m_devList.end()) {
				ccdp::DeviceInfo * pDevice = iter->second;
				if (pDevice) {
					pDevice->activeTime = now;
					pDevice->battery = pLocateInfo_->nBattery;
					pDevice->loose = loose;
					pDevice->online = ccdp::E_LS_ONLINE;
					char szMsg[256] = { 0 };
					sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"battery\":%d,\"online\":1,"
						"\"loose\":%d,\"datetime\":%llu}", getNextSequence(), pLocateInfo_->szDeviceId, pLocateInfo_->nBattery, 
						loose, now);
					sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_INFO);
				}
			}
		}
	}
}

int CcDeviceProxy::analyzeDeviceStatus(int status_)
{
	int result = 0;
	if (status_ > 0) {
		if ((status_ >> 20 & 1) || (status_ >> 3) & 1) {
			result = 1;
		}
	}
	return result;
}

bool CcDeviceProxy::addPipeMsg(escort::DeviceInteractMessage * pMsg_)
{
	bool result = false;
	if (pMsg_ && pMsg_->msgContent && pMsg_->msgSize) {
		std::lock_guard<std::mutex> lk(m_mutex4PipeMsgQue);
		m_pipeMsgQue.emplace(pMsg_);
		if (m_pipeMsgQue.size() == 1) {
			m_cond4PipeMsgQue.notify_all();
		}
		result = true;
	}
	return result;
}

void CcDeviceProxy::handlePipeMsg()
{
	do {
		std::unique_lock<std::mutex> lk(m_mutex4PipeMsgQue);
		m_cond4PipeMsgQue.wait(lk, [&] { return (!m_nRun || !m_pipeMsgQue.empty()); });
		if (!m_nRun && m_pipeMsgQue.empty()) {
			break;
		}
		auto pMsg = m_pipeMsgQue.front();
		m_pipeMsgQue.pop();
		lk.unlock();
		if (pMsg) {
			if (pMsg->msgContent && pMsg->msgSize) {
				auto js = nlohmann::json::parse(pMsg->msgContent);
				unsigned int seq = 0;
				if (js["seq"].is_number()) {
					seq = js["seq"].get<unsigned int>();
				}
				switch (pMsg->msgType) {
					case escort::MSG_DEV_PUSH_CONF: {
						escort::DeviceConfigInformation devConf;
						memset(&devConf, 0, sizeof(escort::DeviceConfigInformation));
						if (js["id"].is_string()) {
							std::string strId = js["id"].get<std::string>();
							if (!strId.empty()) {
								strcpy_s(devConf.deviceId, sizeof(devConf.deviceId), strId.c_str());
							}
						}
						if (js["factory"].is_number()) {
							devConf.factory = js["factory"].get<int>();
						}
						if (js["param"].is_number()) {
							devConf.param = js["param"].get<int>();
						}
						if (js["value"].is_number()) {
							devConf.value = js["value"].get<int>();
						}
						if (js["datetime"].is_number()) {
							devConf.datetime = js["datetime"].get<unsigned long long>();
						}
						if (strlen(devConf.deviceId)) {
							handlePipeDeviceConfig(&devConf, seq);
						}
						break;
					}
					case escort::MSG_DEV_PUSH_INFO: {
						break;
					}
					case escort::MSG_DEV_PUSH_CHARGE: {
						break;
					}
					case escort::MSG_DEV_PUSH_LOCATE: {
						break;
					}
					case escort::MSG_DEV_ALIVE: {
						break;
					}
				}
				delete[] pMsg->msgContent;
				pMsg->msgContent = NULL;
			}
			delete pMsg;
			pMsg = NULL;
		}
	} while (1);
}

void CcDeviceProxy::handlePipeDeviceConfig(escort::DeviceConfigInformation * pDevConf_, unsigned int seq_)
{
	if (pDevConf_) {
		char szMsg[256] = { 0 };
		char szLog[512] = { 0 };
		if (strlen(pDevConf_->deviceId)) {
			std::lock_guard<std::mutex> lk(m_mutex4DevList);
			DeviceList::iterator iter = m_devList.find(pDevConf_->deviceId);
			if (iter != m_devList.end()) {
				ccdp::DeviceInfo * pDevice = iter->second;
				if (pDevice) {
					if (pDevice->online && strlen(pDevice->link)) {
						switch (pDevConf_->param) {
							case escort::DEV_CONNECT: {
								char szCmd[256] = { 0 };
								sprintf_s(szCmd, sizeof(szCmd), "[SG*%s*0004*SBLJ]", pDevConf_->deviceId);
								TS_SendData(m_ullSrvInst, pDevice->link, szCmd, (unsigned int)strlen(szCmd));
								sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]send device connect to %s, deviceId=%s, cmd=%s\n",
									__FUNCTION__, __LINE__, pDevice->link, pDevice->deviceId, szCmd);
								LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								break;
							}
							case escort::DEV_TASK: {
								char szCmd[256] = { 0 };
								sprintf_s(szCmd, sizeof(szCmd), "[SG*%s*0006*RWZX,%d]", pDevConf_->deviceId, pDevConf_->value);
								TS_SendData(m_ullSrvInst, pDevice->link, szCmd, (unsigned int)strlen(szCmd));
								sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]send device task=%d to %s, deviceId=%s, cmd=%s\n",
									__FUNCTION__, __LINE__, pDevConf_->value, pDevice->link, pDevice->deviceId, szCmd);
								LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								break;
							}
							case escort::DEV_ALARM: {
								char szCmd[256] = { 0 };
								sprintf_s(szCmd, sizeof(szCmd), "[SG*%s*0006*WSTF,%d]", pDevConf_->deviceId, pDevConf_->value);
								TS_SendData(m_ullSrvInst, pDevice->link, szCmd, (unsigned int)strlen(szCmd));
								sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]send device alarm=%d to %s, deviceId=%s, cmd=%s\n",
									__FUNCTION__, __LINE__, pDevConf_->value, pDevice->link, pDevice->deviceId, szCmd);
								LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								break;
							}
							case escort::DEV_SET_INTERVAL: {
								char szCmd[256] = { 0 };
								if (pDevConf_->value > 0 && pDevConf_->value < 10) {
									sprintf_s(szCmd, sizeof(szCmd), "[SG*%s*0008*UPLOAD,%d]", pDevConf_->deviceId, pDevConf_->value);
								}
								else if (pDevConf_->value>= 10 && pDevConf_->value < 100) {
									sprintf_s(szCmd, sizeof(szCmd), "[SG*%s*0009*UPLOAD,%d]", pDevConf_->deviceId, pDevConf_->value);
								}
								else if (pDevConf_->value >= 100 && pDevConf_->value < 1000) {
									sprintf_s(szCmd, sizeof(szCmd), "[SG*%s*000a*UPLOAD,%d]", pDevConf_->deviceId, pDevConf_->value);
								}
								TS_SendData(m_ullSrvInst, pDevice->link, szCmd, (unsigned int)strlen(szCmd));
								sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]set device interval=%d to %s, deviceId=%s, cmd=%s\n",
									__FUNCTION__, __LINE__, pDevConf_->value, pDevice->link, pDevice->deviceId, szCmd);
								LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								break;
							}
							case escort::DEV_RESET: {
								char szCmd[256] = { 0 };
								sprintf_s(szCmd, sizeof(szCmd), "[SG*%s*0005*RESET]", pDevConf_->deviceId);
								TS_SendData(m_ullSrvInst, pDevice->link, szCmd, (unsigned int)strlen(szCmd));
								sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]send device reset to %s, deviceId=%s, cmd=%s\n",
									__FUNCTION__, __LINE__, pDevice->link, pDevice->deviceId, szCmd);
								LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								break;
							}
							case escort::DEV_POWEROFF: {
								char szCmd[256] = { 0 };
								sprintf_s(szCmd, sizeof(szCmd), "[SG*%s*0008*POWEROFF]", pDevConf_->deviceId);
								TS_SendData(m_ullSrvInst, pDevice->link, szCmd, (unsigned int)strlen(szCmd));
								sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]send device poweroff to %s, deviceId=%s, cmd=%s\n",
									__FUNCTION__, __LINE__, pDevice->link, pDevice->deviceId, szCmd);
								LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
								break;
							}
							default: break;
						}
						sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":%d,\"param\":%d,\"retcode\":0}", 
							seq_, pDevConf_->deviceId, pDevConf_->factory, pDevConf_->param);
					}
					else {
						sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":%d,\"param\":%d,\"retcode\":-1}",
							seq_, pDevConf_->deviceId, pDevConf_->factory, pDevConf_->param);
						sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]deviceId=%s, param=%d, value=%d, offline\n",
							__FUNCTION__, __LINE__, pDevConf_->deviceId, pDevConf_->param, pDevConf_->value);
						LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
					}
				}
			}
		}
		if (strlen(szMsg)) {
			sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_CONF);
		}
	}
}

void CcDeviceProxy::checkDeviceLink()
{
	size_t nDeviceSize = sizeof(ccdp::DeviceInfo);
	std::vector<ccdp::DeviceInfo *> offlineDevList;
	unsigned long long now = time(NULL);
	if (1) {
		std::lock_guard<std::mutex> lk(m_mutex4DevList);
		if (!m_devList.empty()) {
			for (DeviceList::iterator iter = m_devList.begin(); iter != m_devList.end(); iter++) {
				ccdp::DeviceInfo * pDevice = iter->second;
				if (pDevice) {
					bool bNotifyOffline = false;
					if (pDevice->online == ccdp::E_LS_ONLINE) {
						unsigned long long interval = now - pDevice->activeTime;
						if (interval > 60 && interval < 500) {
							char szCmd[256] = { 0 };
							sprintf_s(szCmd, sizeof(szCmd), "[SG*%s*0005*VERNO]", pDevice->deviceId);
							if (TS_SendData(m_ullSrvInst, pDevice->link, szCmd, (unsigned int)strlen(szCmd)) != 0) {
								bNotifyOffline = true;
							}
						}
						else if (interval >= 500) {
							bNotifyOffline = true;
						}
					}
					if (bNotifyOffline) {
						pDevice->online = ccdp::E_LS_OFFLINE;
						ccdp::DeviceInfo * pDupDevice = new ccdp::DeviceInfo();
						memcpy_s(pDupDevice, nDeviceSize, pDevice, nDeviceSize);
						offlineDevList.emplace_back(pDupDevice);
					}
				}
			}
		}
	}
	if (!offlineDevList.empty()) {
		char szLog[512] = { 0 };
		std::vector<ccdp::DeviceInfo *>::iterator iter = offlineDevList.begin();
		while (iter != offlineDevList.end()) {
			ccdp::DeviceInfo * pDevice = *iter;
			if (pDevice) {
				char szMsg[256] = { 0 };
				sprintf_s(szMsg, sizeof(szMsg), "{\"seq\":%u,\"id\":\"%s\",\"factory\":1,\"battery\":%d,\"online\":0,"
					"\"loose\":%d,\"datetime\":%llu}", getNextSequence(), pDevice->deviceId, pDevice->battery,
					pDevice->loose, now);
				sendMsgByPipe(szMsg, escort::MSG_DEV_PUSH_INFO);
				sprintf_s(szLog, sizeof(szLog), "[device]%s[%d]deviceId=%s, link=%s offline\n",
					__FUNCTION__, __LINE__, pDevice->deviceId, pDevice->link);
				LOG_Log(m_ullLogInst, szLog, pf_logger::eLOGCATEGORY_INFO, m_usLogType);
				TS_CloseEndpoint(m_ullSrvInst, pDevice->link);
				delete pDevice;
				pDevice = NULL;
			}
			iter = offlineDevList.erase(iter);
		}
		offlineDevList.clear();
	}
}

std::string CcDeviceProxy::utf8ToAnsi(LPCSTR utf8_)
{
	int nWLen = MultiByteToWideChar(CP_UTF8, 0, utf8_, -1, NULL, NULL);
	LPWSTR pWstr = (LPWSTR)_alloca((nWLen + 1) * sizeof(WCHAR));
	MultiByteToWideChar(CP_UTF8, 0, utf8_, -1, pWstr, nWLen);
	pWstr[nWLen] = '\0';
	int nALen = WideCharToMultiByte(CP_ACP, 0, pWstr, -1, NULL, 0, NULL, NULL);
	LPSTR pStr = (LPSTR)_alloca((nALen + 1) * sizeof(char));
	WideCharToMultiByte(CP_ACP, 0, pWstr, -1, pStr, nALen, NULL, NULL);
	pStr[nALen] = '\0';
	std::string result = pStr;
	return result;
}

void CcDeviceProxy::clearDeviceList()
{
	std::lock_guard<std::mutex> lk(m_mutex4DevList);
	if (!m_devList.empty()) {
		DeviceList::iterator iter = m_devList.begin();
		while (iter != m_devList.end()) {
			ccdp::DeviceInfo * pDevInfo = iter->second;
			if (pDevInfo) {
				delete pDevInfo;
				pDevInfo = NULL;
			}
			iter = m_devList.erase(iter);
		}
	}
}

void CcDeviceProxy::clearLinkList()
{
	std::lock_guard<std::mutex> lk(m_mutex4LinkList);
	m_devLinkList.clear();
}

unsigned int CcDeviceProxy::getNextSequence()
{
	std::lock_guard<std::mutex> lk(m_mutex4RepSeq);
	return m_uiRepSeq++;
}

void CcDeviceProxy::sendMsgByPipe(const char * pMsg_, unsigned short msgType_)
{
	if (pMsg_) {
		unsigned int nMsgLen = (unsigned int)strlen(pMsg_);
		unsigned char * pData = new unsigned char[nMsgLen + 1];
		memcpy_s(pData, nMsgLen + 1, pMsg_, nMsgLen);
		pData[nMsgLen] = '\0';
		char szMsgType[16] = { 0 };
		sprintf_s(szMsgType, sizeof(szMsgType), "%hu", msgType_);
		char szMsgEncrypt[16] = { 0 };
		sprintf_s(szMsgEncrypt, sizeof(szMsgEncrypt), "%hu", m_usMsgEncrypt);
		if (m_usMsgEncrypt > 0) {
			encryptMessage(pData, 0, nMsgLen, m_usMsgEncrypt);
		}
		zmsg_t * msg = zmsg_new();
		zframe_t * frame_type = zframe_from(szMsgType);
		zframe_t * frame_encrypt = zframe_from(szMsgEncrypt);
		zframe_t * frame_data = zframe_new(pData, nMsgLen);
		zmsg_append(msg, &frame_type);
		zmsg_append(msg, &frame_encrypt);
		zmsg_append(msg, &frame_data);
		if (1) {
			std::lock_guard<std::mutex> lk(m_mutex4Pipeline);
			zmsg_send(&msg, m_pipeline);
		}
		delete[] pData;
		pData = NULL;
	}
}

void CcDeviceProxy::encryptMessage(unsigned char * pData_, unsigned int begin_, unsigned int end_, 
	unsigned short key_)
{
	if (key_ > 0) {
		if (pData_ && end_ > begin_) {
			for (unsigned int i = begin_; i < end_; i++) {
				pData_[i] += 1;
				pData_[i] ^= key_;
			}
		}
	}
}

void CcDeviceProxy::decryptMessage(unsigned char * pData_, unsigned int begin_, unsigned int end_, 
	unsigned short key_)
{
	if (key_ > 0) {
		if (pData_ && end_ > begin_) {
			for (unsigned int i = begin_; i < end_; i++) {
				pData_[i] ^= key_;
				pData_[i] -= 1;
			}
		}
	}
}

void CcDeviceProxy::formatDatetime(unsigned long long ullDateTime_, char * pDateTime_, size_t nSize_)
{
	struct tm t;
	localtime_s(&t, (const time_t *)&ullDateTime_);
	char szDatetime[20] = { 0 };
	sprintf_s(szDatetime, sizeof(szDatetime), "%04d%02d%02d%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1,
		t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
	if (pDateTime_ && nSize_) {
		size_t nLen = strlen(szDatetime);
		strncpy_s(pDateTime_, nSize_, szDatetime, nSize_ > nLen ? nSize_ : nLen);
	}
}

unsigned long long CcDeviceProxy::makeDatetime(const char * pDateTime_)
{
	if (pDateTime_) {
		struct tm tm_time;
		sscanf_s(pDateTime_, "%04d%02d%02d%02d%02d%02d", &tm_time.tm_year, &tm_time.tm_mon, &tm_time.tm_mday,
			&tm_time.tm_hour, &tm_time.tm_min, &tm_time.tm_sec);
		tm_time.tm_year -= 1900;
		tm_time.tm_mon -= 1;
		return mktime(&tm_time);
	}
	return 0;
}

void CcDeviceProxy::addDisLink(std::string strDisLink_)
{
	if (!strDisLink_.empty()) {
		std::lock_guard<std::mutex> lk(m_mutex4DisLinkQue);
		m_disLinkQue.emplace(strDisLink_);
		if (m_disLinkQue.size() == 1) {
			m_cond4DisLinkQue.notify_all();
		}
	}
}

void CcDeviceProxy::dealDisLink()
{
	while (1) {
		std::unique_lock<std::mutex> lk(m_mutex4DisLinkQue);
		m_cond4DisLinkQue.wait(lk, [&] {
			return (!m_nRun || !m_disLinkQue.empty());
		});
		if (!m_nRun && m_disLinkQue.empty()) {
			lk.unlock();
			break;
		}
		if (!m_disLinkQue.empty()) {
			std::string disLink = m_disLinkQue.front();
			m_disLinkQue.pop();
			lk.unlock();
			handleDisLink(disLink.c_str());
		}
	}
}
