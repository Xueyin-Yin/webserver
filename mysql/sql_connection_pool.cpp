#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include <sql_connection_pool.h>

using namespace std;

connection_pool *connection_pool::connPool = NULL;
static pthread_mutex_t mutex;

connection_pool::connection_pool() {
    pthread_mutex_init(&mutex, NULL);
}

connection_pool::connection_pool(string url, string user, string password, string dbName, int port, unsigned int maxConn) {
    this->url = url;
    this->user = user;
    this->password = password;
    this->dbName = dbName;

    pthread_mutex_lock(&lock);

    for (int i = 0; i < maxConn; i++) {
        MYSQL *con = mysql_init(con);
        
        if (con == NULL) {
            cout << "Error" << mysql_error(con);
            exit(1);
        }

        con = mysql_real_connect(con, url.c_str(), user.c_str(), password.c_str(), dbName.c_str(), port, NULL, 0);

        if (con == NULL) {
            cout << "Error:" << mysql_error(con);
            exit(1);
        }

        connectList.push_back(con);

        ++freeConn;

    }

    this->maxConn = maxConn;
    this->curConn = 0;
    pthread_mutex_unlock(&lock);
}

connection_pool *connection_pool::GetInstance(string url, string user, string password, string dbName, int port, unsigned int maxConn) {
    if (connPool == NULL) {
        pthread_mutex_lock(&mutex);
        if (connPool == NULL) {
            connPool = new connection_pool(url, user, password, dbName, port, maxConn);
        }

        pthread_mutex_unlock(&mutex);
    }    

    return connPool;
}

MYSQL *connection_pool::GetConnection() {
    MYSQL *con = NULL;
    pthread_mutex_lock(&lock);

    if (connectList.size() > 0) {
        con = connectList.front();
        connectList.pop_front();

        --freeConn;
        ++curConn;

        pthread_mutex_unlock(&lock);

        return con;
    }

    pthread_mutex_unlock(&lock);

    return NULL;
}

bool connection_pool::ReleaseConnection(MYSQL *con) {
    pthread_mutex_lock(&lock);

    if (con != NULL) {
        connectList.push_back(con);
        ++freeConn;
        --curConn;

        pthread_mutex_unlock(&lock);
        return true;
    }

    pthread_mutex_unlock(&lock);
    return false;
}

void connection_pool::DestoryPool() {
    pthread_mutex_lock(&lock);
    if (connectList.size() > 0) {
        list<MYSQL *>::iterator it;

        for (it = connectList.begin(); it != connectList.end(); it++) {
            MYSQL *con = *it;
            mysql_close(con);
        }

        curConn = 0;
        freeConn = 0;
        connectList.clear();

    }

    pthread_mutex_unlock(&lock);
}

int connection_pool::GetFreeConn() {
    return this->freeConn;
}

connection_pool::~connection_pool() {
    DestoryPool();
}