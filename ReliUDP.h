#pragma once

#include <iostream>
#include <WinSock2.h>
#include <process.h>
#include <list>
#include <vector>
#include <queue>
#include <time.h>
#include <Windows.h>


//#define DEBUG
//#define DEBUG_RS
//#define MUTEX_TIMEOUT		
#define RESEND_COUNT
#define CHECK_SUM

#define MAX_THREAD 12

#define FRAGMENT_DATA_SIZE (512)		//default 512
#define FRAGMENT_HEADER_SIZE 24		
#define FRAGMENT_SIZE (FRAGMENT_DATA_SIZE+FRAGMENT_HEADER_SIZE)

#define TIME_WAIT 12					//default 12	
#define TIME_WAIT_SIZE_FACTOR (0.008*(double(FRAGMENT_DATA_SIZE)/10240.0))	//(0.008*(double(FRAGMENT_DATA_SIZE)/10240.0))			ms/frame

#define SEND_SAMPLE_SIZE (1024*8)		//default 8k

#define SEND_TIMEOUT 3000				//default 12k
#define SEND_TIMEOUT_FACTOR 50			//default 200  ms/frame
#define SEND_NORECEIVER_TIMEOUT 500		//default 500

#define EXPECT_RATE .95					//default 95%
#define EXPECT_TIMEOUT 32				//default 32 ms
#define EXPECT_EXCEPTION_SIZE 12		//default 12

#define RECV_SEQID_BUFFER_SIZE 200		//default 200

#define RECV_BUF_WAIT 20				//default 20

#define FRAGMENT_DATA 17953
#define FRAGMENT_RESPONSE 38761
#define FRAGMENT_INVALID 55479
 
#define FRAGMENT_TIMEOUT 12000			//default 12k
#define FRAGMENT_TIMEOUT_FACTOR 2		//default 2ms/frame

#define MUTEX_WAIT_TIMEOUT 5000			//default 5k

#define ID_ERROR -2

#define SOCK_INIT_FAIL false
#define SOCK_INIT_OK true

#define SEND_BLOCK 1
#define SEND_UNBLOCK 0
#define SEND_BLOCK_CHECK 1	//bit 0

using namespace std;

typedef unsigned int uint32_t;

typedef struct _fragmentST{
	uint32_t type;			//DATA / RESPONSE / INVALID
	uint32_t messageSeqID;	
	uint32_t fragmentID;
	uint32_t fragmentNum;
	uint32_t dataSize;
	uint32_t checkSum;
	char data[FRAGMENT_DATA_SIZE];	
}fragment;


class bitSet{
public:
	bitSet(int n){
		N=n;
		size=(N>>3)+((N&7)!=0); //about N/8  bytes
		dat=new char[size];
		memset(dat,0,size);
	}
	~bitSet(){
		delete[] dat;
	}
	void set(int pos){
		if(pos<N)
			dat[pos>>3]|=1<<(pos&7);
	}
	void reset(int pos){
		if(pos<N)
			dat[pos>>3]&=-1^(1<<(pos&7));
	}
	void allSet(){
		memset(dat,-1,size);
	}
	void allReset(){
		memset(dat,0,size);
	}
	bool check(int pos){		
		return pos<N ? (dat[pos>>3]&(1<<(pos&7))) != 0 : false;
	}
	bool bigCheck(int posB){	//check 1 byte as 8-bit ...faster than check 8 single bit
		return posB<size ? (dat[posB]) != 0 : false;
	}
	int getOne(){
		int i;
		for(i=0;i<(N>>3);++i)
			if(bigCheck(i))
				for(int j=i<<3;j<(i<<3)+8;++j)
					if(check(j))
						return j;
		for(i<<=3;i<N;++i)
			if(check(i))
				return i;
		return -1;
	}
	vector<int> getAll(){
		vector<int> res;
		int i;
		for(i=0;i<(N>>3);++i)
			if(bigCheck(i))
				for(int j=i<<3;j<(i<<3)+8;++j)
					if(check(j))
						res.push_back(j);
		for(i<<=3;i<N;++i)
			if(check(i))
				res.push_back(i);
		return res;
	}
private:
	char *dat;
	int N;
	int size;
};


class statEntry{
public:
	statEntry(int i,int n){
		id=i;
		bits=new bitSet(n);
	}
	~statEntry(){
		delete bits;
	}
	int id;
	bitSet *bits;
};


class sendStat{
public:
	sendStat(){
		indexSet.clear();
	}
	~sendStat(){
		indexSet.clear();
	}
	list<statEntry>::iterator find(int id){
		for(list<statEntry>::iterator it=indexSet.begin();it!=indexSet.end();++it){
			if(it->id==id)
				return it;
		}
		return indexSet.end();
	}
	void newSeq(fragment *frame){
		list<statEntry>::iterator it=find(frame->messageSeqID);
		if(it==indexSet.end()){
			statEntry *entry=new statEntry(frame->messageSeqID,frame->fragmentNum);
			indexSet.push_back(*entry);
		}
	}
	void set(fragment *frame){
		list<statEntry>::iterator it=find(frame->messageSeqID);
		if(it!=indexSet.end())
			it->bits->set(frame->fragmentID);		
	}
	void reset(fragment *frame){
		list<statEntry>::iterator it=find(frame->messageSeqID);
		if(it!=indexSet.end())
			it->bits->reset(frame->fragmentID);		
	}
	void allSet(fragment *frame){
		list<statEntry>::iterator it=find(frame->messageSeqID);
		if(it!=indexSet.end())
			it->bits->allSet();		
	}
	void allReset(int id){
		list<statEntry>::iterator it=find(id);
		if(it!=indexSet.end())
			it->bits->allReset();		
	}
	int getOne(int id){
		list<statEntry>::iterator it=find(id);
		if(it!=indexSet.end())
			return it->bits->getOne();		
		return ID_ERROR;
	}
	vector<int> getAll(fragment *frame){
		list<statEntry>::iterator it=find(frame->messageSeqID);
		if(it != indexSet.end())
			return it->bits->getAll();
		vector<int> empty;
		return empty;
	}
	void removeSeq(fragment *frame){
		list<statEntry>::iterator it=find(frame->messageSeqID);
		if(it!=indexSet.end()){
			indexSet.erase(it);	
		}
	}

private:
	list<statEntry> indexSet;

};



class recvEntry{
public:
	recvEntry(int i,int n,int dataSize){
		id=i;
		size=n;
		bits=new bitSet(n);
		bits->allSet();	//initial 0xFF
		data=new char[dataSize];		
		timeoutTime=clock()+ FRAGMENT_TIMEOUT +FRAGMENT_TIMEOUT_FACTOR*n;
	}
	~recvEntry(){
		delete bits;
		delete[] data;
	}
	bool checkTimeout(clock_t now){
		return now>timeoutTime || now+1200000<timeoutTime;	//obvious timoutOut or overflow => timeout
	}
public:
	int id;	
	bitSet *bits;
	char *data;
	clock_t timeoutTime;
	int size;
};


class recvStat{
public:
	recvStat(){
		indexSet.clear();
		receivedSeqSet.clear();
	}
	~recvStat(){
		indexSet.clear();
		receivedSeqSet.clear();
	}	
	void addSeqId(fragment *frame){
		receivedSeqSet.push_back(frame->messageSeqID);
		if(receivedSeqSet.size()>RECV_SEQID_BUFFER_SIZE)	//buffer overflow remove head
			receivedSeqSet.erase(receivedSeqSet.begin());
	}	
	bool checkSeqId(fragment *frame){
		for(list<uint32_t>::iterator it=receivedSeqSet.begin();it!=receivedSeqSet.end();++it)
			if(*it==frame->messageSeqID)
				return true;
		return false;
	}
	list<recvEntry>::iterator find(int id){
		for(list<recvEntry>::iterator it=indexSet.begin();it!=indexSet.end();++it){
			if(it->id==id)
				return it;
		}
		return indexSet.end();
	}
	bool have(int id){
		return find(id)!=indexSet.end();
	}
	void removeTimeout(){
		clock_t now=clock();
		list<recvEntry>::iterator it=indexSet.begin();
		for(size_t i=0;i<indexSet.size();++i){			
			if(it->checkTimeout(now)){
#ifdef DEBUG
				cout<<"[timeout:"<<it->id<<"]"<<endl;
#endif
				indexSet.erase(it);				
				it=indexSet.begin();
				int t=i;
				while(t--)	++it;
				--i;
			}			
			else
				++it;
		}
	}
	void newSeq(fragment *frame){
		list<recvEntry>::iterator it=find(frame->messageSeqID);
		if(it==indexSet.end()){
			recvEntry *entry= new recvEntry(frame->messageSeqID,frame->fragmentNum,frame->dataSize);
			indexSet.push_back(*entry);
		}
	}	
	bool check(fragment *frame){
		list<recvEntry>::iterator it=find(frame->messageSeqID);
		if(it != indexSet.end()){
			return it->bits->check(frame->fragmentID);
		}
		return false;
	}
	void storeData(fragment *frame,int dataLen){
		list<recvEntry>::iterator it=find(frame->messageSeqID);
		if(it != indexSet.end()){
			memcpy((it->data)+(frame->fragmentID*FRAGMENT_DATA_SIZE),frame->data,dataLen);	
			it->bits->reset(frame->fragmentID);	//reset mark as received
		}
	}	
	const char* getDataPointer(int id){
		list<recvEntry>::iterator it=find(id);
		if(it != indexSet.end()){
			return it->data;
		}
		return NULL;
	}
	int getOne(int id){
		list<recvEntry>::iterator it=find(id);
		if(it!=indexSet.end())
			return it->bits->getOne();		
		return ID_ERROR;
	}
	void removeSeq(int id){
		list<recvEntry>::iterator it=find(id);
		if(it!=indexSet.end()){
			indexSet.erase(it);	
		}
	}


private:
	list<recvEntry> indexSet;
	list<uint32_t> receivedSeqSet;	
};

class bufEntry{
public:
	bufEntry(int n){
		data=new char[n];
		dataLength=n;
	}
	~bufEntry(){
		delete[] data;
	}	
	void storeData(const char *src,int dataLen){
		memcpy(data,src,dataLen);
	}
	int popData(char *des){
		memcpy(des,data,dataLength);
		return dataLength;
	}
private:
	char *data;
	int dataLength;
};

class recvBuffer{
public:
	recvBuffer(){
		clear();		
	}
	~recvBuffer(){
		clear();
	}
	bool empty(){
		return indexSet.empty();
	}
	void addEntry(const char *src,int datalen){		
		bufEntry *entry=new bufEntry(datalen);
		entry->storeData(src,datalen);
		indexSet.push(*entry);		
	}
	int popEntry(char *des){
		int len=indexSet.front().popData(des);
		indexSet.pop();
		return len;
	}
	void clear(){
		queue<bufEntry> empty;
		swap(empty,indexSet);
	}

private:
	queue<bufEntry> indexSet;	
};

class CRC32{
public:
	CRC32(){
		const uint32_t Polynomial = 0xEDB88320;
		for (unsigned int i = 0; i <= 0xFF; ++i){
			uint32_t crc = i;
			for (unsigned int j = 0; j < 8; ++j)
				crc = (crc >> 1) ^ (-int(crc & 1) & Polynomial);
			crc32Lookup[i] = crc;
		}
	}
	uint32_t check(const void* data, size_t length, uint32_t previousCrc32 = 0){
		uint32_t crc = ~previousCrc32;
		unsigned char* current = (unsigned char*) data;
		while(length--)
			crc = (crc >> 8) ^ crc32Lookup[(crc & 0xFF) ^ *current++];
		return ~crc;
	}
private:	
	uint32_t crc32Lookup[256];
};





class ReliUDP
{
public:
	ReliUDP(void);
	~ReliUDP(void);
	bool winSockInit();
	void setLocalPort(int port);
	void setRemoteAddr(string ip,int port);
	void setTempRemoteAddr(string ip,int port);
	void startCom();
	void stopCom();
	void clearCom();
	void udpSendData(const char *dat,int dataLength);
	int udpRecvData(char *buf,int dataLength);
	void sendData(const char *dat,int dataLength,char sendOpt=SEND_BLOCK);
	int recvData(char *buf,int dataLength);
	void sendResponse(fragment *frame);
#ifdef RESEND_COUNT
	void show(){

		cout<<resendCount<<endl;
	}
#endif

	friend unsigned __stdcall recvThread(LPVOID data);
	friend unsigned __stdcall sendDataThread(LPVOID data);
#ifdef CHECK_SUM
	friend uint32_t calcCheckSum(ReliUDP *godFather,fragment *frame,int size);
#endif

private:	
	int messageSeqID;
	SOCKADDR_IN localAddr;
	SOCKADDR_IN remoteAddr;
	int localPort;
	int remotePort;
	string remoteIP;
	SOCKET sock;
	sendStat ST;
	recvBuffer BUF;	
#ifdef CHECK_SUM
	CRC32 crcObj;
#endif
	bool stat;
	int dataCopyingFlag;
	HANDLE recvThreadHandle;
	HANDLE sendMutex;	
	HANDLE sendStatMutex;
	HANDLE bufMutex;
	HANDLE sendCountMutex;
	HANDLE messageSeqIdMutex;
	HANDLE threadNumMutex;
#ifdef RESEND_COUNT
	HANDLE resendCountMutex;
	uint32_t resendCount;
#endif
	uint32_t sendCount;
	clock_t lastSendTime;	
	int threadNum;
};

typedef struct _sendThreadPara{
	_sendThreadPara(ReliUDP *father,const char *d,uint32_t len,uint32_t id,char opt){
		godFather=father;
		dat=d;
		dataLength=len;
		messageSeqId=id;
		sendOpt=opt;
	}
	ReliUDP *godFather;
	const char *dat;
	uint32_t dataLength;
	uint32_t messageSeqId;
	char sendOpt;
}sendPara;
