#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include "LbManager.h"
#include "RawSocket.h"
#include "Utils.h"

using namespace std;

LbManager::LbManager(unsigned int listenPort_, const string& upstreamHosts) : listenPort(listenPort_) {
    vector<string> result = split(upstreamHosts, ',');
    for (const auto& server : result) {
        auto pUpstream = new Upstream(server);
        if (pUpstream->check()) {
            upstreams.push_back(pUpstream);
        } else {
            cerr << "init upstream " << server << " failed." << endl;
            delete pUpstream;
        }
    }
}

LbManager::~LbManager() {
    clinetIp2serverIndex.clear();
    for (Upstream* upstream : upstreams) {
        delete upstream;
    }
    upstreams.clear();
}

bool LbManager::startup() {
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_port = htons(listenPort);
    clientAddr.sin_addr.s_addr = htonl(INADDR_ANY);

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

void LbManager::serve() {
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
                    on_data_out(events[i].data.fd);
                }
                if (events[i].events & EPOLLIN) {
                    on_data_in(events[i].data.fd);
                }
            }
        }
    }
}

void LbManager::shutdown() {
    close(sockListenFd);
    close(epollfd);
    close(pipefd[0]);
    close(pipefd[1]);

    for (auto it = links.begin(); it != links.end(); it++) {
        LbLink* link = it->second;
        close(link->clientFd);
        close(link->serverFd);
        delete link;
    }
    links.empty();
}

/**
 * pick upstream, can be used first time client pick server or
 * fail over to other server when finding current server failed to serve
 * @param clientIp
 * @param currentIndex current server index in upstreams array
 * @param firstIndex the first time index picked, it should be calculated by ip hashed value
 * @return
 */
Upstream* LbManager::pick_upstream(std::string clientIp, int& currentIndex, int firstIndex) {
    int size = upstreams.size();
    if (size == 0) return nullptr;

    bool isPureFailover = (firstIndex >= 0);
    if (!isPureFailover) {  // first time pick upstream
        if (clinetIp2serverIndex.find(clientIp) != clinetIp2serverIndex.end()) {
            currentIndex = clinetIp2serverIndex[clientIp];
            if (upstreams[currentIndex]->good)
                return upstreams[currentIndex];
            else
                clinetIp2serverIndex.erase(clientIp);
        }
        currentIndex = 0;
        for (char c : clientIp) {
            if (c != '.') {
                currentIndex += c - '0';
            }
        }
        currentIndex = (currentIndex % size);
        firstIndex = currentIndex;
        Upstream* picked = upstreams[currentIndex];
        clinetIp2serverIndex[clientIp] = currentIndex;
        if (picked->good) {
            return picked;
        }

        // cout << "picked one status not good, switch to others" << endl;
    }

    // fail over to other server
    for (int i = (currentIndex + 1) % size; i != firstIndex;) {
        if (upstreams[i]->good) {
            currentIndex = i;
            if (!isPureFailover) {
                // pure fail-over means already have first server tried but failed to finish client's request
                // in that case, the server may not be bad, only sometimes not response, so don't change fast index
                // query
                clinetIp2serverIndex[clientIp] = currentIndex;
            }
            return upstreams[currentIndex];
        }
        ++i;
        i %= size;
    }
    return nullptr;
}

void LbManager::response_client_with_server_error(int clientFd_, const string& errorMsg) {
    // TODO current close clientFd_, client recv ConnectionResetError(104, 'Connection reset by peer')
    // TODO close more gently, current send back then close, we even don't wait client's ack
    static const string header{"HTTP/1.1 503 Service Unavailable\r\n\r\n"};
    if (clientFd_ > 0) {
        send(clientFd_, header.c_str(), header.size(), 0);
        close(clientFd_);
    }
}

void LbManager::on_link() {
    struct sockaddr_in clientAddr;
    int socklen = sizeof(sockaddr_in);
    int clientFd_ = accept(sockListenFd, (struct sockaddr*)&clientAddr, (socklen_t*)&socklen);
    if (clientFd_ <= 0) {
        perror("accept from client error");
        return;
    }
    string clientIp = inet_ntoa(clientAddr.sin_addr);
    string clientEndpoint_ = clientIp;
    clientEndpoint_ += ':';
    clientEndpoint_ += std::to_string(ntohs(clientAddr.sin_port));

    int currentIndex = -1;
    int firstIndex = -1;
    Upstream* upstream = nullptr;
    int serverFd_ = -1;
    while (true) {
        upstream = pick_upstream(clientIp, currentIndex, firstIndex);
        if (upstream == nullptr) {
            cerr << "no server available now" << endl;
            response_client_with_server_error(clientFd_, "no server available now");
            return;
        }
        if (firstIndex == -1) {
            firstIndex = currentIndex;
        }
        serverFd_ = do_tcp_connect(&upstream->serverAddr);  // fd to server
        if (serverFd_ <= 0) {
            string errorMsg{"can not connect to server "};
            errorMsg += upstream->endpoint;
            perror(errorMsg.c_str());
            upstream->good = false;
        } else {
            break;
        }
    }

    LbLink* link = new LbLink(clientFd_, serverFd_, clientEndpoint_, upstream);
    link->firstUpstreamIndex = firstIndex;
    link->currentUpstreamIndex = currentIndex;

    set_nonblock(clientFd_);
    set_nonblock(serverFd_);

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

    cout << now_string() << " open " << clientEndpoint_ << " <--> " << upstream->endpoint << endl;
}

void LbManager::on_leave(int leaverFd) {
    LbLink* link = fetch_link(leaverFd);
    if (link == nullptr) {
        return;
    }
    on_leave(link, leaverFd);
}

void LbManager::on_leave(LbLink* link, int leaverFd) {
    link->on_leave();

    // links
    links.erase(link->clientFd);
    links.erase(link->serverFd);

    // unregister event
    struct epoll_event ev;
    ev.data.fd = link->clientFd;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, link->clientFd, &ev);
    ev.data.fd = link->serverFd;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, link->serverFd, &ev);
    link->print_leave_info(leaverFd);
    delete link;
}

LbLink* LbManager::fetch_link(int fd) {
    if (links.find(fd) != links.end()) {
        return links.find(fd)->second;
    } else {
        // broken means the link already removed, so we cannot figure out the other side fd
        return nullptr;
    }
}

/**
 * failover only enabled for those servers who can connect to but didn't respond any data back
 * @param link
 * @return
 */
bool LbManager::failover(LbLink* link) {
    if (link->client_do_not_support_failover()) return false;
    if (link->serverTotalBytes != 0) return false;  // server normal leave, no need failover

    Upstream* upstream = nullptr;
    if (!link->hasFirstUpstreamTriedAgain) {  // retry this upstream again
        upstream = link->pUpstream;
        link->hasFirstUpstreamTriedAgain = true;
    } else {
        upstream = pick_upstream("", link->currentUpstreamIndex, link->firstUpstreamIndex);
    }

    if (upstream == nullptr) return false;

    int serverFd_ = do_tcp_connect(&upstream->serverAddr);  // fd to server
    if (serverFd_ <= 0) {
        perror("can not connect to server");
        upstream->good = false;
        return failover(link);  // failover again
    }

    // old server leave
    struct epoll_event ev;
    int oldServerFd = link->serverFd;
    ev.data.fd = oldServerFd;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, oldServerFd, &ev);
    close(oldServerFd);
    links.erase(oldServerFd);

    // new server setup
    links[serverFd_] = link;
    set_nonblock(serverFd_);
    ev.data.fd = serverFd_;
    ev.events = EPOLLIN;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, serverFd_, &ev);

    cout << now_string() << " failover " << link->clientEndpoint << " <--> " << upstream->endpoint << endl;
    link->reset_server_side_for_failover(upstream, serverFd_);
    if (link->on_server_send() < 0) {
        return failover(link);
    }
    return true;
}

void LbManager::on_data_in(int recvFd) {
    LbLink* link = fetch_link(recvFd);
    if (link == nullptr) {
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
        if (link->is_server_side(recvFd) && failover(link)) {
            // do nothing
        } else {
            on_leave(recvFd);
        }
        return;
    } else {
        //        if(link->is_client_side(recvFd)){
        //            link->parse_client_header();
        //        }
    }

    // send
    int otherSideFd = link->other_side_fd(recvFd);
    ret = link->on_send(otherSideFd);
    // cout << "on_data_in do_tcp_send " << otherSideFd << " " << ret << endl;
    if (ret == 0) {
        // start watch EPOLLOUT
        struct epoll_event ev;
        ev.data.fd = otherSideFd;
        ev.events = EPOLLIN | EPOLLOUT;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, otherSideFd, &ev);
    } else if (ret < 0) {
        cerr << "on_data_in error " << recvFd << endl;
        on_leave(otherSideFd);
        return;
    }
}

void LbManager::on_data_out(int sendFd) {
    LbLink* link = fetch_link(sendFd);
    if (link == nullptr) {
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
        cerr << "on_data_out error " << sendFd << endl;
        on_leave(sendFd);
    }
}

int LbManager::do_tcp_listen(struct sockaddr_in* _addr) {
    const int flag = 1;
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
        return -1;
    }
    if (bind(listenFd, (const struct sockaddr*)_addr, sizeof(struct sockaddr_in)) < 0) {
        return -1;
    }
    if (listen(listenFd, SOMAXCONN) < 0) {
        return -1;
    }
    return listenFd;
}

int LbManager::do_tcp_connect(struct sockaddr_in* _addr) {
    const int flag = 1;
    int connectFd = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(connectFd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
        return -1;
    }
    if (connect(connectFd, (const struct sockaddr*)_addr, sizeof(struct sockaddr_in)) < 0) {
        return -1;
    }
    return connectFd;
}
