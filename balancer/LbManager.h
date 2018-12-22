#ifndef BEAUTY_LBMANAGER_H
#define BEAUTY_LBMANAGER_H

#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include <random>
#include <unordered_map>
#include <vector>
#include "LbConstants.h"
#include "LbLink.h"
#include "RawSocket.h"
#include "RollingLog.h"
#include "Upstream.h"
#include "Utils.h"

using namespace std;

struct ILbManager {
    int pipeFd[2];  // for signal coming from manager
    pthread_t thread;

    ILbManager() = default;
    virtual ~ILbManager() = default;
    virtual bool startup() = 0;
    virtual void serve() = 0;
    virtual void shutdown() = 0;
};

template <LbPolicy policy = LbPolicy::IP_HASHED>
struct LbManager : public ILbManager {
    mt19937 generator;
    std::uniform_int_distribution<int> uid;
    int sockListenFd;  // listen fd
    int fdHeartbeatTimer{-1};
    int epollFd;  // EPOLL_CTL_ADD sockListenFd and pipeFd[0]
    uint16_t listenPort;
    struct sockaddr_in clientAddr;

    std::unordered_map<int, LbLink*> links;
    std::vector<Upstream*> upstreams;
    int upstreamSize{0};
    RollingLog& logger;
    ostream* os{nullptr};

    /**
     * listen on localhost:listenPort, when client arrives, then direct connect to serverHost:serverPort for client
     */
    LbManager(uint16_t listenPort, const string& upstreamHosts, RollingLog& logger_);
    virtual ~LbManager();

    bool startup();
    void serve();
    void shutdown();

    void on_link();  // accept new connection
    void on_leave(int leaverFd);
    void on_leave(LbLink* link, int leaverFd);
    void on_data_in(int recvFd);
    void on_data_out(int sendFd);
    LbLink* fetch_link(int fd);

    int do_tcp_listen(struct sockaddr_in* _addr);
    int do_tcp_connect(struct sockaddr_in* _addr);

    int ip_hashed_index(const std::string& clientIp_);
    Upstream* pick_upstream_on_link(LbLink* link);
    Upstream* pick_upstream_failover(int& currentIndex, int firstIndex);
    int random_on_first_client_data_in(LbLink* link);
    bool randomed_pick_upstream(LbLink* link);
    bool ip_hashed_pick_upstream(LbLink* link);
    void client_on_leave(LbLink* link);
    void response_client_with_server_error(int clientFd_, const string& errorMsg);
    bool failover(LbLink* link);
    Upstream* get_upstream_by_host(const string& host);
    int check_upstream_connectivity(Upstream* upstream);
    void update_link_server_side(LbLink* link, Upstream* upstream, int serverFd_, char lbPolicy);
};

template <LbPolicy policy>
LbManager<policy>::LbManager(uint16_t listenPort_, const string& upstreamHosts, RollingLog& logger_)
    : listenPort(listenPort_), logger(logger_), os(logger_.ofs) {
    vector<string> result = split(upstreamHosts, ',');
    for (const auto& server : result) {
        auto pUpstream = new Upstream(server);
        if (pUpstream->check()) {
            upstreams.push_back(pUpstream);
            ++upstreamSize;
        } else {
            *os << "init upstream " << server << " failed." << endl;
            delete pUpstream;
        }
    }
}

template <LbPolicy policy>
LbManager<policy>::~LbManager() {
    for (Upstream* upstream : upstreams) {
        delete upstream;
    }
    upstreams.clear();
}

template <LbPolicy policy>
bool LbManager<policy>::startup() {
    if (upstreamSize <= 0) return false;

    random_device rd;
    generator.seed(rd());
    uid = std::uniform_int_distribution<int>(0, upstreamSize - 1);

    clientAddr.sin_family = AF_INET;
    clientAddr.sin_port = htons(listenPort);
    clientAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    // listen
    sockListenFd = do_tcp_listen(&clientAddr);
    if (sockListenFd < 0) {
        *os << "tcp listen failed " << errno << " " << strerror(errno);
        return false;
    }

    // pipe
    if (pipe(pipeFd) < 0) {
        *os << "pipe failed " << errno << " " << strerror(errno);
        return false;
    }

    // epoll
    epollFd = epoll_create(EPOLL_BUFFER_SIZE);  // epoll_create(int size); size is no longer used

    if (create_timer(HeartbeatMilliseconds, &fdHeartbeatTimer)) {
        epoll_add(epollFd, fdHeartbeatTimer);
    }

    // epoll <--> listen, pipe
    epoll_add(epollFd, sockListenFd);
    epoll_add(epollFd, pipeFd[0]);
    return true;
}

template <LbPolicy policy>
void LbManager<policy>::serve() {
    struct epoll_event events[EPOLL_BUFFER_SIZE];

    uint64_t dummy;
    while (true) {
        int count = epoll_wait(epollFd, events, EPOLL_BUFFER_SIZE, -1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                *os << "epoll error\n";
                return;
            }
        }
        for (int i = 0; i < count; i++) {
            int fdReady = events[i].data.fd;
            if (fdReady == pipeFd[0]) {
                *os << "pipe data arrived, proxy serve finish, going to shutdown proxy\n";
                return;
            } else if (fdReady == sockListenFd) {
                on_link();
            }
            if (fdReady == fdHeartbeatTimer) {
                read(fdReady, &dummy, sizeof(dummy));
                os = logger.update();
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

template <LbPolicy policy>
void LbManager<policy>::shutdown() {
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
template <LbPolicy policy>
Upstream* LbManager<policy>::pick_upstream_on_link(LbLink* link) {
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
    *os << now_string() << "no server available now" << endl;
    return nullptr;
}

/**
 * pick upstream, fail over to other server when finding current server failed to serve
 * @param currentIndex current server index in upstreams array
 * @param firstIndex the first time index picked, it should be calculated by ip hashed value
 * @return
 */
template <LbPolicy policy>
Upstream* LbManager<policy>::pick_upstream_failover(int& currentIndex, int firstIndex) {
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

template <LbPolicy policy>
void LbManager<policy>::response_client_with_server_error(int clientFd_, const string& errorMsg) {
    // TODO current close clientFd_, client recv ConnectionResetError(104, 'Connection reset by peer')
    // TODO close more gently, current send back then close, we even don't wait client's ack
    static const string header{"HTTP/1.1 503 Service Unavailable\r\n\r\n"};
    if (clientFd_ > 0) {
        send(clientFd_, header.c_str(), header.size(), 0);
        close(clientFd_);
    }
}

template <LbPolicy policy>
int LbManager<policy>::ip_hashed_index(const std::string& clientIp_) {
    int index = 0;
    for (char c : clientIp_) {
        if (c != '.') {
            index += c - '0';
        }
    }
    return index % upstreamSize;
}

/**
 * python client with async query ticket need to forward to appointed host
 * other python client will choose random host
 * other client stick to ip hashed host
 */
template <LbPolicy policy>
int LbManager<policy>::random_on_first_client_data_in(LbLink* link) {
    int ret = link->parse_client_content();
    if (ret == 1) {
        if (link->source == LbClientSource::PythonClient) {
            if (link->isAsyncCall) {
                Upstream* upstream = get_upstream_by_host(link->asyncHost);
                if (upstream) {
                    int serverFd = check_upstream_connectivity(upstream);
                    if (serverFd > 0) {
                        update_link_server_side(link, upstream, serverFd, LbPolicyRandomTicket);
                        return 1;
                    }
                    return -1;
                } else {
                    *os << now_string() << " can not find target async host " << link->asyncHost << endl;
                    return -1;
                }
            } else {  // randomly pick one
                return randomed_pick_upstream(link) ? 1 : -1;
            }
        }
    } else if (link->isAsyncCall && ret <= -2) {  // no complete content
        // *os << "parse_client_content failed " << ret << " " << link->source << endl;
        return 0;
    }
    return ip_hashed_pick_upstream(link) ? 1 : -1;  // at last, restore to ip hashed method
}

template <LbPolicy policy>
bool LbManager<policy>::ip_hashed_pick_upstream(LbLink* link) {
    Upstream* upstream = nullptr;
    int serverFd_ = -1;
    while (true) {
        upstream = pick_upstream_on_link(link);
        if (upstream == nullptr) {
            return false;
        }
        ++link->onLinkRetryServerCount; // add count here so that it won't trap in infinite loop
        if (!upstream->good && (link->startTimestamp - upstream->badTimestamp) < FirstUpstreamBadRetryTimeThreshold) {
            continue;
        }

        serverFd_ = check_upstream_connectivity(upstream);  // fd to server
        if (serverFd_ > 0) {
            break;
        }
    }

    update_link_server_side(link, upstream, serverFd_, LbPolicyIpHashed);
    return true;
}

template <LbPolicy policy>
bool LbManager<policy>::randomed_pick_upstream(LbLink* link) {
    Upstream* upstream = nullptr;
    int serverFd_ = -1;
    while (true) {
        if (link->check_random_retry_count_exceed()) {
            return false;
        }
        ++link->randomRetryServerCount; // add count here so that it won't trap in infinite loop
        upstream = upstreams[uid(generator)];
        if (!upstream->good && (link->startTimestamp - upstream->badTimestamp) < FirstUpstreamBadRetryTimeThreshold) {
            continue;
        }

        serverFd_ = check_upstream_connectivity(upstream);  // fd to server
        if (serverFd_ > 0) {
            break;
        }
    }

    update_link_server_side(link, upstream, serverFd_, LbPolicyRandom);
    return true;
}

template <LbPolicy policy>
void LbManager<policy>::on_link() {
    struct sockaddr_in clientAddr;
    int socklen = sizeof(sockaddr_in);
    int clientFd_ = accept(sockListenFd, (struct sockaddr*)&clientAddr, (socklen_t*)&socklen);
    if (clientFd_ <= 0) {
        *os << "accept from client error " << errno << " " << strerror(errno);
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
        *os << now_string() << "no server available now" << endl;
        return;
    }

    if (policy == LbPolicy::IP_HASHED) {
        if (ip_hashed_pick_upstream(link)) {
            // success
        } else {
            response_client_with_server_error(clientFd_, "no server available now");
            delete link;
            return;
        }
    }

    set_nonblock(clientFd_);
    links[clientFd_] = link;
    epoll_add(epollFd, clientFd_);  // register event
}

template <LbPolicy policy>
void LbManager<policy>::on_leave(int leaverFd) {
    LbLink* link = fetch_link(leaverFd);
    if (link == nullptr) {
        return;
    }
    on_leave(link, leaverFd);
}

template <LbPolicy policy>
void LbManager<policy>::on_leave(LbLink* link, int leaverFd) {
    link->on_leave();

    // links
    links.erase(link->clientFd);
    links.erase(link->serverFd);

    // unregister event
    epoll_delete(epollFd, link->clientFd);
    epoll_delete(epollFd, link->serverFd);
    link->print_leave_info(leaverFd, *os);
    delete link;
}

template <LbPolicy policy>
void LbManager<policy>::client_on_leave(LbLink* link) {
    close(link->clientFd);
    links.erase(link->clientFd);
    epoll_delete(epollFd, link->clientFd);
    *os << "client_on_leave " << link->clientEndpoint << " " << link->clientTotalBytes << endl;
    delete link;
}

template <LbPolicy policy>
LbLink* LbManager<policy>::fetch_link(int fd) {
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
template <LbPolicy policy>
bool LbManager<policy>::failover(LbLink* link) {
    if (link->client_do_not_support_failover()) return false;
    if (link->serverTotalBytes != 0) return false;  // server normal leave, no need failover

    Upstream* upstream = nullptr;
    if (policy == LbPolicy::IP_HASHED) {
        if (!link->hasFirstUpstreamTriedAgain) {  // retry this upstream again
            upstream = link->pUpstream;
            link->hasFirstUpstreamTriedAgain = true;
        } else {
            upstream = pick_upstream_failover(link->currentUpstreamIndex, link->firstUpstreamIndex);
        }
    } else {
        if (link->check_random_retry_count_exceed()) {
            return false;
        }

        if (link->isAsyncCall && link->source == LbClientSource::PythonClient) {
            upstream = get_upstream_by_host(link->asyncHost);
        } else {
            upstream = upstreams[uid(generator)];
        }
        ++link->randomRetryServerCount;
    }

    if (upstream == nullptr) return false;

    int serverFd_ = do_tcp_connect(&upstream->serverAddr);  // fd to server
    if (serverFd_ <= 0) {
        *os << "can not connect to server " << errno << " " << strerror(errno);
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

    *os << now_string() << " failover " << link->clientEndpoint << " <--> " << upstream->endpoint << endl;
    link->reset_server_side_for_failover(upstream, serverFd_);
    if (link->on_server_send() < 0) {
        return failover(link);
    }
    return true;
}

template <LbPolicy policy>
void LbManager<policy>::on_data_in(int recvFd) {
    LbLink* link = fetch_link(recvFd);
    if (link == nullptr) {
        return;
    }

    if (link->is_buffer_not_empty(recvFd)) {  // wait buffer to be empty
        return;
    }

    // recv
    int ret = link->on_recv(recvFd);
    // *os << "on_data_in do_tcp_recv " << recvFd << " " << ret << endl;
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
        if (policy == LbPolicy::RANDOMED) {
            if (link->is_client_side(recvFd) && link->pUpstream == nullptr) {
                ret = random_on_first_client_data_in(link);
                if (ret < 0) {
                    client_on_leave(link);
                    return;
                } else if (ret == 0) {
                    //                    *os << "wait for complete client data: " << endl;
                    //                    link->print_client_request(*os);
                    link->clearClientBuffer = false;
                    return;  // wait for complete client data
                }
                link->clearClientBuffer = true;  // now we can send whole to server
            }
        }
    }

    // send
    int otherSideFd = link->other_side_fd(recvFd);
    ret = link->on_send(otherSideFd);
    // *os << "on_data_in do_tcp_send " << otherSideFd << " " << ret << endl;
    if (ret == 0) {
        // start watch EPOLLOUT
        epoll_mod2both(epollFd, otherSideFd);
    } else if (ret < 0) {
        *os << "on_data_in error " << recvFd << endl;
        on_leave(otherSideFd);
        return;
    }
}

template <LbPolicy policy>
void LbManager<policy>::on_data_out(int sendFd) {
    LbLink* link = fetch_link(sendFd);
    if (link == nullptr) {
        return;
    }

    int ret = link->on_send(sendFd);
    if (ret == 0) {        // keep watch EPOLLOUT
    } else if (ret > 0) {  // send success, then remove EPOLLOUT
        epoll_mod2in(epollFd, sendFd);
    } else {
        *os << "on_data_out error " << sendFd << endl;
        on_leave(sendFd);
    }
}

template <LbPolicy policy>
int LbManager<policy>::do_tcp_listen(struct sockaddr_in* _addr) {
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

template <LbPolicy policy>
int LbManager<policy>::do_tcp_connect(struct sockaddr_in* _addr) {
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

template <LbPolicy policy>
Upstream* LbManager<policy>::get_upstream_by_host(const string& host) {
    if (host.empty()) return nullptr;
    for (Upstream* upstream : upstreams) {
        if (upstream->is_host_match(host)) {
            return upstream;
        }
    }
    return nullptr;
}

template <LbPolicy policy>
int LbManager<policy>::check_upstream_connectivity(Upstream* upstream) {
    int serverFd_ = do_tcp_connect(&upstream->serverAddr);  // fd to server
    if (serverFd_ <= 0) {
        *os << "can not connect to server " << upstream->endpoint << " " << errno << " " << strerror(errno);
        upstream->set_status(false);
        return -1;
    } else {
        upstream->set_status(true);
        return serverFd_;
    }
}

template <LbPolicy policy>
void LbManager<policy>::update_link_server_side(LbLink* link, Upstream* upstream, int serverFd_, char lbPolicy) {
    link->serverFd = serverFd_;
    link->pUpstream = upstream;

    set_nonblock(serverFd_);
    links[serverFd_] = link;
    epoll_add(epollFd, serverFd_);
    link->print_on_link_info(lbPolicy, *os);
}

#endif
