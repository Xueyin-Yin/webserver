#include <mysql/mysql.h>
#include <iostream>
#include <string>
#include <string.h>
#include <cstdio>
#include <sql_connection_pool.h>
#include <map>
#include <fstream>
#include <sstream>

using namespace std;

//#define CGISQL
#define CGISQLPOOL

int main(int argc, char *argv[]) {
    map<string, string> users;

    pthread_mutex_t lock;
    pthread_mutex_init(&lock, NULL);

#ifdef CGISQL
    MYSQL *con = NULL;
    con = mysql_init(con);

    if (con == NULL) {
        cout << "Error: " << mysql_error(con);
        exit(1);
    }

    con = mysql_real_connect(con, "localhost", "root", "root", "qgydb", 3306, NULL, 0);

    if (con == NULL) {
        cout << "Error: " << mysql_error(con);
        exit(1);
    } 

    if (mysql_query(con, "SELECT username, passwd FROM user")) {
        printf("INSERT error: %s\n", mysql_error(con));
        return -1;
    }

    MYSQL_RES *result = mysql_store_result(con);

    int num_fields = mysql_num_fields(result);

    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);

        users[temp1] = temp2;
    }

    string name(argv[1]);
    string passwd(argv[2]);

    char flag = *argv[0];

    string sql_insert = "INSERT INTO user(username, passwd) VALUES('" + name + "','" + passwd + "')";

    switch (flag)
    {
    case '3':
        if (users.find(name) == users.end()) {
            pthread_mutex_lock(&lock);
            int res = mysql_query(con, sql_insert);
            pthread_mutex_unlock(&lock);

            if (!res) {
                printf("1\n");
            } else {
                printf("0\n");
            }
        } else {
            printf("0\n");
        }
        break;

    case '2':
        if (users.find(name) != users.end() && users[name] == passwd) {
            printf("1\n");
        } else {
            printf("0\n");
        }
        break;
    default:
        printf("0\n");
        break;
    }

    mysql_free_result(result);

#endif

#ifdef CGISQLPOOL
    ifstream out(argv[2]);
    string line;

    while (getline(out, line)) {
        string str;
        stringstream id_passwd(line);

        getline(id_passwd, str, ' ');
        string temp1(str);

        getline(id_passwd, str, ' ');
        string temp2(str);
        users[temp1] = temp2;
    }

    out.close();

    string name(argv[0]);
    string passwd(argv[1]);

    if (users.find(name) != users.end() && users[name] == passwd) {
        printf("1\n");
    } else {
        printf("0\n");
    }
#endif

}

