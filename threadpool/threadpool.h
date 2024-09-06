#pragma once


#include <list>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection.h"

template <typename T>
class threadPool
{
    private:
        int                 threadNumer;    // 线程池中的线程数
        int                 maxRequest;     // 请求队列中允许的最大请求数
        pthread_t*          threads;        // 描述线程池的数组，其大小为threadNumber
        std::list<T*>       workQueue;      // 请求队列
        locker              queueLocker;    // 保护请求队列的互斥锁
        sem                 queueState;     // 是否有任务需要处理
        connectionPool*     connPool;       // 数据库连接池
        int                 actorModel;     // 模型切换

        static void* worker(void* arg);
        void run();

    public:
        threadPool(int _actorModel, connectionPool* _connPool, int _threadNumber = 8, int _maxRequest = 10000);
        ~threadPool();
        bool append(T* request, int state); // 添加任务
        bool appendP(T* request);
};

template <typename T>
threadPool<T>::threadPool(int _actorModel, connectionPool* _connPool, int _threadNumber, int _maxRequest) :
            actorModel(_actorModel), threadNumber(_threadNumber), maxRequest(_maxRequest), threads(NULL), connPool(_connPool)
{
    if (_threadNumber <= 0 || _maxRequest <= 0) {
        throw std::invalid_argument("Thread number and max request must be greater than 0");
    }
    threads = new pthread_t[_threadNumber];
    if (!threads) {
        throw std::bad_alloc();
    }

    for (int i = 0; i < _threadNumber; i++) {
        // 创建线程，如果失败则抛出异常
        if (pthread_create(threads + i, NULL, worker, this) != 0) {
            delete[] threads;
            throw std::runtime_error("Failed to create thread");
        }
        // 分离线程，成功返回0，如果失败则抛出异常
        if (pthread_detach(threads[i])) {
            delete[] threads;
            throw std::runtime_error("Failed to detach thread");
        }
    }
}

template <typename T>
threadPool<T>::~threadPool()
{
    delete[] threads;
}

template <typename T>
bool threadPool<T>::append(T* request, int state)
{
    queueLocker.lock();
    if (workQueue.size() >= maxRequest)
    {
        queueLocker.unlock();
        return false;
    }

    request->state = state;
    workQueue.push_back(request);
    queueLocker.unlock();
    queueState.post();  // 信号量+1
    return true;
}

template <typename T>
bool threadPool<T>::appendP(T* request)
{
    queueLocker.lock();
    if (workQueue.size() >= maxRequest)
    {
        queueLocker.unlock();
        return false;
    }
    workQueue.push_back(request);
    queueLocker.unlock();
    queueState.post();  // 信号量+1
    return true;
}

template <typename T>
void* threadPool<T>::worker(void* arg)
{
    threadPool* pool = (threadPool*)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadPool<T>::run()
{
    while (true)
    {
        queueState.wait();  // 信号量-1
        queueLocker.lock();
        if (workQueue.empty())
        {
            queueLocker.unlock;
            continue;
        }

        T* request = workQueue.front();
        workQueue.pop_front();
        queueLocker.unlock();

        if (!request)   continue;
        if (actorModel == 1)
        {
            // 读写模式
            if (request->state == 0)
            {
                if (request->readOnce())
                {
                    request->improv = 1;
                    connectionRAII mysqlConn(&request->mysql, connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timerFlag = 1;     // 读取失败，关闭定时器
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timerFlag = 1;     // 写入失败，关闭定时器
                }
            }
        }
        else
        // 直接获取数据库连接
        {
            connectionRAII mysqlConn(&request->mysql, connPool);
            request->process();
        }
    }
}