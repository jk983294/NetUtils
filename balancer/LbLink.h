#ifndef BEAUTY_LINK_H
#define BEAUTY_LINK_H

#include <string>

/**
 * client                      proxy                        server
 *             send                              send
 *        -------------> clientSendBuffer ----------------->
 *             recv                              recv
 *        <------------- clientRecvBuffer <-----------------
 */

#define PACKET_BUFFER_SIZE 2048
#define EPOLL_BUFFER_SIZE 256
constexpr int MaxServerRetZeroRetryTimes = 3;

struct Upstream;

struct LbLink {
    int clientFd{-1};  // accept as client fd
    int serverFd{-1};  // upstream server fd

    size_t clientTotalBytes{0};
    int sendBufferLength{0};
    int sendBufferOffset{0};
    char clientSendBuffer[PACKET_BUFFER_SIZE];

    size_t serverTotalBytes{0};
    int recvBufferOffset{0};
    int recvBufferLength{0};
    char clientRecvBuffer[PACKET_BUFFER_SIZE];

    std::string clientEndpoint;
    Upstream* pUpstream{nullptr};

    int firstUpstreamIndex{-1};
    int currentUpstreamIndex{-1};
    int serverRetZeroRetryTimes{0};

    bool hasFirstUpstreamTriedAgain{false};
    bool clientHeaderParsed{false};
    std::string clientQueryPath;
    int clientContentLength{0};

    bool client_do_not_support_failover() {  // if bytes exceed current buffer size, means some byte cannot re-send
        return !(clientTotalBytes > 0 && clientTotalBytes < PACKET_BUFFER_SIZE);
    }

    LbLink(int clientFd_, int serverFd_, const std::string& clientEndpoint_, Upstream* pUpstream_);

    void print_leave_info(int leaver);

    bool is_client_side(int fd) { return fd == clientFd; }
    bool is_server_side(int fd) { return fd == serverFd; }
    int other_side_fd(int fd) { return fd == clientFd ? serverFd : clientFd; }
    bool is_buffer_empty(int fd) {
        if (is_client_side(fd))
            return sendBufferLength == 0;
        else
            return recvBufferLength == 0;
    }
    bool is_buffer_not_empty(int fd) { return !is_buffer_empty(fd); }

    void on_leave();

    // handle EPOLLIN event
    // > 0: success; 0: not finished; < 0: closed or other error
    int on_recv(int fd);
    int on_client_recv();
    int on_server_recv();

    // handle EPOLLOUT event
    // > 0: success; 0: not finished; < 0: closed or other error
    int on_send(int fd);
    int on_client_send();
    int on_server_send();

    void parse_client_header();

    void reset_server_side_for_failover(Upstream* newOne, int newServerFd_);
};

#endif
