#include <stdio.h>
#include <string>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <mysql/mysql.h>
#include "sql_connection.h"

connectionPool::connectionPool()
{
    curConn = 0;
    freeConn = 0;
}

connectionPool* connectionPool::GetInstance()
{
    static connectionPool connPool;
    return &connPool;
}

// 初始化数据库连接池
void connectionPool::init(string _url, string _user, string _passwd, string _dbName, 
                          int _port, int _maxConn, int _closeLog)
{
    // 初始化数据库信息
    url = _url;
    user = _user;
    passwd = _passwd;
    dbName = _dbName;
    port = _port;
    closeLog = _closeLog;

    // 创建maxConn条数据库连接
    for (int i = 0; i < maxConn; i ++ )
    {
        MYSQL* con = NULL;
        con = mysql_init(con);  
        // 初始化一个MYSQL对象
        if (con == NULL)
        {
            LOG_ERROR("MySQL failed...");
            exit(1);
        }
        con = mysql_real_connect(con, url.c_str(), user.c_str(), passwd.c_str(), dbName.c_str(), port, NULL, 0);
        // mysql_real_connect用于建立一个到MySQL数据库的连接，con变为连接句柄
        connList.push_back(con);
        freeConn++;
    }

    // 创建信号量
    reserve = sem(freeConn);
}

// 从数据库连接池中获取一个可用连接，当有请求时，同时更新使用和空闲连接数
MYSQL* connectionPool::GetConnection()
{
    MYSQL* con = NULL;
    if (connList.empty()) return NULL;
    // 如果连接池为空，返回空

    reserve.wait(); 
    // wait()函数会将信号量的值减1，如果信号量的值为0，则阻塞等待
    // 也就是说，如果当前没有空闲连接，那么就会阻塞等待
    lock.lock(); 
    // 这里加锁的目的是为了保证curConn和freeConn的一致性
    con = connList.front();
    connList.pop_front();

    freeConn -- ;
    curConn ++ ;

    lock.unlock();
    return con;
}

// 释放当前使用的连接
bool connectionPool::ReleaseConnection(MYSQL* con)
{
    if (con == NULL) return false;

    // 加锁
    lock.lock();
    connList.push_back(con);
    // 释放连接，空闲连接数加1，把当前的连接放回连接池
    freeConn ++ ;
    curConn -- ;
    lock.unlock();

    // 信号量加1
    reserve.post();
    return true;
}

// 销毁数据库连接池
void connectionPool::DestroyPool()
{
    lock.lock();
    if (!connList.empty())
    {
        // 因为connList是一个list，在内存中是不连续存储的，不能通过数组的方式直接释放
        // 迭代器使用了一种统一的方式来遍历容器，不管容器是什么类型
        list<MYSQL*>::iterator it;
        for (it = connList.begin(); it != connList.end(); it ++ )
        {
            MYSQL* con = *it;
            mysql_close(con);
            // 关闭连接
        }
        curConn = 0;
        freeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

// 返回空闲连接数
int connectionPool::GetFreeConn()
{
    return this->freeConn;
}

connectionPool::~connectionPool()
{
    DestroyPool();
}

connectionRAII::connectionRAII(MYSQL** SQL, connectionPool* connPool)
{
    *SQL = connPool->GetConnection();
    // SQL是一个MYSQL*类型的指针，*SQL是一个MYSQL*类型的对象，用于存储从连接池中取出的连接
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(conRAII);
}