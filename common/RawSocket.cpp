#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <strings.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include "RawSocket.h"

static int wait_for_connect(int sock, int timeout) {
    int rc = -1;
    struct pollfd pollFds[1];
    pollFds[0].fd = sock;
    pollFds[0].events = POLLOUT;
    pollFds[0].revents = 0;

    if (poll(pollFds, 1, timeout * 1000) == 1) {
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0) {
            rc = 0;
        }
    }
    return rc;
}

int set_nonblock(int fdSocket) {
    int rc = 0;
    int flags = fcntl(fdSocket, F_GETFL, 0);
    if (flags < 0) {
        rc = -1;
    } else {
        if (fcntl(fdSocket, F_SETFL, (flags | O_NONBLOCK)) < 0) {
            rc = -1;
        }
    }
    return rc;
}

static int disable_nagle(int fdSocket) {
    int flag = 1;
    return setsockopt(fdSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
}

static int set_sndbuf(int fdSocket, int bufSize) {
    return setsockopt(fdSocket, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));
}

static int set_rcvbuf(int fdSocket, int bufSize) {
    return setsockopt(fdSocket, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
}

static int shutdown(int fdSocket) { return close(fdSocket); }

int make_tcp_socket_server(char const *addrListen, uint16_t portListen) {
    int descriptor = -1;
    struct in_addr serverInAddr;
    if (inet_aton(addrListen, &serverInAddr) > 0) {
        struct sockaddr_in saddr;
        bzero((char *)&saddr, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(portListen);
        saddr.sin_addr = serverInAddr;

        descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (-1 == descriptor) {
            perror("socket: ");
            return -1;
        }

        int optval = 1;
        if (-1 == setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
            perror("setsockopt: ");
            close(descriptor);
            return -1;
        }

        if (-1 == bind(descriptor, (struct sockaddr *)&saddr, sizeof(struct sockaddr))) {
            perror("bind: ");
            close(descriptor);
            return -1;
        }

        if (-1 == listen(descriptor, 5)) {
            perror("listen: ");
            close(descriptor);
            return -1;
        }
        set_nonblock(descriptor);
    }
    return descriptor;
}

int make_tcp_socket_client(char const *addrLocal, uint16_t portLocal, char const *addrRemote, uint16_t portRemote,
                           int timeout) {
    int descriptor;
    int rc = -1;
    if ((descriptor = socket(AF_INET, SOCK_STREAM, 0)) >= 0) {
        struct sockaddr_in laddr, raddr;
        bzero((char *)&laddr, sizeof(laddr));
        laddr.sin_family = AF_INET;
        raddr = laddr;
        raddr.sin_family = AF_INET;
        laddr.sin_port = htons(portLocal);
        raddr.sin_port = htons(portRemote);
        struct in_addr in_laddr, in_raddr;
        if (inet_aton(addrLocal, &in_laddr) > 0 && inet_aton(addrRemote, &in_raddr) > 0) {
            laddr.sin_addr = in_laddr;
            raddr.sin_addr = in_raddr;
            if ((rc = bind(descriptor, (struct sockaddr *)&laddr, sizeof(struct sockaddr))) == 0) {
                set_sndbuf(descriptor, 16 * 1024 * 1024);
                set_rcvbuf(descriptor, 16 * 1024 * 1024);
                disable_nagle(descriptor);
                set_nonblock(descriptor);
                /* Start the connect, note that this is a non-blocking socket */
                if ((rc = connect(descriptor, (struct sockaddr *)&raddr, sizeof(struct sockaddr))) == -1) {
                    if (EINPROGRESS == errno) {
                        /* Wait for connect to complete within timeout seconds */
                        rc = wait_for_connect(descriptor, timeout);
                    }
                }
            }
        }
    }

    if (-1 == rc && descriptor >= 0) {
        close(descriptor);
        descriptor = -1;
    }
    return descriptor;
}

int make_udp_socket(char const *addrLocal, uint16_t portLocal) {
    int descriptor;
    if ((descriptor = socket(PF_INET, SOCK_DGRAM, 0)) >= 0) {
        struct sockaddr_in laddr;
        bzero((char *)&laddr, sizeof(laddr));
        laddr.sin_family = AF_INET;
        laddr.sin_port = htons(portLocal);
        struct in_addr in_laddr;
        if (inet_aton(addrLocal, &in_laddr) > 0) {
            laddr.sin_addr = in_laddr;
            if (bind(descriptor, (struct sockaddr *)&laddr, sizeof(struct sockaddr)) < 0) {
                close(descriptor);
                return -1;
            } else {
                set_sndbuf(descriptor, 16 * 1024 * 1024);
                set_rcvbuf(descriptor, 16 * 1024 * 1024);
                set_nonblock(descriptor);
            }
        }
    }
    return descriptor;
}

int accept_tcp_socket_client(int fdServerSocket, uint16_t *portClient, char **ipClient, bool setSocketNonblock) {
    struct sockaddr_in addrClient;
    bzero((char *)&addrClient, sizeof(addrClient));
    socklen_t socklenClient = sizeof(addrClient);

    int fdClient = accept(fdServerSocket, (struct sockaddr *)&addrClient, &socklenClient);
    if (fdClient >= 0) {
        *portClient = ntohs(addrClient.sin_port);
        *ipClient = (char *)malloc(32);
        inet_ntop(AF_INET, &(addrClient.sin_addr), *ipClient, 32);
        set_sndbuf(fdClient, 16 * 1024 * 1024);
        set_rcvbuf(fdClient, 16 * 1024 * 1024);
        if (setSocketNonblock) {
            set_nonblock(fdClient);
        }
    }
    return fdClient;
}

int close_socket(int fdSocket) { return shutdown(fdSocket); }

/**
 * writes numBytes from character array buf to socket
 */
ssize_t write_socket(int fdSocket, uint8_t const *buf, std::size_t numBytes) {
    std::size_t sentCount = 0;
    std::size_t unsentCount = numBytes;
    while (unsentCount > 0) {
        ssize_t nsnt = send(fdSocket, buf + sentCount, unsentCount, MSG_NOSIGNAL);
        if (nsnt > 0) {
            sentCount += nsnt;
            unsentCount -= nsnt;
        } else if (nsnt < 0) {
            if ((EINTR != errno) && (EAGAIN != errno)) {
                return -1;
            }
        }
    }
    return sentCount;
}

/**
 * reads numBytes into character array buf from socket fdSocket.
 * this call may return after reading less than numBytes if argument readExactAmount is set to false.
 */
ssize_t read_socket(int fdSocket, uint8_t *buf, std::size_t numBytes, bool readExactAmount) {
    std::size_t readCount = 0;
    std::size_t unreadCount = numBytes;
    while (unreadCount > 0) {
        ssize_t nrcv = recv(fdSocket, buf + readCount, unreadCount, MSG_WAITALL);
        if (nrcv > 0) {
            readCount += nrcv;
            unreadCount -= nrcv;
        } else if (nrcv < 0) {
            if ((EINTR != errno) && (EAGAIN != errno)) {  // continue if interrupted or there is nothing to read
                return -1;                                // bail out
            }
            if (!readExactAmount && readCount) {
                return readCount;
            }
        } else /* if (nrcv == 0) */
        {
            return 0;  // orderly shutdown from the remote side
        }
    }
    return readCount;
}

ssize_t send_udp(int fdSocket, uint8_t const *buf, std::size_t numBytes, char const *addrRemote, uint16_t portRemote) {
    struct sockaddr_in raddr;
    bzero((char *)&raddr, sizeof(raddr));
    raddr.sin_family = AF_INET;
    raddr.sin_port = htons(portRemote);
    struct in_addr in_raddr;
    if (inet_aton(addrRemote, &in_raddr) > 0) {
        raddr.sin_addr = in_raddr;
        std::size_t nsent = 0;
        std::size_t nunsent = numBytes;
        while (nunsent > 0) {
            ssize_t nsnt = sendto(fdSocket, buf + nsent, nunsent, 0, (struct sockaddr *)&raddr, sizeof(raddr));
            if (nsnt > 0) {
                nsent += nsnt;
                nunsent -= nsnt;
            } else if (nsnt < 0) {
                if ((EINTR != errno) && (EAGAIN != errno)) {
                    return -1;
                }
            }
        }
        return nsent;
    }
    return -1;
}

ssize_t read_udp(int fdSocket, uint8_t *buf, std::size_t numBytes) { return recv(fdSocket, buf, numBytes, 0); }
