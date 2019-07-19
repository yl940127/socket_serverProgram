//
// Created by yl on 19-6-20.
//

#ifndef HELLOSOCKET_EASYTCPSERVER_H
#define HELLOSOCKET_EASYTCPSERVER_H

#include<unistd.h> //uni std
#include<arpa/inet.h>
#include<string.h>

#define SOCKET int
#define INVALID_SOCKET  (SOCKET)(~0)
#define SOCKET_ERROR            (-1)


#include<stdio.h>
#include<vector>
#include<map>
#include<thread>
#include<mutex>
#include<atomic>
#include<memory>

#include"MessageHeader.h"
#include"CELLTimestamp.h"
#include"CELLTask.h"
#include"CELLObjectPool.h"

//缓冲区最小单元大小
#ifndef RECV_BUFF_SZIE
#define RECV_BUFF_SZIE 10240*5
#define SEND_BUFF_SZIE RECV_BUFF_SZIE
#endif // !RECV_BUFF_SZIE

typedef std::shared_ptr<DataHeader> DataHeaderPtr;
typedef std::shared_ptr<LoginResult> LoginResultPtr;

//客户端数据类型
class ClientSocket :public ObjectPoolBase<ClientSocket,1000>
{
public:
    ClientSocket(SOCKET sockfd = INVALID_SOCKET)
    {
        _sockfd = sockfd;
        memset(_szMsgBuf, 0, RECV_BUFF_SZIE);
        _lastPos = 0;

        memset(_szSendBuf, 0, SEND_BUFF_SZIE);
        _lastSendPos = 0;
    }

    SOCKET sockfd()
    {
        return _sockfd;
    }

    char* msgBuf()
    {
        return _szMsgBuf;
    }

    int getLastPos()
    {
        return _lastPos;
    }
    void setLastPos(int pos)
    {
        _lastPos = pos;
    }

    //发送数据
    int SendData(DataHeaderPtr& header)
    {
        int ret = SOCKET_ERROR;
        //要发送的数据长度
        int nSendLen = header->dataLength;
        //要发送的数据
        const char* pSendData = (const char*)header.get();

        while (true)
        {
            if (_lastSendPos + nSendLen >= SEND_BUFF_SZIE)
            {
                //计算可拷贝的数据长度
                int nCopyLen = SEND_BUFF_SZIE - _lastSendPos;
                //拷贝数据
                memcpy(_szSendBuf + _lastSendPos, pSendData, nCopyLen);
                //计算剩余数据位置
                pSendData += nCopyLen;
                //计算剩余数据长度
                nSendLen -= nCopyLen;
                //发送数据
                ret = send(_sockfd, _szSendBuf, SEND_BUFF_SZIE, 0);
                //数据尾部位置清零
                _lastSendPos = 0;
                //发送错误
                if (SOCKET_ERROR == ret)
                {
                    return ret;
                }
            }else {
                //将要发送的数据 拷贝到发送缓冲区尾部
                memcpy(_szSendBuf + _lastSendPos, pSendData, nSendLen);
                //计算数据尾部位置
                _lastSendPos += nSendLen;
                break;
            }
        }
        return ret;
    }

private:
    // socket fd_set  file desc set
    SOCKET _sockfd;
    //第二缓冲区 消息缓冲区
    char _szMsgBuf[RECV_BUFF_SZIE];
    //消息缓冲区的数据尾部位置
    int _lastPos;

    //第二缓冲区 发送缓冲区
    char _szSendBuf[SEND_BUFF_SZIE];
    //发送缓冲区的数据尾部位置
    int _lastSendPos;
};
typedef std::shared_ptr<ClientSocket> ClientSocketPtr;

class CellServer;
//网络事件接口
class INetEvent
{
public:
    //纯虚函数
    //客户端加入事件
    virtual void OnNetJoin(ClientSocketPtr& pClient) = 0;
    //客户端离开事件
    virtual void OnNetLeave(ClientSocketPtr& pClient) = 0;
    //客户端消息事件
    virtual void OnNetMsg(CellServer* pCellServer, ClientSocketPtr& pClient, DataHeader* header) = 0;
    //recv事件
    virtual void OnNetRecv(ClientSocketPtr& pClient) = 0;
private:

};

//网络消息发送任务
class CellS2CTask:public CellTask
{
    ClientSocketPtr _pClient;
    DataHeaderPtr _pHeader;
public:
    CellS2CTask(ClientSocketPtr& pClient, DataHeaderPtr& header)
    {
        _pClient = pClient;
        _pHeader = header;
    }

    //执行任务
    void doTask()
    {
        _pClient->SendData(_pHeader);
    }
};
typedef std::shared_ptr<CellS2CTask> CellS2CTaskPtr;

//网络消息接收处理服务类
class CellServer
{
public:
    CellServer(SOCKET sock = INVALID_SOCKET)
    {
        _sock = sock;
        _pNetEvent = nullptr;
    }

    ~CellServer()
    {
        Close();
        _sock = INVALID_SOCKET;
    }

    void setEventObj(INetEvent* event)
    {
        _pNetEvent = event;
    }

    //关闭Socket
    void Close()
    {
        if (_sock != INVALID_SOCKET)
        {
            for (auto iter : _clients)
            {
                close(iter.second->sockfd());
            }
            //关闭套节字closesocket
            close(_sock);

            _clients.clear();
        }
    }

    //是否工作中
    bool isRun()
    {
        return _sock != INVALID_SOCKET;
    }

    //处理网络消息
    //备份客户socket fd_set
    fd_set _fdRead_bak;
    //客户列表是否有变化
    bool _clients_change;
    SOCKET _maxSock;
    void OnRun()
    {
        _clients_change = true;
        while (isRun())
        {
            if (!_clientsBuff.empty())
            {//从缓冲队列里取出客户数据
                std::lock_guard<std::mutex> lock(_mutex);
                for (auto pClient : _clientsBuff)
                {
                    _clients[pClient->sockfd()] = pClient;
                }
                _clientsBuff.clear();
                _clients_change = true;
            }

            //如果没有需要处理的客户端，就跳过
            if (_clients.empty())
            {
                std::chrono::milliseconds t(1);
                std::this_thread::sleep_for(t);
                continue;
            }

            //伯克利套接字 BSD socket
            fd_set fdRead;//描述符（socket） 集合
            //清理集合
            FD_ZERO(&fdRead);
            if (_clients_change)
            {
                _clients_change = false;
                //将描述符（socket）加入集合
                _maxSock = _clients.begin()->second->sockfd();
                for (auto iter : _clients)
                {
                    FD_SET(iter.second->sockfd(), &fdRead);
                    if (_maxSock < iter.second->sockfd())
                    {
                        _maxSock = iter.second->sockfd();
                    }
                }
                memcpy(&_fdRead_bak, &fdRead, sizeof(fd_set));
            }
            else {
                memcpy(&fdRead, &_fdRead_bak, sizeof(fd_set));
            }

            ///nfds 是一个整数值 是指fd_set集合中所有描述符(socket)的范围，而不是数量
            ///既是所有文件描述符最大值+1 在Windows中这个参数可以写0
            int ret = select(_maxSock + 1, &fdRead, nullptr, nullptr, nullptr);
            if (ret < 0)
            {
                printf("select任务结束。\n");
                Close();
                return;
            }
            else if (ret == 0)
            {
                continue;
            }


            std::vector<ClientSocketPtr> temp;
            for (auto iter : _clients)
            {
                if (FD_ISSET(iter.second->sockfd(), &fdRead))
                {
                    if (-1 == RecvData(iter.second))
                    {
                        if (_pNetEvent)
                            _pNetEvent->OnNetLeave(iter.second);
                        _clients_change = true;
                        close(iter.first);
                        temp.push_back(iter.second);
                    }
                }
            }
            for (auto pClient : temp)
            {

                _clients.erase(pClient->sockfd());

            }
        }
    }
    //接收数据 处理粘包 拆分包
    int RecvData(ClientSocketPtr& pClient)
    {

        //接收客户端数据
        char* szRecv = pClient->msgBuf() + pClient->getLastPos();
        int nLen = (int)recv(pClient->sockfd(), szRecv, (RECV_BUFF_SZIE)- pClient->getLastPos(), 0);
        _pNetEvent->OnNetRecv(pClient);
        //printf("nLen=%d\n", nLen);
        if (nLen <= 0)
        {
            //printf("客户端<Socket=%d>已退出，任务结束。\n", pClient->sockfd());
            return -1;
        }
        //将收取到的数据拷贝到消息缓冲区
        //memcpy(pClient->msgBuf() + pClient->getLastPos(), _szRecv, nLen);
        //消息缓冲区的数据尾部位置后移
        pClient->setLastPos(pClient->getLastPos() + nLen);

        //判断消息缓冲区的数据长度大于消息头DataHeader长度
        while (pClient->getLastPos() >= sizeof(DataHeader))
        {
            //这时就可以知道当前消息的长度
            DataHeader* header = (DataHeader*)pClient->msgBuf();
            //判断消息缓冲区的数据长度大于消息长度
            if (pClient->getLastPos() >= header->dataLength)
            {
                //消息缓冲区剩余未处理数据的长度
                int nSize = pClient->getLastPos() - header->dataLength;
                //处理网络消息
                OnNetMsg(pClient, header);
                //将消息缓冲区剩余未处理数据前移
                memcpy(pClient->msgBuf(), pClient->msgBuf() + header->dataLength, nSize);
                //消息缓冲区的数据尾部位置前移
                pClient->setLastPos(nSize);
            }
            else {
                //消息缓冲区剩余数据不够一条完整消息
                break;
            }
        }
        return 0;
    }

    //响应网络消息
    virtual void OnNetMsg(ClientSocketPtr& pClient, DataHeader* header)
    {
        _pNetEvent->OnNetMsg(this, pClient, header);
    }

    void addClient(ClientSocketPtr& pClient)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        //_mutex.lock();
        _clientsBuff.push_back(pClient);
        //_mutex.unlock();
    }

    void Start()
    {
        _thread = std::thread(std::mem_fn(&CellServer::OnRun), this);
        _taskServer.Start();
    }

    size_t getClientCount()
    {
        return _clients.size() + _clientsBuff.size();
    }

    void addSendTask(ClientSocketPtr& pClient, DataHeaderPtr& header)
    {
        auto task = std::make_shared<CellS2CTask>(pClient, header);
        _taskServer.addTask((CellTaskPtr&)task);
    }
private:
    SOCKET _sock;
    //正式客户队列
    std::map<SOCKET,ClientSocketPtr> _clients;
    //缓冲客户队列
    std::vector<ClientSocketPtr> _clientsBuff;
    //缓冲队列的锁
    std::mutex _mutex;
    std::thread _thread;
    //网络事件对象
    INetEvent* _pNetEvent;
    //
    CellTaskServer _taskServer;
};

typedef std::shared_ptr<CellServer> CellServerPtr;
class EasyTcpServer : public INetEvent
{
private:
    SOCKET _sock;
    //消息处理对象，内部会创建线程
    std::vector<CellServerPtr> _cellServers;
    //每秒消息计时
    CELLTimestamp _tTime;
protected:
    //SOCKET recv计数
    std::atomic_int _recvCount;
    //收到消息计数
    std::atomic_int _msgCount;
    //客户端计数
    std::atomic_int _clientCount;
public:
    EasyTcpServer()
    {
        _sock = INVALID_SOCKET;
        _recvCount = 0;
        _msgCount = 0;
        _clientCount = 0;
    }
    virtual ~EasyTcpServer()
    {
        Close();
    }
    //初始化Socket
    SOCKET InitSocket()
    {

        if (INVALID_SOCKET != _sock)
        {
            printf("<socket=%d>关闭旧连接...\n", (int)_sock);
            Close();
        }
        _sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (INVALID_SOCKET == _sock)
        {
            printf("错误，建立socket失败...\n");
        }
        else {
            printf("建立socket=<%d>成功...\n", (int)_sock);
        }
        return _sock;
    }

    //绑定IP和端口号
    int Bind(const char* ip, unsigned short port)
    {
        //if (INVALID_SOCKET == _sock)
        //{
        //	InitSocket();
        //}
        // 2 bind 绑定用于接受客户端连接的网络端口
        sockaddr_in _sin = {};
        _sin.sin_family = AF_INET;
        _sin.sin_port = htons(port);//host to net unsigned short


        if (ip) {
            _sin.sin_addr.s_addr = inet_addr(ip);
        }
        else {
            _sin.sin_addr.s_addr = INADDR_ANY;
        }

        int ret = bind(_sock, (sockaddr*)&_sin, sizeof(_sin));
        if (SOCKET_ERROR == ret)
        {
            printf("错误,绑定网络端口<%d>失败...\n", port);
        }
        else {
            printf("绑定网络端口<%d>成功...\n", port);
        }
        return ret;
    }

    //监听端口号
    int Listen(int n)
    {
        // 3 listen 监听网络端口
        int ret = listen(_sock, n);
        if (SOCKET_ERROR == ret)
        {
            printf("socket=<%d>错误,监听网络端口失败...\n",_sock);
        }
        else {
            printf("socket=<%d>监听网络端口成功...\n", _sock);
        }
        return ret;
    }

    //接受客户端连接
    SOCKET Accept()
    {
        // 4 accept 等待接受客户端连接
        sockaddr_in clientAddr = {};
        int nAddrLen = sizeof(sockaddr_in);
        SOCKET cSock = INVALID_SOCKET;


        cSock = accept(_sock, (sockaddr*)&clientAddr, (socklen_t *)&nAddrLen);

        if (INVALID_SOCKET == cSock)
        {
            printf("socket=<%d>错误,接受到无效客户端SOCKET...\n", (int)_sock);
        }
        else
        {
            //将新客户端分配给客户数量最少的cellServer
            ClientSocketPtr c(new ClientSocket(cSock));
            //addClientToCellServer(std::make_shared<ClientSocket>(cSock));
            addClientToCellServer(c);
            //获取IP地址 inet_ntoa(clientAddr.sin_addr)
        }
        return cSock;
    }

    void addClientToCellServer(ClientSocketPtr& pClient)
    {
        //查找客户数量最少的CellServer消息处理对象
        auto pMinServer = _cellServers[0];
        for(auto pCellServer : _cellServers)
        {
            if (pMinServer->getClientCount() > pCellServer->getClientCount())
            {
                pMinServer = pCellServer;
            }
        }
        pMinServer->addClient(pClient);
        OnNetJoin(pClient);
    }

    void Start(int nCellServer)  
    {
        for (int n = 0; n < nCellServer; n++)
        {
            auto ser = std::make_shared<CellServer>(_sock);
            _cellServers.push_back(ser);
            //注册网络事件接受对象
            ser->setEventObj(this);
            //启动消息处理线程
            ser->Start();
        }
    }
    //关闭Socket
    void Close()
    {
        if (_sock != INVALID_SOCKET)
        {

            close(_sock);

        }
    }
    //处理网络消息
    bool OnRun()
    {
        if (isRun())
        {
            time4msg();
            //伯克利套接字 BSD socket
            fd_set fdRead;//描述符（socket） 集合
            //清理集合
            FD_ZERO(&fdRead);
            //将描述符（socket）加入集合
            FD_SET(_sock, &fdRead);
            ///nfds 是一个整数值 是指fd_set集合中所有描述符(socket)的范围，而不是数量
            ///既是所有文件描述符最大值+1 在Windows中这个参数可以写0
            timeval t = { 0,10};
            int ret = select(_sock + 1, &fdRead, 0, 0, &t); //
            if (ret < 0)
            {
                printf("Accept Select任务结束。\n");
                Close();
                return false;
            }
            //判断描述符（socket）是否在集合中
            if (FD_ISSET(_sock, &fdRead))
            {
                FD_CLR(_sock, &fdRead);
                Accept();
                return true;
            }
            return true;
        }
        return false;
    }
    //是否工作中
    bool isRun()
    {
        return _sock != INVALID_SOCKET;
    }

    //计算并输出每秒收到的网络消息
    void time4msg()
    {
        auto t1 = _tTime.getElapsedSecond();
        if (t1 >= 1.0)
        {
            printf("thread<%d>,time<%lf>,socket<%d>,clients<%d>,recv<%d>,msg<%d>\n", _cellServers.size(), t1, _sock,(int)_clientCount, (int)(_recvCount/ t1), (int)(_msgCount / t1));
            _recvCount = 0;
            _msgCount = 0;
            _tTime.update();
        }
    }
    //只会被一个线程触发 安全
    virtual void OnNetJoin(ClientSocketPtr& pClient)
    {
        _clientCount++;
        //printf("client<%d> join\n", pClient->sockfd());
    }
    //cellServer 4 多个线程触发 不安全
    //如果只开启1个cellServer就是安全的
    virtual void OnNetLeave(ClientSocketPtr& pClient)
    {
        _clientCount--;
        //printf("client<%d> leave\n", pClient->sockfd());
    }
    //cellServer 4 多个线程触发 不安全
    //如果只开启1个cellServer就是安全的
    virtual void OnNetMsg(CellServer* pCellServer, ClientSocketPtr& pClient, DataHeader* header)
    {
        _msgCount++;
    }

    virtual void OnNetRecv(ClientSocketPtr& pClient)
    {
        _recvCount++;
        //printf("client<%d> leave\n", pClient->sockfd());
    }
};


#endif //HELLOSOCKET_EASYTCPSERVER_H
