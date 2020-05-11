#include <http_conn.h>
#include <../log/log.h>
#include <map>
#include <mysql/mysql.h>
#include <fstream>

#define SYNSQL
//#define CGISQLPOOL
//#define CGISQL

//#define ET
#define LT

const string ok_200_title = "OK";
const string error_400_title = "Bad Request";
const string error_400_form = "Your request has bad syntax or is inherently impossible to statisfy.\n";
const string error_403_title = "Forbidden";
const string error_404_title = "Not Found";
const string error_404_form = "The requested file was not found on this server.\n";
const string error_500_title = "Internal Error";
const string error_500_form = "There was an unusual problem serving the request file.\n";

const string doc_root = "./root";

const string account_dir = "./CGImysql/id_passwd.txt";

map<string, string> users;

#ifdef SYNSQL

void http_conn::initmysql_result(connection_pool *connPool) {
    MYSQL *mysql = connPool->GetConnection();

    if (mysql_query(mysql, "SELECT username, passwd FROM user")) {
        LOG_ERROR("SELECT error: %s\n", mysql_error(mysql));
    }

    MYSQL_RES *result = mysql_store_result(mysql);

    int num_fields = mysql_num_fields(result);

    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }

    connPool->ReleaseConnection(mysql);

}

#endif

#ifdef CGISQLPOOL

void http_conn::initresultFile(connection_pool *connPool) {
    ofstream out(account_dir);

    MYSQL *mysql = connPool->GetConnection();

    if (mysql_query(mysql, "SELECT username, passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    MYSQL_RES *result = mysql_store_result(mysql);

    int num_fields = mysql_num_fields(result);

    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        out << temp1 << " " << temp2 << endl;
        users[temp1] = temp2;
    }

    connPool->ReleaseConnection(mysql);
    out.close();
}

#endif

int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;

#ifdef ET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef LT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);

}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd) {
    epoll_event event;
    event.data.fd = fd;

#ifdef ET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef LT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

void http_conn::init() {
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_idx, '\0', WRITE_BUFFER_SIZE);
    memset(m_read_file, '\0', FILENAME_LEN);
}

http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;

    for (; m_checked_idx < n_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];

        if (temp == 'r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if (temp == '\n') {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    return LINE_OPEN;
}

bool http_conn::read_once() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    int bytes_read = 0;

    while (true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return false;
        } else if (bytes_read == 0) {
            return false;
        }

        m_read_idx += bytes_read;
    }

    return true;
}

http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    m_url = strpbrk(text, "\t");

    if (!m_url) {
        return BAD_REQUEST;
    }

    *m_url++ = '\0';
    char *method = text;

    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1;
    } else {
        return BAD_REQUEST;
    }

    m_url += strspn(m_url, "\t");
    m_version = strpbrk(m_url, "\t");

    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    if (strlen(m_url) == 1) {
        strcat(m_url, "judge.html");
    }

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    if (text[0] == '\0') {
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-length:", 15)) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        LOG_INFO("Oops! Unknown header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        Log::get_instance()->flush();

        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content(text);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
        return NO_REQUEST;
    }
}

http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    const char *p = strrchr(m_url, '/');

    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        char flag = m_url[1];

        char *m_url_real = (char *) malloc (sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; i++) {
            name[i-5] = m_string[i];
        }
        name[i-5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; i++, j++) {
            password[j] = m_string[i];
        }
        password[j] = '\0';

        if (0 == m_SQLVerify) {
            if (*(p + 1) == '3') {
                char *sql_insert = (char *) malloc (sizeof(char) * 200);
                strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
                strcat(sql_insert, "'");
                strcat(sql_insert, name);
                strcat(sql_insert, "','");
                strcat(sql_insert, password);
                strcat(sql_insert, "')");

                if (users.find(name) == users.end()) {
                    m_lock.lock();
                    int res = mysql_query(mysql, sql_insert);
                    users.insert(pair<string, string>(name, password));
                    m_lock.unlock();

                    if (!res) {
                        strcpy(m_url, "/log.html");
                    } else {
                        strcpy(m_url, "/registerError.html");
                    }
                } else {
                    strcpy(m_url, "registerError.html");
                }
            } else if (*(p + 1) == '2') {
                if (users.find(name) != users.end() && users[name] == password) {
                    strcpy(m_url, "/welcome.html");
                } else {
                    strcpy(m_url, "/logError.html");
                }
            }
        } else if (1 == m_SQLVerify) {
            if (*(p + 1) == '3') {
                char *sql_insert = (char *) malloc (sizeof(char) * 200);
                strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
                strcat(sql_insert, "'");
                strcat(sql_insert, name);
                strcat(sql_insert, "','");
                strcat(sql_insert, password);
                strcat(sql_insert, "')");

                if (users.find(name) == users.end()) {
                    m_lock.lock();
                    int res = mysql_query(mysql, sql_insert);
                    users.insert(pair<string, string>(name, password));
                    m_lock.unlock();

                    if (!res) {
                        strcpy(m_url, "/log.html");
                        m_lock.lock();
                        ofstream out("./CGImysql/id_passwd.txt", ios::app);
                        out << name << " " << password << endl;
                        out.close();
                        m_lock.unlock();
                    } else {
                        strcpy(m_url, "/registerError.html");
                    }
                } else {
                    strcpy(m_url, "registerError.html");
                }
            } else if (*(p + 1) == '2') {
                pid_t pid;
                int pipefd[2];
                if (pipe(pipefd) < 0) {
                    LOG_ERROR("pipe() error: %d", 4);
                    return BAD_REQUEST;
                }
                if ((pid = fork()) < 0) {
                    LOG_ERROR("fork() error: %d", 3);
                    return BAD_REQUEST;
                }

                if (pid == 0) {
                    dup2(pipefd[1], 1);
                    close(pipefd[0]);
                    execl(m_real_file, name, password, "./CGImysql/id_passwd.txt", "1", NULL);
                } else {
                    close(pipefd[1]);
                    char result;
                    int ret = read(pipefd[0], &result, 1);

                    if (ret != 1) {
                        LOG_ERROR("Pipe read error: ret = %d", ret);
                        return BAD_REQUEST;
                    }

                    LOG_INFO("%s", "Login checking");
                    Log::get_instance()->flush();

                    if (result == '1') {
                        strcpy(m_url, "/welcome.html");
                    } else {
                        strcpy(m_url, "/logError.html");
                    }

                    waitpid(pid, NULL, 0);

                }
            }
        } else {
            pid_t pid;
            int pipefd[2];

            if (pipe(pipefd) < 0) {
                LOG_ERROR("pipe() error: %d", 4);
                return BAD_REQUEST;
            }
            if ((pid = fork()) < 0) {
                LOG_ERROR("fork() error: %d", 3);
                return BAD_REQUEST;
            }

            if (pid == 0) {
                dup2(pipefd[1], 1);

                close(pipefd[0]);

                execl(m_real_file, &flag, name, password, "2", sql_user, sql_passwd, sql_name, NULL);
            } else {
                close(pipefd[1]);
                char result;
                int ret = read(pipefd[0], &result, 1);

                if (ret != 1) {
                    LOG_ERROR("pipe read error: ret = %d", ret);
                    return BAD_REQUEST;
                }
                if (flag == '2') {
                    LOG_INFO("%s", "Login checking");
                    Log::get_instance()->flush();

                    if (result == '1') {
                        strcpy(m_url, "/welcome.html");
                    } else {
                        strcpy(m_url, "/logError.html");
                    }
                } else if (flag == '3') {
                    LOG_INFO("%s", "Register checking");
                    Log::get_instance()->flush();

                    if (result == '1') {
                        strcpy(m_url, "/log.html");
                    } else {
                        strcpy(m_url, "/registerError.html");
                    }
                }

                waitpid(pid, NULL, 0);
            }
        }
    }

    if (*(p + 1) == '0') {
        char *m_url_real = (char *) malloc (sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '1') {
        char *m_url_real = (char *) malloc (sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '5') {
        char *m_url_real = (char *) malloc (sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_read_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '6') {
        char *m_url_real = (char *) malloc (sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '7') {
        char *m_url_real = (char *) malloc (sizeof(char) * 200);
        strcpy(m_url_real, "fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *) mmap (0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;
}