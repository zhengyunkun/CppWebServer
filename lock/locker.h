#pragma once


#include <exception>
#include <pthread.h>
#include <semaphore.h>

// 封装信号量的类
class sem
{
    private:
        sem_t semaphore;

    public:
        sem()
        {
            if (sem_init(&semaphore, 0, 0) != 0)
                throw std::exception();
        }

        sem(int num)
        {
            if (sem_init(&semaphore, 0, num) != 0)
                throw std::exception();
        }

        ~sem()
        {
            sem_destroy(&semaphore);
        }

        // 信号量减1
        bool wait()
        {
            return sem_wait(&semaphore) == 0;
        }

        // 信号量加1
        bool post()
        {
            return sem_post(&semaphore) == 0;
        }
};

// 封装互斥锁的类
class locker
{
    private:
        pthread_mutex_t mutex;

    public:
        locker()
        {
            if (pthread_mutex_init(&mutex, NULL) != 0)
            {
                throw std::exception();
            }
        }

        ~locker()
        {
            pthread_mutex_destroy(&mutex);
        }
        
        // 加锁，返回值为0表示成功
        bool lock()
        {
            return pthread_mutex_lock(&mutex) == 0;
        }

        // 解锁，返回值为0表示成功
        bool unlock()
        {
            return pthread_mutex_unlock(&mutex) == 0;
        }

        pthread_mutex_t *get()
        {
            return &mutex;
        }
};

// 封装条件变量的类（不可以直接用<condition_variable>吗？非要这么麻烦封装个类？）
class cond
{
    private:
        //static pthread_mutex_t m_mutex;
        pthread_cond_t cv;

    public:
        cond()
        {
            if (pthread_cond_init(&cv, NULL) != 0)
            {
                //pthread_mutex_destroy(&m_mutex);
                throw std::exception();
            }
        }

        ~cond()
        {
            pthread_cond_destroy(&cv);
        }

        bool wait(pthread_mutex_t *m_mutex)
        {
            int ret = 0;
            //pthread_mutex_lock(&m_mutex);
            ret = pthread_cond_wait(&cv, m_mutex);
            //pthread_mutex_unlock(&m_mutex);
            return ret == 0;
        }

        bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
        {
            int ret = 0;
            //pthread_mutex_lock(&m_mutex);
            ret = pthread_cond_timedwait(&cv, m_mutex, &t);
            //pthread_mutex_unlock(&m_mutex);
            return ret == 0;
        }

        bool signal()
        {
            return pthread_cond_signal(&cv) == 0;
        }
        
        bool broadcast()
        {
            return pthread_cond_broadcast(&cv) == 0;
        }
};
