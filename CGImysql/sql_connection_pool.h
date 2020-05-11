// Copyright 2020 Xueyin Yin
// Author: Xueyin Yin
// This is the header file of http_conn

#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <errors.h>
#include <string.h>
#include <iostream>
#include <string>
#include <../lock/lock.h>

using namespace std;

class connection_pool {
    public:
        MYSQL *GetConnection();
        bool ReleaseConnection(MYSQL *conn);
        void DestoryPool();

        static connection_pool *GetInstance(string url, string user, string password, string dbName, int port, unsigned int maxConn);
        int GetFreeConn();

        connection_pool();
        ~connection_pool();

    private:
        unsigned int maxConn;
        unsigned int curConn;
        unsigned int freeConn;

    private:
        pthread_mutex_t lock;
        list<MYSQL *> connectList;
        connection_pool *connect;
        MYSQL *con;
        connection_pool(string url, string user, string password, string dataName, int port, unsigned int maxConn);
        static connection_pool *connPool;

    private:
        string url;
        string port;
        string user;
        string password;
        string dbName;

};

#endif