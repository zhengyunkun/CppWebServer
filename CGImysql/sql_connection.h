#pragma once


#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include "../lock/locker.h"
#include "../log/log.h"

class connectionPool
{
    private:
        int            maxConn;      // 最大连接数
        int            curConn;      // 当前已经使用的连接数
        int            freeConn;     // 当前空闲连接数
        locker         lock;         // 互斥锁
        list<MYSQL*>   connList;     // 连接池
        sem reserve;                 // 信号量，确保同一时间只有一个线程访问共享资源

        connectionPool();
        ~connectionPool();

    public:
        string url;                  // 主机地址
        string user;                 // 用户
        string passwd;               // 数据库密码
        string dbName;               // 数据库名
        int    port;                 // 端口
        int    closeLog;             // 日志开关

        void     init(string _url, string _user, string _passwd, string _dbName, int _port, int _maxConn, int _closeLog);
        MYSQL*   GetConnection();
        bool     ReleaseConnection(MYSQL* conn);
        int      GetFreeConn();
        void     DestroyPool();
        static   connectionPool* GetInstance();
};

// 通过RAII机制，不需要显式释放连接 
class connectionRAII
{
    private:
        MYSQL*            conRAII;      // 一个MYSQL连接
        connectionPool*   poolRAII;     // 连接池
    
    public:
        connectionRAII(MYSQL** con, connectionPool* connPool);  
        // 从连接池中取一个连接
        ~connectionRAII();
};