#include "ReliUDP.h"



#ifdef CHECK_SUM
inline uint32_t calcCheckSum(ReliUDP *godFather, fragment *frame, int size) {
    frame->checkSum = 0;
    uint32_t ckcSum = godFather->crcObj.check(frame, size);
    frame->checkSum = ckcSum;
    return ckcSum;
}
#endif

//IPortSeq  == operator
bool operator==(const IPortSeq &a, const IPortSeq &b) {
    return (a.IP == b.IP && a.port == b.port && a.SeqID == b.SeqID);
}


SOCKADDR_IN ReliUDP::getAddr(string ip, int port) {
    SOCKADDR_IN addr;
    memset(&addr, 0, sizeof(SOCKADDR_IN));
    addr.sin_family = AF_INET;
    addr.sin_addr.S_un.S_addr = inet_addr(ip.data());
    addr.sin_port = htons(port);
    return addr;
}

ReliUDP::ReliUDP(void) {
    if(winSockInit() == SOCK_INIT_FAIL) {
        cout << "socket init fail" << endl;
    }
    memset(&localAddr, 0, sizeof(localAddr));
    stat = COM_OFF;
    sendCount = 0;
    threadNum = 0;
    sendSpeedRate = MaxSpeedRate;
#ifdef RESEND_COUNT
    resendCount = 0;
    sendTotalCount = 0;
#endif
}


ReliUDP::~ReliUDP(void) {
}


bool ReliUDP::winSockInit() {
    WORD wVersionRequested;
    WSADATA wsaData;
    wVersionRequested = MAKEWORD( 2, 2 );
    int err = WSAStartup( wVersionRequested, &wsaData );
    if ( err != 0 ) { 		//winSock Error
        cout << "WSAStartup Fail" << endl;
        return SOCK_INIT_FAIL;
    }
    if ( LOBYTE( wsaData.wVersion ) != 2 || HIBYTE( wsaData.wVersion ) != 2) {  //version check
        WSACleanup( );
        cout << "WSAStartup WORD Fail" << endl;
        return SOCK_INIT_FAIL;
    }
    return SOCK_INIT_OK;
}

void ReliUDP::setLocalAddr(string ip, int port) {
    localIP = ip;
    localPort = port;
}

void ReliUDP::setSendSpeed(DWORD sp) {
    sendSpeedRate = sp;
}


void ReliUDP::startCom() {
    if(stat)	//already started
        return;
    //set local addr
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(localPort);
    localAddr.sin_addr.S_un.S_addr = inet_addr(localIP.data());
    //init socket
    if((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        std::cout << "Socket Error!" << endl;
    
    if(bind(sock, (sockaddr *) &localAddr, sizeof(sockaddr)) < 0)
    std::cout << "Bind Error!" << endl;
    
    DWORD nonBlocking = 1;
    if(ioctlsocket(sock, FIONBIO, &nonBlocking) != 0) {	 //set non-blocking socket
        std::cout <<  "Set Non-Blocking Error!"  << endl;
    }
    //set OS socket bufferSize
    int buffsize = OSBufferSize; // 64k
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&buffsize, sizeof(buffsize));	//set OS SEND buffer size
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&buffsize, sizeof(buffsize));	//set OS RECV buffer size
    //Initialize Critical Section
    InitializeCriticalSection(&sendMutex);
    InitializeCriticalSection(&sendStatMutex);
    InitializeCriticalSection(&bufMutex);
    InitializeCriticalSection(&sendCountMutex);
    InitializeCriticalSection(&messageSeqIdMutex);
    InitializeCriticalSection(&threadNumMutex);
    InitializeCriticalSection(&recvStatMutex);
    InitializeCriticalSection(&rttTestMutex);
    InitializeCriticalSection(&rttTestRecvMutex);
    InitializeCriticalSection(&udpRecvMutex);
    InitializeCriticalSection(&udpSendMutex);
    InitializeCriticalSection(&sendFlowCntMutex);
    InitializeCriticalSection(&responseFlowCntMutex);
    InitializeCriticalSection(&speedMutex);
#ifdef RESEND_COUNT
    InitializeCriticalSection(&resendCountMutex);
#endif
    //start recvThread[]
    stat = COM_ON;
    unsigned dwThreadID;
    for(int i = 0; i < RecvThreadNum; ++i) {
        recvThreadHandle[i] = (HANDLE)_beginthreadex(NULL, 0, &recvThread, (LPVOID) this, 0, &dwThreadID);
        if(recvThreadHandle[i] == 0) {
            cout << "create recv Thread Error" << endl;
            return;
        }
    }
}

void ReliUDP::resetCom(SOCKADDR_IN addr) {
    resetWaitFlag = true;
    int waitTime = MaxTestTimes;
    do {
        sendReset(addr);
        Sleep(50);
    }
    while(resetWaitFlag && waitTime--);
}

void ReliUDP::stopCom() {
    //wait for send thread
    while(sendCount)
        Sleep(20);
    stat = COM_OFF;
    //wait for recvThread[] to terminate
    for(int i = 0; i < RecvThreadNum; ++i) {
        while(1) {
            DWORD res = WaitForSingleObject(recvThreadHandle[i], 0);
            if(res == WAIT_OBJECT_0)
                break;
        }
    }
    //delete critical section
    DeleteCriticalSection(&sendMutex);
    DeleteCriticalSection(&sendStatMutex);
    DeleteCriticalSection(&sendCountMutex);
    DeleteCriticalSection(&bufMutex);
    DeleteCriticalSection(&messageSeqIdMutex);
    DeleteCriticalSection(&threadNumMutex);
    DeleteCriticalSection(&rttTestMutex);
    DeleteCriticalSection(&rttTestRecvMutex);
    DeleteCriticalSection(&udpRecvMutex);
    DeleteCriticalSection(&udpSendMutex);
    DeleteCriticalSection(&sendFlowCntMutex);
    DeleteCriticalSection(&responseFlowCntMutex);
    DeleteCriticalSection(&speedMutex);
#ifdef RESEND_COUNT
    DeleteCriticalSection(&resendCountMutex);
#endif
    closesocket(sock);
}

void ReliUDP::clearCom() {
    stopCom();
    WSACleanup();
}

int ReliUDP::testRTT(SOCKADDR_IN addr) {
    DWORD sendTime[MaxTestTimes];
    uint16_t seq = 0;
    DWORD *recvTime = new DWORD[MaxTestTimes];
    memset(recvTime, 0, sizeof(DWORD)*MaxTestTimes);

    EnterCriticalSection(&rttTestMutex);
    RTTRecvTime = recvTime; //set address
    RTTRecvCount = 0;
    while(RTTRecvCount < 8 && seq < MaxTestTimes) {
        sendTime[seq] = GetTickCount();
        sendRTTTest(addr, seq++);
        Sleep(25);
    }
    if(!RTTRecvCount) {
        LeaveCriticalSection(&rttTestMutex);
        delete[] recvTime;
        return -1;		//Error: caused by  DWORD time overflow
    }
    DWORD totalTime = 0;
    for(int i = 0; i < MaxTestTimes; ++i) {
        if(recvTime[i])
            totalTime += recvTime[i] - sendTime[i];
    }
    DWORD RTT = (totalTime + (RTTRecvCount >> 1)) / RTTRecvCount;
    LeaveCriticalSection(&rttTestMutex);
    delete[] recvTime;
    return RTT;
}

void ReliUDP::udpSendData(const char *dat, int dataLength, SOCKADDR_IN addr) {
    fd_set writeFD;
    timeval timeout;
    timeout.tv_sec = SendSelectTimeoutTime;
    timeout.tv_usec = 0;
    FD_ZERO(&writeFD);
    FD_SET(sock, &writeFD);
    EnterCriticalSection(&udpSendMutex);
    int ret = select(sock + 1, NULL, &writeFD, NULL, &timeout);
    if(ret <= 0) {	//error or send block
        LeaveCriticalSection(&udpSendMutex);
        return;
    }
    if(sendto(sock, dat, dataLength, 0, (sockaddr *)&addr, sizeof(sockaddr)) < 0) {
#ifdef DEBUG
        std::cout << "Send Error: " << WSAGetLastError() << endl;
#endif
    }
    LeaveCriticalSection(&udpSendMutex);
}

int ReliUDP::udpRecvData(char *buf, int dataLength, SOCKADDR_IN *addr) {
    fd_set readFD;
    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = RecvSelectTimeoutTime;
    FD_ZERO(&readFD);
    FD_SET(sock, &readFD);
    EnterCriticalSection(&udpRecvMutex);
    int ret = select(sock + 1, &readFD, NULL, NULL, &timeout);
    if(ret <= 0) {	//error or no data to read
        LeaveCriticalSection(&udpRecvMutex);
        return 0;
    }
    int res = sizeof(sockaddr);
    if((res = recvfrom(sock, buf, dataLength, 0, (sockaddr *)addr, &res)) < 0) {
#ifdef DEBUG
        std::cout << "Recv Error: " << WSAGetLastError() << endl;
#endif
    }
    LeaveCriticalSection(&udpRecvMutex);
    return res;
}


void ReliUDP::sendData(const char *dat, int dataLength, SOCKADDR_IN addr, char sendOpt, int RTT) {
    EnterCriticalSection(&messageSeqIdMutex);
    uint32_t seqID = ST.getIPortSeq(&addr);
    LeaveCriticalSection(&messageSeqIdMutex);
    EnterCriticalSection(&sendCountMutex);
    ++sendCount;
    LeaveCriticalSection(&sendCountMutex);
    if((sendOpt & SEND_BLOCK_CHECK) == SEND_UNBLOCK) {	//unblock send threadNum check
        EnterCriticalSection(&threadNumMutex);
        if(threadNum >= MaxThread)
            sendOpt |= SEND_BLOCK;
        else
            ++threadNum;
        LeaveCriticalSection(&threadNumMutex);
    }
    sendPara *para = new sendPara(this, dat, dataLength, seqID, addr, sendOpt, RTT);
    if((sendOpt & SEND_BLOCK_CHECK) == SEND_BLOCK) {	//block send
        sendDataThread(para);
    }
    else {	//unblock send
        EnterCriticalSection(&sendMutex);
        dataCopyingFlag = true;
        unsigned dwThreadID;
        HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, &sendDataThread, (LPVOID) para, 0, &dwThreadID);
        CloseHandle(hThread);
        while(dataCopyingFlag)
            Sleep(5);
        LeaveCriticalSection(&sendMutex);
    }
}


unsigned __stdcall sendDataThread(LPVOID data) {
    //parameters prepare
    sendPara *para = (sendPara *)data;
    ReliUDP *godFather = para->godFather;
    int dataLength = para->dataLength;
    //prepare data
    const char *dat;
    if((para->sendOpt & SEND_BLOCK_CHECK) == SEND_BLOCK)	//block send
        dat = para->dat;
    else {	//unblock send new mem
        dat = new char[dataLength];
        memcpy((void *)dat, para->dat, dataLength);
        godFather->dataCopyingFlag = false;
    }
    int fragmentCount = dataLength / FragmentDataSize + ((dataLength % FragmentDataSize) != 0);
    //the sending frame
    fragment frame;
    frame.type = FRAGMENT_DATA;
    frame.messageSeqID = para->messageSeqId;	//note: this is the real messageSeqId godfather->messageSeqID maybe not !
    frame.dataSize = dataLength;
#ifdef RESEND_COUNT
    EnterCriticalSection(&godFather->resendCountMutex);
    godFather->sendTotalCount += fragmentCount;
    LeaveCriticalSection(&godFather->resendCountMutex);
#endif
    EnterCriticalSection(&godFather->sendStatMutex);
    //set the send stat
    godFather->ST.newSeq(&frame, &para->addr);	//remote addr
    godFather->ST.allSet(&frame, &para->addr);
    LeaveCriticalSection(&godFather->sendStatMutex);
    //calculate timeout & sleep
    DWORD timeoutTime = GetTickCount() + SendTimeout + fragmentCount * SendTimeoutFactor;
    DWORD noReceiverTimeout = GetTickCount() + SendNoReceiverTimeout;	//caused by no valid receiver
    vector<int> queue;
    uint32_t prevSize = 0;
    bool flag = false;
    uint32_t expectedSize = 0;
    DWORD speedTimer;
    DWORD sendStartTime = GetTickCount();
    DWORD tmpTime;
    size_t fragIdCur = 0;
    while(1) {
        queue.clear();	//let me make it clear
        bool over = false;
        EnterCriticalSection(&godFather->sendStatMutex);
        queue = godFather->ST.getAll(&frame, &para->addr);	//check the send stat	get all un-responded frame
        LeaveCriticalSection(&godFather->sendStatMutex);
        if(queue.empty()) {	//all clear
#ifdef DEBUG
            cout << "[send complete: " << frame.messageSeqID << "]" << endl;
#endif
            over = true;
        }
        if(checkTimeout(timeoutTime) || (queue.size() == fragmentCount && checkTimeout(noReceiverTimeout))) {	//timoutCheck
#ifdef DEBUG
            cout << "[Send Abort: " << frame.messageSeqID << "]" << endl;
#endif
            over = true;
        }
        if(over) {
            EnterCriticalSection(&godFather->sendStatMutex);
            godFather->ST.removeSeq(&frame, &para->addr);	//remove seq from SEND_STAT
            LeaveCriticalSection(&godFather->sendStatMutex);
            EnterCriticalSection(&godFather->sendCountMutex);
            --godFather->sendCount;
            if(!godFather->sendCount) { //send over reset flow control  CNT
                godFather->sendFlowCnt = 0;
                godFather->responseFlowCnt = 0;
            }
            LeaveCriticalSection(&godFather->sendCountMutex);
            if((para->sendOpt & SEND_BLOCK_CHECK) == SEND_UNBLOCK) {
                delete[] (char *)dat; //unblock send -> free mem
                EnterCriticalSection(&godFather->threadNumMutex);
                --godFather->threadNum;
                LeaveCriticalSection(&godFather->threadNumMutex);
            }
            delete data;
            return 0;
        }

        //flow control
        tmpTime = GetTickCount(); //time count from send start
        if(tmpTime - sendStartTime > para->RTT) { //after RTT
            EnterCriticalSection(&godFather->sendFlowCntMutex);
            EnterCriticalSection(&godFather->responseFlowCntMutex);
            if(godFather->sendSpeedRate > MinSpeedRate  && tmpTime - godFather->time > 500 && godFather->responseFlowCnt < int(godFather->sendFlowCnt * ExpectRate)) { //slow down
                godFather->sendSpeedRate = max(MinSpeedRate, godFather->sendSpeedRate >> 1);
                cout << "speed down:" << godFather->sendSpeedRate << endl;
                godFather->time = tmpTime;

            }
            else if(godFather->sendSpeedRate < MaxSpeedRate &&  tmpTime - godFather->time > 500 && godFather->responseFlowCnt >= int(godFather->sendFlowCnt * ExpectRate)) { //speed up
                godFather->sendSpeedRate = min(MaxSpeedRate, godFather->sendSpeedRate << 1);
                cout << "speed up:" << godFather->sendSpeedRate << endl;
                godFather->time = tmpTime;
            }
            godFather->responseFlowCnt += (godFather->responseFlowCnt < godFather->sendFlowCnt) ? 1 : -1; //equalizer
            LeaveCriticalSection(&godFather->responseFlowCntMutex);
            LeaveCriticalSection(&godFather->sendFlowCntMutex);
        }

        //send data
        int SendSampleCount = SendSampleSize / FragmentSize + 1;
        int lastDataSize = dataLength - (fragmentCount - 1) * FragmentDataSize;
        int lastFrameSize = lastDataSize + FragmentHeaderSize;

        EnterCriticalSection(&godFather->sendFlowCntMutex);
        godFather->sendFlowCnt += SendSampleCount <= queue.size() ? SendSampleCount : queue.size();
        LeaveCriticalSection(&godFather->sendFlowCntMutex);

        EnterCriticalSection(&godFather->speedMutex);
        speedTimer = GetTickCount() + 1 + 1000 * (SendSampleSize / double(godFather->sendSpeedRate));
        LeaveCriticalSection(&godFather->speedMutex);
        for(size_t i = 0; i < SendSampleCount && i < queue.size(); ++i) {
            while(fragIdCur >= queue.size())	//mod
                fragIdCur -= queue.size();
            frame.fragmentID = queue[fragIdCur];
            int dataSize = FragmentDataSize;
            int frameSize = FragmentSize;
            if(queue[fragIdCur] == fragmentCount - 1) { //last one
                dataSize = lastDataSize;
                frameSize = lastFrameSize;
            }
            memcpy(frame.data, dat + queue[fragIdCur]*FragmentDataSize, dataSize);
#ifdef CHECK_SUM
            //checkSum
            calcCheckSum(godFather, &frame, frameSize);
#endif
            godFather->udpSendData((const char *)&frame, frameSize, para->addr);
            ++fragIdCur;
#ifdef DEBUG_RS
            cout << "[resend: -->>] " << frame.messageSeqID << "--" << frame.fragmentID << endl;
#endif
#ifdef RESEND_COUNT
            EnterCriticalSection(&godFather->resendCountMutex);
            ++godFather->resendCount;
            LeaveCriticalSection(&godFather->resendCountMutex);
#endif
        }
        //speed control
        tmpTime = GetTickCount();
        if(tmpTime < speedTimer) {
            tmpTime = speedTimer - tmpTime;
            Sleep(tmpTime);
        }
    }
}

void ReliUDP::sendReset(SOCKADDR_IN addr) {
    fragment resFrame;
    resFrame.type = FRAGMENT_RESET;
#ifdef CHECK_SUM
    //checkSum
    calcCheckSum(this, &resFrame, ResetSize);
#endif
    udpSendData((const char *)&resFrame, ResetSize, addr);
}

void ReliUDP::sendResetResponse(SOCKADDR_IN addr) {
    fragment resFrame;
    resFrame.type = FRAGMENT_RESET_RESPONSE;
#ifdef CHECK_SUM
    //checkSum
    calcCheckSum(this, &resFrame, ResetSize);
#endif
    udpSendData((const char *)&resFrame, ResetSize, addr);
}

void ReliUDP::sendRTTTest(SOCKADDR_IN addr, uint16_t seq) {
    fragment resFrame;
    resFrame.type = FRAGMENT_RTT_TEST;
    resFrame.messageSeqID = seq;
#ifdef CHECK_SUM
    //checkSum
    calcCheckSum(this, &resFrame, TestRTTSize);
#endif
    udpSendData((const char *)&resFrame, TestRTTSize, addr);
}

void ReliUDP::sendRTTResponse(SOCKADDR_IN addr, uint16_t seq) {
    fragment resFrame;
    resFrame.type = FRAGMENT_RTT_TEST_RESPONSE;
    resFrame.messageSeqID = seq;
#ifdef CHECK_SUM
    //checkSum
    calcCheckSum(this, &resFrame, TestRTTSize);
#endif
    udpSendData((const char *)&resFrame, TestRTTSize, addr);
}

void ReliUDP::sendResponse(fragment *frame, SOCKADDR_IN addr) {
    //the response frame
    fragment resFrame;
    resFrame.type = FRAGMENT_RESPONSE;
    resFrame.messageSeqID = frame->messageSeqID;
    resFrame.fragmentID = frame->fragmentID;
#ifdef CHECK_SUM
    //checkSum
    calcCheckSum(this, &resFrame, ResponseSize);
#endif
    udpSendData((const char *)&resFrame, ResponseSize, addr);
}

int getFrameLength(fragment *frame) {
    switch(frame->type) {
        case FRAGMENT_RESPONSE:
            return ResponseSize;
            break;
        case FRAGMENT_RESET:
        case FRAGMENT_RESET_RESPONSE:
            return ResetSize;
        case FRAGMENT_RTT_TEST:
        case FRAGMENT_RTT_TEST_RESPONSE:
            return TestRTTSize;
        case FRAGMENT_DATA: {
            int fragmentCount = frame->dataSize / FragmentDataSize + ((frame->dataSize % FragmentDataSize) != 0);
            return frame->fragmentID == fragmentCount - 1 ? FragmentHeaderSize + frame->dataSize - (fragmentCount - 1) * FragmentDataSize : FragmentSize;
        }
        default:
            frame->type = FRAGMENT_INVALID;
            return 0;
    }
}


unsigned __stdcall recvThread(LPVOID data) {
    ReliUDP *godFather = (ReliUDP *)data;
    const int bufSize = FragmentSize;	//Data Content + Header
    char buf[bufSize];
    int recvLen, dataLen;
    fragment *frame;					//to form a frame
    SOCKADDR_IN tmpRemoteAddr;
    int timeCnt = 0;
    while(godFather->stat) {			//check service stat
        recvLen = godFather->udpRecvData(buf, bufSize, &tmpRemoteAddr);
        if(recvLen >= MinPacketSize) {	//basic requirement to be a ReliUDP frame
            frame = (fragment *)buf;	//recognize it as a frame
            int frameLen = getFrameLength(frame);
            //checkSum
#ifdef CHECK_SUM
            uint8_t checkSum = frame->checkSum;
            uint8_t realCkcSum = calcCheckSum(godFather, frame, frameLen);
            if(checkSum != realCkcSum) {
                frame->type = FRAGMENT_INVALID;	//mark as invalid fragment
            }
#endif
            switch(frame->type) {
                case FRAGMENT_RESPONSE:
#ifdef DEBUG_RS
                    cout << "[resp: <<--] " << frame->messageSeqID << "--" << frame->fragmentID << endl;
#endif
                    EnterCriticalSection(&godFather->responseFlowCntMutex);
                    ++godFather->responseFlowCnt;							//inc response CNT
                    LeaveCriticalSection(&godFather->responseFlowCntMutex);

                    EnterCriticalSection(&godFather->sendStatMutex);
                    godFather->ST.reset(frame, &tmpRemoteAddr);				//reset
                    LeaveCriticalSection(&godFather->sendStatMutex);
                    break;
                case FRAGMENT_RESET:
                    EnterCriticalSection(&godFather->recvStatMutex);
                    godFather->RT.clearSeqSet(&tmpRemoteAddr);				//clear SEQ buffer Set to reset
                    LeaveCriticalSection(&godFather->recvStatMutex);
                    godFather->sendResetResponse(tmpRemoteAddr);			//send response
                    break;
                case FRAGMENT_RESET_RESPONSE:
                    godFather->resetWaitFlag = false;						//reset RESET flag
                    break;
                case FRAGMENT_RTT_TEST:
                    godFather->sendRTTResponse(tmpRemoteAddr, frame->messageSeqID);	//just send a response
                    break;
                case FRAGMENT_RTT_TEST_RESPONSE:
                    EnterCriticalSection(&godFather->rttTestRecvMutex);
                    ++godFather->RTTRecvCount;
                    godFather->RTTRecvTime[frame->messageSeqID] = GetTickCount();
                    LeaveCriticalSection(&godFather->rttTestRecvMutex);
                    break;
                case FRAGMENT_DATA:
#ifdef DEBUG_RS
                    cout << "[recv: ----] " << frame->messageSeqID << "--" << frame->fragmentID << endl;
#endif
                    //whatever sender needs a response
                    godFather->sendResponse(frame, tmpRemoteAddr);
                    EnterCriticalSection(&godFather->recvStatMutex);
                    //check duplicate message
                    if(godFather->RT.checkSeqId(frame, &tmpRemoteAddr)) {	//duplicate message
                        //do nothing
                        LeaveCriticalSection(&godFather->recvStatMutex);
                        break;
                    }
                    if(!godFather->RT.have(frame, &tmpRemoteAddr)) {	//new entry
                        godFather->RT.newSeq(frame, &tmpRemoteAddr);
                    }
                    //let's do it
                    dataLen = frameLen - FragmentHeaderSize;
                    if(godFather->RT.check(frame, &tmpRemoteAddr)) {				//make sure this frame hasn't been received
                        godFather->RT.storeData(frame, &tmpRemoteAddr, dataLen);	//storeData & set Bit
                        //check if all received
                        if(godFather->RT.getOne(frame, &tmpRemoteAddr) == -1) {		//all received
                            //add messageSeqID
                            godFather->RT.addSeqId(frame, &tmpRemoteAddr);
                            EnterCriticalSection(&godFather->bufMutex);
                            godFather->BUF.addEntry(godFather->RT.getDataPointer(frame, &tmpRemoteAddr), frame->dataSize, tmpRemoteAddr);
                            LeaveCriticalSection(&godFather->bufMutex);
                            godFather->RT.removeSeq(frame, &tmpRemoteAddr);
#ifdef DEBUG
                            cout << "[ID:" << frame->messageSeqID << " received]" << endl;
#endif
                        }
                    }
                    LeaveCriticalSection(&godFather->recvStatMutex);
                    break;
                case FRAGMENT_INVALID:	//do nothing  :discard
#ifdef DEBUG
                    cout << "[invalid frame]" << endl;
#endif
                    break;
            }
            if(!((timeCnt++) & 0xff)) { //256 recv => 1 remove timeout entry
                EnterCriticalSection(&godFather->recvStatMutex);
                godFather->RT.removeTimeout();
                LeaveCriticalSection(&godFather->recvStatMutex);
            }
        }
    }
    return 0;
}


int ReliUDP::recvData(char *buf, int dataLength, SOCKADDR_IN *addr) {
    while(BUF.empty())
        Sleep(RecvBUFWait);
    EnterCriticalSection(&bufMutex);
    int ret = BUF.popEntry(buf, addr);
    LeaveCriticalSection(&bufMutex);
    return ret;
}

uint32_t ReliUDP::getNextDataLength() {
    return BUF.getHeadSize();
}
