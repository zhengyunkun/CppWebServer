#include "timer.h"
#include "../http/http_conn.h"

timerList::timerList()
{
    head = NULL;
    tail = NULL;
}
timerList::~timerList()
{
    utilTimer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void timerList::addTimer(utilTimer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    if (timer->expireTime < head->expireTime)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    addTimer(timer, head);
}
void timerList::adjustTimer(utilTimer *timer)
{
    if (!timer)
    {
        return;
    }
    utilTimer *tmp = timer->next;
    if (!tmp || (timer->expireTime < tmp->expireTime))
    {
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        addTimer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        addTimer(timer, timer->next);
    }
}
void timerList::deleteTimer(utilTimer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
void timerList::tick()
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL);
    utilTimer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expireTime)
        {
            break;
        }
        tmp->callBack(tmp->userData);
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

void timerList::addTimer(utilTimer *timer, utilTimer *lst_head)
{
    utilTimer *prev = lst_head;
    utilTimer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expireTime < tmp->expireTime)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setNonBlocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addFd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setNonBlocking(fd);
}

//信号处理函数
void Utils::sigHandler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipeFd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void Utils::addSig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timerHandler()
{
    timLst.tick();
    alarm(TIMESLOT);
}

void Utils::showError(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::pipeFd = 0;
int Utils::epollFd = 0;

class Utils;
void callBack(clientData *user_data)
{
    epoll_ctl(Utils::epollFd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    httpConnection::userCount--;
}
