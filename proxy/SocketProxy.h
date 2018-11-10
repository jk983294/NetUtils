#ifndef BEAUTY_SOCKETPROXY_H
#define BEAUTY_SOCKETPROXY_H

#include <pthread.h>
#include <unordered_map>
#include "Link.h"

using namespace std;

struct SocketProxy {
    int sockListenFd;  // listen fd
    int pipefd[2];     // for signal coming from manager
    int epollfd;       // EPOLL_CTL_ADD sockListenFd and pipefd[0]

    unsigned int listenPort;
    string serverHost;
    unsigned int serverPort;
    struct sockaddr_in clientAddr;
    struct sockaddr_in serverAddr;
    std::unordered_map<int, Link*> links;
    pthread_t thread;
    string serverEndpoint;

    /**
     * listen on localhost:listenPort, when client arrives, then direct connect to serverHost:serverPort for client
     */
    SocketProxy(unsigned int listenPort, const string& serverHost_, unsigned int serverPort_);
    ~SocketProxy() = default;

    bool startup();
    void serve();
    void shutdown();

    void on_link();  // accept new connection
    void on_leave(int fd);
    void on_data_in(int recvFd);
    void on_data_out(int sendFd);
    bool fetch_link(int fd, int* otherSideFd, Link** link);

    int do_tcp_listen(struct sockaddr_in* _addr);
    int do_tcp_connect(struct sockaddr_in* _addr);
};

inline SocketProxy::SocketProxy(unsigned int listenPort_, const string& serverHost_, unsigned int serverPort_)
    : listenPort(listenPort_), serverHost(serverHost_), serverPort(serverPort_) {
    if (serverHost == "localhost") {
        serverHost = "0.0.0.0";
    }
    serverEndpoint = serverHost_ + ':' + std::to_string(serverPort_);
}

inline bool SocketProxy::startup() {
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_port = htons(listenPort);
    clientAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);

    if (inet_pton(AF_INET, serverHost.c_str(), &serverAddr.sin_addr) <= 0) {
        perror("inet_pton convert address from text to binary form failed");
        return false;
    }

    // listen
    sockListenFd = do_tcp_listen(&clientAddr);
    if (sockListenFd < 0) {
        perror("tcp listen failed");
        return false;
    }

    // pipe
    if (pipe(pipefd) < 0) {
        perror("pipe failed");
        return false;
    }

    // epoll
    epollfd = epoll_create(EPOLL_BUFFER_SIZE);  // epoll_create(int size); size is no longer used

    // epoll <--> listen, pipe
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sockListenFd;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, sockListenFd, &ev);

    ev.data.fd = pipefd[0];
    epoll_ctl(epollfd, EPOLL_CTL_ADD, pipefd[0], &ev);
    return true;
}

inline void SocketProxy::serve() {
    struct epoll_event events[EPOLL_BUFFER_SIZE];

    while (true) {
        int count = epoll_wait(epollfd, events, EPOLL_BUFFER_SIZE, -1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                cerr << "epoll error\n";
                return;
            }
        }
        for (int i = 0; i < count; i++) {
            if (events[i].data.fd == pipefd[0]) {
                cout << "pipe data arrived, proxy serve finish, going to shutdown proxy\n";
                return;
            } else if (events[i].data.fd == sockListenFd) {
                on_link();
            } else {
                if (events[i].events & EPOLLOUT) {
                    /**
                     * socket is always writable as long as there's space in socket in-kernel send buffer
                     * when socket buffer is not enough, you'll get EAGAIN from send or write call
                     * in this case you start waiting for output to be possible by including EPOLLOUT.
                     * once you get that, write your pending output bytes to the socket, and if you are successful,
                     * remove EPOLLOUT from the events.
                     */
                    on_data_out(events[i].data.fd);
                }
                if (events[i].events & EPOLLIN) {
                    on_data_in(events[i].data.fd);
                }
            }
        }
    }
}

inline void SocketProxy::shutdown() {
    close(sockListenFd);
    close(epollfd);
    close(pipefd[0]);
    close(pipefd[1]);

    for (std::unordered_map<int, Link*>::iterator it = links.begin(); it != links.end(); it++) {
        Link* link = it->second;
        close(link->clientFd);
        close(link->serverFd);
        delete link;
    }
    links.empty();
}

inline void SocketProxy::on_link() {
    struct sockaddr_in clientAddr;
    int socklen = sizeof(sockaddr_in);
    int clientFd_ = accept(sockListenFd, (struct sockaddr*)&clientAddr, (socklen_t*)&socklen);
    string clientEndpoint_{inet_ntoa(clientAddr.sin_addr)};
    clientEndpoint_ += ':';
    clientEndpoint_ += std::to_string(ntohs(clientAddr.sin_port));

    int serverFd_ = do_tcp_connect(&serverAddr);  // fd to server
    if (serverFd_ <= 0) {
        perror("can not connect to server");
        return;
    }
    Link* link = new Link(clientFd_, serverFd_, clientEndpoint_, serverEndpoint);

    int flags;
    flags = fcntl(clientFd_, F_GETFL, 0);
    fcntl(clientFd_, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(serverFd_, F_GETFL, 0);
    fcntl(serverFd_, F_SETFL, flags | O_NONBLOCK);

    links[clientFd_] = link;
    links[serverFd_] = link;

    // register event
    struct epoll_event ev;
    ev.data.fd = clientFd_;
    ev.events = EPOLLIN;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, clientFd_, &ev);
    ev.data.fd = serverFd_;
    ev.events = EPOLLIN;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, serverFd_, &ev);

    cout << "open " << clientFd_ << " " << clientEndpoint_ << " <--> " << serverFd_ << " " << serverEndpoint << endl;
}

inline void SocketProxy::on_leave(int leaverFd) {
    Link* link;
    int otherSideFd;
    if (!fetch_link(leaverFd, &otherSideFd, &link)) {
        return;
    }

    // socket
    close(leaverFd);
    close(otherSideFd);

    // links
    links.erase(leaverFd);
    links.erase(otherSideFd);

    // unregister event
    struct epoll_event ev;
    ev.data.fd = leaverFd;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, leaverFd, &ev);
    ev.data.fd = otherSideFd;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, otherSideFd, &ev);
    link->print_leave_info(leaverFd);
    delete link;
}

inline bool SocketProxy::fetch_link(int fd, int* otherSideFd, Link** link) {
    if (links.find(fd) != links.end()) {
        *link = links.find(fd)->second;
        if (fd == (*link)->clientFd)
            *otherSideFd = (*link)->serverFd;
        else
            *otherSideFd = (*link)->clientFd;
        return true;
    } else {
        // broken means the link already removed, so we cannot figure out the other side fd
        cout << "broken " << fd << " <--> ?" << endl;
        return false;
    }
}

inline void SocketProxy::on_data_in(int recvFd) {
    Link* link;
    int otherSideFd = 0;
    if (!fetch_link(recvFd, &otherSideFd, &link)) {
        return;
    }

    if (link->is_buffer_not_empty(recvFd)) {  // wait buffer to be empty
        return;
    }

    // recv
    int ret = link->on_recv(recvFd);
    // cout << "on_data_in do_tcp_recv " << recvFd << " " << ret << endl;
    if (ret == 0) {
        return;
    } else if (ret < 0) {
        on_leave(recvFd);
        return;
    }

    // send
    ret = link->on_send(otherSideFd);
    // cout << "on_data_in do_tcp_send " << otherSideFd << " " << ret << endl;
    if (ret == 0) {
        // start watch EPOLLOUT
        struct epoll_event ev;
        ev.data.fd = otherSideFd;
        ev.events = EPOLLIN | EPOLLOUT;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, otherSideFd, &ev);
    } else if (ret < 0) {
        on_leave(recvFd);
        return;
    }
}

inline void SocketProxy::on_data_out(int sendFd) {
    Link* link;
    int otherSideFd = 0;
    if (!fetch_link(sendFd, &otherSideFd, &link)) {
        return;
    }

    int ret = link->on_send(sendFd);
    if (ret == 0) {        // keep watch EPOLLOUT
    } else if (ret > 0) {  // send success, then remove EPOLLOUT
        struct epoll_event ev;
        ev.data.fd = sendFd;
        ev.events = EPOLLIN;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, sendFd, &ev);
    } else {
        on_leave(sendFd);
    }
}

inline int SocketProxy::do_tcp_listen(struct sockaddr_in* _addr) {
    const int flag = 1;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
        return -1;
    }
    if (bind(listenfd, (const struct sockaddr*)_addr, sizeof(struct sockaddr_in)) < 0) {
        return -1;
    }
    if (listen(listenfd, SOMAXCONN) < 0) {
        return -1;
    }
    return listenfd;
}

inline int SocketProxy::do_tcp_connect(struct sockaddr_in* _addr) {
    const int flag = 1;
    int connectfd = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(connectfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
        return -1;
    }
    if (connect(connectfd, (const struct sockaddr*)_addr, sizeof(struct sockaddr_in)) < 0) {
        return -1;
    }
    return connectfd;
}

#endif
