#pragma once


#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include "../log/log.h"

// 定时器类
class utilTimer;

struct clientData
{
    sockaddr_in address;
    int sockfd;
    utilTimer* timer;
};

void callBack(clientData* userData);

class utilTimer
{
    public:
        time_t      expireTime;
        clientData* userData;                // 客户数据
        utilTimer*  prev;                    // 前面的utilTimer
        utilTimer*  next;                    // 后面的utilTimer
        void (*callBack)(clientData*);       // 定时器回调函数

        utilTimer() : prev(NULL), next(NULL) {}
};

// 定时器链表
class timerList
{
    private:
        utilTimer* head;
        utilTimer* tail;
        void addTimer(utilTimer* timer, utilTimer* listHead);

    public:
        timerList();
        ~timerList();

        void addTimer(utilTimer* timer);        // 添加定时器
        void adjustTimer(utilTimer* timer);     // 调整定时器
        void deleteTimer(utilTimer* timer);     // 删除定时器
        void tick();                            // 每次被调用，就会处理链表上到期的定时器
};

class Utils
{
    public:
        static int*   pipeFd;
        timerList     timLst;
        static int    epollFd;
        int           TIMESLOT;

        Utils()  {}
        ~Utils() {}
        void          init(int timeslot);
        int           setNonBlocking(int fd);
        void          addFd(int epollFd, int fd, bool oneShot, int TRIGMode);
        static void   sigHandler(int sig);
        void          addSig(int sig, void(handler)(int), bool restart = true);
        void          timerHandler();
        void          showError(int connfd, const char* info);
};