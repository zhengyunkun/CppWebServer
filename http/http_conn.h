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
#include <sys/uio.h>
#include <map>

#include "../log/log.h"
#include "../lock/locker.h"
#include "../CGImysql/sql_connection.h"
#include "../timer/timer.h"

static const int FILENAME_LEN = 200;
static const int READ_BUFFER_SIZE = 2048;
static const int WRITE_BUFFER_SIZE = 1024;

class httpConnection
{
    public:
        static int      epollFd;
        static int      userCount;
        MYSQL*          mysql;
        int             state;
        enum METHOD
        {
            GET,
            POST,
            HEAD,
            PUT,
            DELETE,
            TRACE,
            OPTIONS,
            CONNECT,
            PATH
        };
        enum CHECK_STATE
        {
            CHECK_STATE_REQUESTLINE,
            CHECK_STATE_HEADER,
            CHECK_STATE_CONTENT
        };
        enum HTTP_CODE
        {
            NO_REQUEST,
            GET_REQUEST,
            BAD_REQUEST,
            NO_RESOURCE,
            FORBIDDEN_REQUEST,
            FILE_REQUEST,
            INTERNAL_ERROR,
            CLOSED_CONNECTION
        };
        enum LINE_STATUS
        {
            LINE_OK,
            LINE_BAD,
            LINE_OPEN
        };

        httpConnection() {}
        ~httpConnection() {}
        void                init(int _sockfd, const sockaddr_in& _addr, char* _root, int _TRIGMode, 
                                 int _closeLog, string _user, string _passwd, string _sqlName);
        void                closeConnection(bool realClose = true);
        void                process();
        bool                readOnce();
        bool                write();
        sockaddr_in*        getAddress() { return &address; }
        void                initMysqlResult(connectionPool* connPool);
        int                 timerFlag;
        int                 improv;

    private:
        int                 sockfd;
        sockaddr_in         address;
        char                readBuf[READ_BUFFER_SIZE];
        long                readIdx;                            // 已经读取的字节数
        long                checkedIdx;                         // 已经检查过的字节数
        int                 startLine;
        char                writeBuf[WRITE_BUFFER_SIZE];
        int                 writeIdx;
        CHECK_STATE         checkState;
        METHOD              method;
        char                realFile[FILENAME_LEN];
        char*               url;
        char*               version;
        char*               host;
        int                 contentLength;                      // HTTP请求体的长度
        bool                linger;                             // 是否持续保持连接
        char*               fileAddress;
        struct stat         fileState;
        struct iovec        iv[2];                              // iv用来管理缓冲区
        int                 ivCount;
        int                 cgi;
        char*               headString;                         // 存储请求头数据
        int                 bytesToSend;
        int                 bytesHaveSend;
        char*               docRoot;
        map<string, string> users;
        int                 TRIGMode;
        int                 closeLog;
        char                sqlUser[100];
        char                sqlPasswd[100];
        char                sqlName[100];

        void                init();
        HTTP_CODE           processRead();
        bool                processWrite(HTTP_CODE ret);
        HTTP_CODE           parseRequestLine(char* text);
        HTTP_CODE           parseHeaders(char* text);
        HTTP_CODE           parseContent(char* text);
        HTTP_CODE           doRequest();
        char*               getLine() { return readBuf + startLine; };
        LINE_STATUS         parseLine();
        void                unmap();
        bool                addResponse(const char* format, ...);
        bool                addContent(const char* content);
        bool                addStatusLine(int status, const char* title);
        bool                addHeaders(int contentLength);
        bool                addContentType();
        bool                addContentLength(int contentLength);
        bool                addLinger();
        bool                addBlankLine();
};