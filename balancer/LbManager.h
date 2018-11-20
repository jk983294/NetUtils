#ifndef BEAUTY_LBMANAGER_H
#define BEAUTY_LBMANAGER_H

#include <pthread.h>
#include <unordered_map>
#include <vector>
#include "LbLink.h"
#include "Upstream.h"

using namespace std;

struct LbManager {
    int sockListenFd;  // listen fd
    int pipeFd[2];     // for signal coming from manager
    int epollFd;       // EPOLL_CTL_ADD sockListenFd and pipeFd[0]

    uint16_t listenPort;

    struct sockaddr_in clientAddr;

    std::unordered_map<int, LbLink*> links;
    // std::unordered_map<std::string, int> clinetIp2serverIndex;
    std::vector<Upstream*> upstreams;
    int upstreamSize{0};
    pthread_t thread;

    /**
     * listen on localhost:listenPort, when client arrives, then direct connect to serverHost:serverPort for client
     */
    LbManager(uint16_t listenPort, const string& upstreamHosts);
    ~LbManager();

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
    void response_client_with_server_error(int clientFd_, const string& errorMsg);
    bool failover(LbLink* link);
};

#endif
