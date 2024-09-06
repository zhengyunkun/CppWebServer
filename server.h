#pragma once


#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <cassert>
#include "./http/http_conn.h"
#include "./threadpool/threadpool.h"

const int MAX_FD = 65536;               // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000;     // 最大事件数
const int TIMESLOT = 5;                 // 最小超时单位

class WebServer
{
    public:
        // 初始化服务器
        int                 port;
        char*               root;
        int                 logWrite;
        int                 closeLog;
        int                 actorModel;
        int                 pipeFd[2];
        int                 epollFd;
        httpConnection*     users;

        // 数据库
        connectionPool*     connPool;
        string              user;
        string              password;
        string              databaseName;
        int                 sqlNum;

        // 线程池
        threadPool<httpConnection>* pool;
        int threadNum;

        // epoll相关
        epoll_event events[MAX_EVENT_NUMBER];

        int listenFd;
        int optLinger;
        int TRIGMode;
        int LISTENTRIGMode;
        int CONNTRIGMode;

        // 定时器
        clientData* usersTimer;
        Utils utils;

    public:
        WebServer();
        ~WebServer();

        void init(int _port, string _user, string _password, string _databaseName, int _logWrite,
                  int _optLinger, int _TRIGMode, int _sqlNum, int _threadNum, int _closeLog, int _actorModel);

        void threadPool();
        void sqlPool();
        void logWrite();
        void trigMode();
        void eventListen();
        void eventLoop();
        void timer(int connfd, struct sockaddr_in client_address);
        void adjustTimer(utilTimer* timer);
        void dealTimer(utilTimer* timer, int sockfd);
        bool dealClientData();
        bool dealSignal(bool& timeout, bool& stopServer);
        void dealRead(int sockfd);
        void dealWrite(int sockfd);
};