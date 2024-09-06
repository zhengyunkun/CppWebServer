#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

// 定义HTTP响应的一些状态信息
const char* ok200Title = "OK";
const char* error400Title = "Bad Request";
const char* error400Form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error403Title = "Forbidden";
const char* error403Form = "You do not have permission to get file from this server.\n";
const char* error404Title = "Not Found";
const char* error404Form = "The requested file was not found on this server.\n";
const char* error500Title = "Internal Error";
const char* error500Form = "There was an unusual problem serving the requested file.\n";

locker lock;
map<string, string> users;

void httpConnection::initMysqlResult(connectionPool* connPool)
{
    // 从连接池中取一个连接
    MYSQL* mysql = NULL;
    connectionRAII mysqlCon(&mysql, connPool);

    // 在user表中检索username，passwd数据，查询成功返回0
    if (mysql_query(mysql, "SELECT username, passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }
    // 从表中检索完整的结果集
    MYSQL_RES* result = mysql_store_result(mysql);
    // 返回结果集中的列数
    int numFields = mysql_num_fields(result);
    // 返回所有字段结构的数组
    MYSQL_FIELD* fields = mysql_fetch_fields(result);
    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string s1(row[0]);
        string s2(row[1]);
        users[s1] = s2;
    }
}

// 对文件描述符设置非阻塞
void setNonBlocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
// 也就是epoll只会通知一次该事件，之后该文件描述符会被自动从epoll中删除
// TRIGMode为1说明是ET模式，0说明是LT模式
void addFd(int epollFd, int fd, bool oneShot, int TRIGMode)
{
    epoll_event events;
    events.data.fd = fd;
    if (TRIGMode == 1) events.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else events.events = EPOLLIN | EPOLLRDHUP;

    if (oneShot) events.events |= EPOLLONESHOT;
    epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &events);
    setNonBlocking(fd);
}

// 从内核事件表删除描述符
void removeFd(int epollFd, int fd)
{
    epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modFd(int epollFd, int fd, int ev, int TRIGMode)
{
    epoll_event events;
    events.data.fd = fd;
    if (TRIGMode == 1) events.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else events.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &events);
}

int httpConnection::userCount = 0;
int httpConnection::epollFd = -1;

// 关闭连接，关闭一个连接，客户总量减一
void httpConnection::closeConnection(bool realClose)
{
    if (realClose && (sockfd != -1))
    {
        printf("Fd %d closed...\n", sockfd);
        removeFd(epollFd, sockfd);
        sockfd = -1;
        userCount--;
    }
}

// 初始化连接，外部调用初始化套接字地址
void httpConnection::init(int _sockfd, const sockaddr_in& _addr, char* _root, int _TRIGMode, 
                          int _closeLog, string _user, string _passwd, string _sqlName)
{
    sockfd = _sockfd;
    address = _addr;

    addFd(epollFd, _sockfd, true, TRIGMode); // 默认注册EPOLLONESHOT事件
    userCount++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或者http格式出错或者访问的文件中内容完全为空
    docRoot = _root;
    TRIGMode = _TRIGMode;
    closeLog = _closeLog;

    strcpy(sqlUser, _user.c_str());
    strcpy(sqlPasswd, _passwd.c_str());
    strcpy(sqlName, _sqlName.c_str());

    init();
}

// 初始化新接受的连接
// checkState默认是CHECK_STATE_REQUESTLINE，分析请求行状态
void httpConnection::init()
{
    mysql = NULL;
    bytesHaveSend = 0;
    bytesToSend = 0;
    checkState = CHECK_STATE_REQUESTLINE;
    linger = false;
    method = GET;
    url = 0;
    version = 0;
    contentLength = 0;
    host = 0;
    startLine = 0;
    checkedIdx = 0;
    readIdx = 0;
    writeIdx = 0;
    cgi = 0;
    state = 0;
    timerFlag = 0;
    improv = 0;

    memset(readBuf, '\0', READ_BUFFER_SIZE);
    memset(writeBuf, '\0', WRITE_BUFFER_SIZE);
    memset(realFile, '\0', FILENAME_LEN);
}

// 从状态机，用于解析出一行内容
// 返回值为行的读取状态，有LINE_OK, LINE_BAD, LINE_OPEN
httpConnection::LINE_STATUS httpConnection::parseLine()
{
    char temp;
    for (; checkedIdx < readIdx; checkedIdx ++ )
    {
        temp = readBuf[checkedIdx];
        if (temp == '\r')
        {
            if ((checkedIdx + 1) == readIdx) return LINE_OPEN;  // 还未读到一个完整的行
            else if (readBuf[checkedIdx + 1] == '\n')   // 如果下一个字符是'\n'，则说明读到了一个完整的行
            {
                readBuf[checkedIdx ++ ] == '\0';
                readBuf[checkedIdx ++ ] == '\0';
                // 替换'\r\n'为'\0\0'，返回LINE_OK
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')  // 如果当前字符是'\n'，则说明读到了一个完整的行
        {
            if (checkedIdx > 1 && readBuf[checkedIdx - 1] == '\r')
            {
                readBuf[checkedIdx - 1] = '\0'; // 替换'\r\n'为'\0'
                readBuf[checkedIdx ++ ] = '\0'; // 替换'\n'为'\0'
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 循环读取客户端数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
bool httpConnection::readOnce()
{
    if (readIdx >= READ_BUFFER_SIZE) return false;
    int readBytes = 0;

    // ET模式下，需要一次性将数据读完
    if (TRIGMode == 1)
    {
        readBytes = read(sockfd, readBuf + readIdx, READ_BUFFER_SIZE - readIdx);
        // 读取数据到readBuf + readIdx位置，最多读取READ_BUFFER_SIZE - readIdx个字节
        readIdx += readBytes;
        if (readBytes <= 0) return false;
        return true;
    }

    // LT模式下，循环读取数据
    else
    {
        while (true)
        {
            readBytes = read(sockfd, readBuf + readIdx, READ_BUFFER_SIZE - readIdx);
            if (readBytes == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                return false;
            }
            else if (readBytes == 0) return false;
            readIdx += readBytes;
        }
        return true;
    }
}

// 解析HTTP请求行，获得请求方法，目标URL，以及HTTP版本号
// 以"GET /index.html HTTP/1.1"为例
httpConnection::HTTP_CODE httpConnection::parseRequestLine(char* text)
{
    url = strpbrk(text, " \t"); 
    // ' '和'\t'
    // 在text中找到第一个匹配" \t"中任一字符的位置，返回该位置的指针

    // 如果请求行中没有空白字符或'\t'字符，则HTTP请求必有问题
    if (!url) return BAD_REQUEST;
    *url++ = '\0';  // 将请求方法与url分隔开，text指的是请求方法，而url指的是url

    char* meth = text;
    // meth = "GET"
    if (strcasecmp(meth, "GET") == 0) method = GET;
    else if (strcasecmp(meth, "POST") == 0)
    {
        method = POST;
        cgi = 1;
    }
    else return BAD_REQUEST;

    url += strspn(url, " \t");
    // 跳过url前的空白字符，url = "/index.html HTTP/1.1"
    version = strpbrk(url, " \t");
    // version 指向 "index.html HTTP/1.1"中的第一个空格
    if (!version) return BAD_REQUEST;
    *version++ = '\0';
    // url = "/index.html", version = "HTTP/1.1"
    version += strspn(version, " \t");
    if (strcasecmp(version, "HTTP/1.1") != 0) return BAD_REQUEST;
    if (strncasecmp(url, "http://", 7) == 0)    // 比较前7个字符
    {
        url += 7;
        // 跳过"http://"
        url = strchr(url, '/');
        // 在url中找到第一个'/'字符的位置，返回该位置的指针
    }
    else if (strncasecmp(url, "https://", 8) == 0)
    {
        url += 8;
        url = strchr(url, '/');
    }

    // url = "/index.html"
    if (!url || url[0] != '/') return BAD_REQUEST;

    // 当url为'/'时，显示判断界面
    if (strlen(url) == 1) strcat(url, "judge.html");

    checkState = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
httpConnection::HTTP_CODE httpConnection::parseHeaders(char* text)
{
    if (text[0] == '\0')    // 请求头部字段为空，说明是空行，表示头部字段解析完毕，接下来去解析请求体
    {
        if (contentLength != 0)
        {
            checkState = CHECK_STATE_CONTENT;
            return NO_REQUEST;
            // 请求头部解析完毕，但是还有请求体需要解析
        }
        // 如果没有请求体，则说明解析完毕
        return GET_REQUEST; 
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) linger = true;
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        contentLength = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        host = text;
    }
    else
    {
        LOG_INFO("oop! unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 判断HTTP请求是否被完整读入
httpConnection::HTTP_CODE httpConnection::parseContent(char* text)
{
    if (readIdx >= (contentLength + checkedIdx))
    {
        text[contentLength] = '\0';
        // POST请求中最后为输入的用户名和密码
        headString = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，用于从buffer中取出所有完整的行
// 
httpConnection::HTTP_CODE httpConnection::processRead()
{
    LINE_STATUS lineStatus = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    while ((checkState == CHECK_STATE_CONTENT && lineStatus == LINE_OK)
            || ((lineStatus = parseLine()) == LINE_OK))
    // 循环读取数据，直到无数据可读或对方关闭连接
    {
        text = getLine();
        startLine = checkedIdx;
        LOG_INFO("%s", text);
        switch (checkState)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parseRequestLine(text);
            if (ret == BAD_REQUEST) return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parseHeaders(text);
            if (ret == GET_REQUEST) return doRequest();
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parseContent(text);
            if (ret == GET_REQUEST) return doRequest();
            lineStatus = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;
}

// 比如url = /2user=alice&password=12345
httpConnection::HTTP_CODE httpConnection::doRequest()
{
    strcpy(realFile, docRoot);
    int len = strlen(docRoot);
    const char* p = strrchr(url, '/');
    // 在url中找到最后一个'/'字符的位置，返回该位置的指针

    // 处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))   // '/'后面的第一个字符是2或3
    {
        // 根据标志位判断是登录检测还是注册检测，falg == 2表示登录检测，flag == 3表示注册检测
        char flag = url[1];

        char* urlReal = (char*)malloc(sizeof(char) * 200);
        strcpy(urlReal, "/");
        strcat(urlReal, url + 2);
        strncpy(realFile + len, urlReal, FILENAME_LEN - len - 1);
        // 把urlReal中的内容复制到realFile中
        free(urlReal);

        // 把用户名和密码提取出来
        // user=alice&password=12345
        char name[100], passwd[100];
        int i;
        for (i = 5; headString[i] != '&'; i ++ ) name[i - 5] = headString[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; headString[i] != '\0'; i ++ , j ++ ) passwd[j] = headString[i];
        passwd[j] = '\0';

        if (*(p + 1) == '3')
        {
            // 如果是注册，首先检测数据库中是否有重名的
            // 如果没有，则插入数据库中
            char* sqlInsert = (char*)malloc(sizeof(char) * 200);
            strcpy(sqlInsert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sqlInsert, "'");
            strcat(sqlInsert, name);
            strcat(sqlInsert, "', '");
            strcat(sqlInsert, passwd);
            strcat(sqlInsert, "')");

            if (users.find(name) == users.end())
            {
                lock.lock();
                int res = mysql_query(mysql, sqlInsert);
                users.insert(pair<string, string>(name, passwd));
                lock.unlock();

                if (!res) strcpy(url, "/log.html");
                // 注册成功，返回登录界面
                else strcpy(url, "/registerError.html");
            }
            else strcpy(url, "/registerError.html");
        }

        // 如果是登录，直接判断
        // 如果输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == passwd) strcpy(url, "/welcome.html");
            else strcpy(url, "/logError.html");
        }
    }

    // 不处理cgi请求
    if (*(p + 1) == '0')
    {
        char* urlReal = (char*)malloc(sizeof(char) * 200);
        strcpy(urlReal, "/register.html");
        strncpy(realFile + len, urlReal, strlen(urlReal));
        free(urlReal);
    }
    else if (*(p + 1) == '1')
    {
        char* urlReal = (char*)malloc(sizeof(char) * 200);
        strcpy(urlReal, "/log.html");
        strncpy(realFile + len, urlReal, strlen(urlReal));
        free(urlReal);
    }
    else if (*(p + 1) == '5')
    {
        char* urlReal = (char*)malloc(sizeof(char) * 200);
        strcpy(urlReal, "/picture.html");
        strncpy(realFile + len, urlReal, strlen(urlReal));
        free(urlReal);
    }
    else if (*(p + 1) == '6')
    {
        char* urlReal = (char*)malloc(sizeof(char) * 200);
        strcpy(urlReal, "/video.html");
        strncpy(realFile + len, urlReal, strlen(urlReal));
        free(urlReal);
    }
    else if (*(p + 1) == '7')
    {
        char* urlReal = (char*)malloc(sizeof(char) * 200);
        strcpy(urlReal, "/fans.html");
        strncpy(realFile + len, urlReal, strlen(urlReal));
        free(urlReal);
    }
    else strncpy(realFile + len, url, FILENAME_LEN - len - 1);

    // 通过stat获取请求资源文件信息，成功则将信息更新到fileState结构体
    // (1) 失败返回NO_RESOURCE状态，表示请求的资源文件不存在
    // (2) 如果没有访问权限，则返回FORBIDDEN_REQUEST状态
    // (3) 如果是目录，则返回BAD_REQUEST状态，表示请求报文有误
    // (4) 成功则返回FILE_REQUEST状态，表示获取文件成功
    if (stat(realFile, &fileState) < 0) return NO_RESOURCE;
    if (!fileState.st_mode & S_IROTH) return FORBIDDEN_REQUEST;
    if (S_ISDIR(fileState.st_mode)) return BAD_REQUEST;

    int fd = open(realFile, O_RDONLY);
    fileAddress = (char*)mmap(0, fileState.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}


void httpConnection::unmap()
{
    if (fileAddress)
    {
        munmap(fileAddress, fileState.st_size);
        fileAddress = 0;
    }
}

bool httpConnection::write()
{
    int temp = 0;
    if (bytesToSend == 0)
    // 初始时没有数据需要发送
    {
        modFd(epollFd, sockfd, EPOLLIN, TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(sockfd, iv, ivCount);
        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modFd(epollFd, sockfd, EPOLLOUT, TRIGMode);
                return true;
            }
        }

        // 更新已发送字节数和剩余字节数
        bytesHaveSend += temp;
        bytesToSend -= temp;

        // 如果发送的字节大于等于iov[0].iov_len，则说明iov[0]中的数据已发送完
        if (bytesHaveSend >= iv[0].iov_len)
        {
            iv[0].iov_len = 0;
            // 发送完之后，将iov[1]中的数据发送出去
            iv[1].iov_base = fileAddress + (bytesHaveSend - writeIdx);
            iv[1].iov_len = bytesToSend;
        }
        else
        {
            iv[0].iov_base = writeBuf + bytesHaveSend;
            iv[0].iov_len = iv[0].iov_len - bytesHaveSend;
        }

        // 数据已全部发送完，根据linger决定是否关闭连接
        if (bytesToSend <= 0)
        {
            unmap();
            modFd(epollFd, sockfd, EPOLLIN, TRIGMode);

            if (linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

// 
bool httpConnection::addResponse(const char* format, ...)
{
    if (writeIdx >= WRITE_BUFFER_SIZE) return false;

    // 初始化可变参数列表
    va_list argList;
    va_start(argList, format);
    int len = vsnprintf(writeBuf + writeIdx, WRITE_BUFFER_SIZE - 1 - writeIdx, format, argList);
    if (len >= (WRITE_BUFFER_SIZE - 1 - writeIdx))
    {
        va_end(argList);
        return false;
    }
    writeIdx += len;
    va_end(argList);

    LOG_INFO("request:%s", writeBuf);

    return true;
}

bool httpConnection::addStatusLine(int status, const char* title)
{
    return addResponse("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool httpConnection::addHeaders(int contentLen)
{
    return addContentLength(contentLen) && addLinger() && addBlankLine();
}

bool httpConnection::addContentType()
{
    return addResponse("Content-Type:%s\r\n", "text/html");
}

bool httpConnection::addContentLength(int contentLen)
{
    return addResponse("Content-Length:%d\r\n", contentLen);
}

bool httpConnection::addLinger()
{
    return addResponse("Connection:%s\r\n", (linger == true) ? "keep-alive" : "close");
}

bool httpConnection::addBlankLine()
{
    return addResponse("%s", "\r\n");
}

bool httpConnection::addContent(const char* content)
{
    return addResponse("%s", content);
}

// 根据不同的HTTP请求，服务器子线程调用不同的处理函数，返回不同的response
bool httpConnection::processWrite(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        addStatusLine(500, error500Title);
        addHeaders(strlen(error500Form));
        if (!addContent(error500Form)) return false;
        break;
    }
    case BAD_REQUEST:
    {
        addStatusLine(400, error400Title);
        addHeaders(strlen(error400Form));
        if (!addContent(error400Form)) return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        addStatusLine(403, error403Title);
        addHeaders(strlen(error403Form));
        if (!addContent(error403Form)) return false;
        break;
    }
    case FILE_REQUEST:
    {
        addStatusLine(200, ok200Title);
        if (fileState.st_size != 0)
        {
            addHeaders(fileState.st_size);
            iv[0].iov_base = writeBuf;
            iv[0].iov_len = writeIdx;
            iv[1].iov_base = fileAddress;
            iv[1].iov_len = fileState.st_size;
            ivCount = 2;
            bytesToSend = writeIdx + fileState.st_size;
            return true;
        }
        else
        {
            const char* okString = "<html><body></body></html>";
            addHeaders(strlen(okString));
            if (!addContent(okString)) return false;
        }
    }
    default:
        return false;
    }

    iv[0].iov_base = writeBuf;
    iv[0].iov_len = writeIdx;
    ivCount = 1;
    bytesToSend = writeIdx;
    return true;
}

// 服务器子线程调用process函数处理HTTP请求
void httpConnection::process()
{
    HTTP_CODE readRet = processRead();
    if (readRet == NO_REQUEST)
    {
        modFd(epollFd, sockfd, EPOLLIN, TRIGMode);
        return;
    }

    bool writeRet = processWrite(readRet);
    if (!writeRet) closeConnection();
    modFd(epollFd, sockfd, EPOLLOUT, TRIGMode);
}