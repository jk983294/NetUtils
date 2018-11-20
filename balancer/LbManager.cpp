#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include "LbManager.h"
#include "RawSocket.h"
#include "Utils.h"

using namespace std;

LbManager::LbManager(uint16_t listenPort_, const string& upstreamHosts) : listenPort(listenPort_) {
    vector<string> result = split(upstreamHosts, ',');
    for (const auto& server : result) {
        auto pUpstream = new Upstream(server);
        if (pUpstream->check()) {
            upstreams.push_back(pUpstream);
            ++upstreamSize;
        } else {
            cerr << "init upstream " << server << " failed." << endl;
            delete pUpstream;
        }
    }
}

LbManager::~LbManager() {
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
    if (pipe(pipeFd) < 0) {
        perror("pipe failed");
        return false;
    }

    // epoll
    epollFd = epoll_create(EPOLL_BUFFER_SIZE);  // epoll_create(int size); size is no longer used

    // epoll <--> listen, pipe
    epoll_add(epollFd, sockListenFd);
    epoll_add(epollFd, pipeFd[0]);
    return true;
}

void LbManager::serve() {
    struct epoll_event events[EPOLL_BUFFER_SIZE];

    while (true) {
        int count = epoll_wait(epollFd, events, EPOLL_BUFFER_SIZE, -1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                cerr << "epoll error\n";
                return;
            }
        }
        for (int i = 0; i < count; i++) {
            if (events[i].data.fd == pipeFd[0]) {
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
    close(epollFd);
    close(pipeFd[0]);
    close(pipeFd[1]);

    for (auto it = links.begin(); it != links.end(); it++) {
        LbLink* link = it->second;
        close(link->clientFd);
        close(link->serverFd);
        delete link;
    }
    links.empty();
}

/**
 * pick upstream, used first time client pick server
 * @param currentIndex current server index in upstreams array
 * @param firstIndex the first time index picked, it should be calculated by ip hashed value
 * @return
 */
Upstream* LbManager::pick_upstream_on_link(LbLink* link) {
    if (upstreamSize == 0) return nullptr;
    if (link->check_on_link_retry_count_exceed()) {
        return nullptr;
    }

    int currentIndex = link->currentUpstreamIndex;

    if (currentIndex < 0) {
        currentIndex = link->firstUpstreamIndex;
        link->currentUpstreamIndex = currentIndex;
        return upstreams[currentIndex];
    }
    currentIndex = (link->currentUpstreamIndex + 1) % upstreamSize;
    if (currentIndex != link->firstUpstreamIndex) {
        link->currentUpstreamIndex = currentIndex;
        return upstreams[currentIndex];
    }
    cerr << "no server available now" << endl;
    return nullptr;
}

/**
 * pick upstream, fail over to other server when finding current server failed to serve
 * @param currentIndex current server index in upstreams array
 * @param firstIndex the first time index picked, it should be calculated by ip hashed value
 * @return
 */
Upstream* LbManager::pick_upstream_failover(int& currentIndex, int firstIndex) {
    if (upstreamSize == 0) return nullptr;

    // fail over to other server
    for (int i = (currentIndex + 1) % upstreamSize; i != firstIndex;) {
        if (upstreams[i]->good) {
            currentIndex = i;
            return upstreams[currentIndex];
        }
        ++i;
        i %= upstreamSize;
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

int LbManager::ip_hashed_index(const std::string& clientIp_) {
    if (upstreamSize <= 0) return -1;
    int index = 0;
    for (char c : clientIp_) {
        if (c != '.') {
            index += c - '0';
        }
    }
    return index % upstreamSize;
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
    LbLink* link = new LbLink(clientFd_, clientEndpoint_);
    link->firstUpstreamIndex = ip_hashed_index(clientIp);
    if (link->firstUpstreamIndex < 0) {
        delete link;
        cerr << "no server available now" << endl;
        return;
    }

    Upstream* upstream = nullptr;
    int serverFd_ = -1;
    while (true) {
        upstream = pick_upstream_on_link(link);
        if (upstream == nullptr) {
            response_client_with_server_error(clientFd_, "no server available now");
            delete link;
            return;
        }
        if (!upstream->good && (link->startTimestamp - upstream->badTimestamp) < FirstUpstreamBadRetryTimeThreshold) {
            continue;
        }

        ++link->onLinkRetryServerCount;
        serverFd_ = do_tcp_connect(&upstream->serverAddr);  // fd to server
        if (serverFd_ <= 0) {
            string errorMsg{"can not connect to server "};
            errorMsg += upstream->endpoint;
            perror(errorMsg.c_str());
            upstream->set_status(false);
        } else {
            upstream->set_status(true);
            break;
        }
    }

    link->serverFd = serverFd_;
    link->pUpstream = upstream;

    set_nonblock(clientFd_);
    set_nonblock(serverFd_);

    links[clientFd_] = link;
    links[serverFd_] = link;

    // register event
    epoll_add(epollFd, clientFd_);
    epoll_add(epollFd, serverFd_);
    link->print_on_link_info();
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
    epoll_delete(epollFd, link->clientFd);
    epoll_delete(epollFd, link->serverFd);
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
        upstream = pick_upstream_failover(link->currentUpstreamIndex, link->firstUpstreamIndex);
    }

    if (upstream == nullptr) return false;

    int serverFd_ = do_tcp_connect(&upstream->serverAddr);  // fd to server
    if (serverFd_ <= 0) {
        perror("can not connect to server");
        upstream->set_status(false);
        return failover(link);  // failover again
    }

    // old server leave
    int oldServerFd = link->serverFd;
    epoll_delete(epollFd, oldServerFd);
    close(oldServerFd);
    links.erase(oldServerFd);

    // new server setup
    links[serverFd_] = link;
    set_nonblock(serverFd_);
    epoll_add(epollFd, serverFd_);

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
        epoll_mod2both(epollFd, otherSideFd);
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
        epoll_mod2in(epollFd, sendFd);
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
