#ifndef BEAUTY_LINK_H
#define BEAUTY_LINK_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <string>

using namespace std;

#define PACKET_BUFFER_SIZE 2048
#define EPOLL_BUFFER_SIZE 256

/**
 * client                      proxy                        server
 *             send                              send
 *        -------------> clientSendBuffer ----------------->
 *             recv                              recv
 *        <------------- clientRecvBuffer <-----------------
 */

struct Link {
    int clientFd;  // accept as client fd
    int serverFd;  // upstream server fd

    int sendBufferLength{0};
    int sendBufferOffset{0};
    char clientSendBuffer[PACKET_BUFFER_SIZE];

    int recvBufferOffset{0};
    int recvBufferLength{0};
    char clientRecvBuffer[PACKET_BUFFER_SIZE];

    std::string clientEndpoint;
    const string& serverEndpoint;

    Link(int clientFd_, int serverFd_, const string& clientEndpoint_, const string& serverEndpoint_);

    void print_leave_info(int leaver);

    bool isClientSide(int fd) { return fd == clientFd; }
    bool is_buffer_empty(int fd) {
        if (isClientSide(fd))
            return sendBufferLength == 0;
        else
            return recvBufferLength == 0;
    }
    bool is_buffer_not_empty(int fd) { return !is_buffer_empty(fd); }

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
};

inline Link::Link(int clientFd_, int serverFd_, const string& clientEndpoint_, const string& serverEndpoint_)
    : clientFd(clientFd_), serverFd(serverFd_), clientEndpoint(clientEndpoint_), serverEndpoint(serverEndpoint_) {}

inline void Link::print_leave_info(int leaver) {
    if (leaver == clientFd) {
        cout << "leave " << clientEndpoint << " -> " << serverEndpoint << endl;
    } else {
        cout << "leave " << serverEndpoint << " -> " << clientEndpoint << endl;
    }
}

inline int Link::on_client_recv() {
    sendBufferOffset = 0;
    sendBufferLength = 0;

    int ret = recv(clientFd, clientSendBuffer, PACKET_BUFFER_SIZE, 0);
    if (ret < 0) {
        if (errno == EAGAIN) {
            return 0;
        } else {
            return -1;
        }
    } else if (ret == 0) {
        return -1;
    }

    sendBufferLength = ret;
    return ret;
}

inline int Link::on_server_recv() {
    recvBufferOffset = 0;
    recvBufferLength = 0;

    int ret = recv(serverFd, clientRecvBuffer, PACKET_BUFFER_SIZE, 0);
    if (ret < 0) {
        if (errno == EAGAIN) {
            return 0;
        } else {
            return -1;
        }
    } else if (ret == 0) {
        return -1;
    }

    recvBufferLength = ret;
    return ret;
}

inline int Link::on_recv(int fd) {
    if (isClientSide(fd)) {
        return on_client_recv();
    } else {
        return on_server_recv();
    }
}

inline int Link::on_send(int fd) {
    if (isClientSide(fd)) {
        return on_client_send();
    } else {
        return on_server_send();
    }
}

inline int Link::on_client_send() {
    int totalSent = 0;
    while (recvBufferLength > 0) {
        int ret = send(clientFd, clientRecvBuffer + recvBufferOffset, recvBufferLength, 0);
        if (ret < 0) {
            if (errno == EAGAIN) {
                return 0;
            } else {
                return -1;
            }
        } else {
            recvBufferLength -= ret;
            recvBufferOffset += ret;
            totalSent += ret;
        }
    }
    return totalSent;
}
inline int Link::on_server_send() {
    int totalSent = 0;
    while (sendBufferLength > 0) {
        int ret = send(serverFd, clientSendBuffer + sendBufferOffset, sendBufferLength, 0);
        if (ret < 0) {
            if (errno == EAGAIN) {
                return 0;
            } else {
                return -1;
            }
        } else {
            sendBufferLength -= ret;
            sendBufferOffset += ret;
            totalSent += ret;
        }
    }
    return totalSent;
}

#endif
