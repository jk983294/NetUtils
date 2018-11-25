#include <Utils.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <boost/algorithm/string.hpp>
#include <iostream>
#include <sstream>
#include "LbLink.h"
#include "Upstream.h"

using namespace std;

LbLink::LbLink(int clientFd_, const std::string& clientEndpoint_)
    : clientFd(clientFd_), clientEndpoint(clientEndpoint_) {}

void LbLink::print_leave_info(int leaver) {
    if (leaver == clientFd) {
        cout << "leave " << clientEndpoint << " " << clientTotalBytes << " -> " << pUpstream->endpoint << " "
             << serverTotalBytes << endl;
    } else {
        cout << "leave " << pUpstream->endpoint << " " << serverTotalBytes << " -> " << clientEndpoint << " "
             << clientTotalBytes << endl;
        if (serverTotalBytes == 0 && clientTotalBytes > 0 && clientTotalBytes < PACKET_BUFFER_SIZE) {
            string response{clientSendBuffer, clientTotalBytes};
            cout << "request:\n" << response << endl;
        }
    }
}

void LbLink::print_on_link_info() {
    cout << time_t2string(startTimestamp) << " open " << clientEndpoint << " <--> " << pUpstream->endpoint << endl;
}

int LbLink::on_client_recv() {
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
    clientTotalBytes += sendBufferLength;
    return ret;
}

int LbLink::on_server_recv() {
    recvBufferOffset = 0;
    recvBufferLength = 0;

    int ret = recv(serverFd, clientRecvBuffer, PACKET_BUFFER_SIZE, 0);
    if (ret < 0) {
        if (errno == EAGAIN) {
            return 0;
        } else {
            perror("server recv error");
            return -1;
        }
    } else if (ret == 0) {
        if (serverTotalBytes == 0) {
            ++serverRetZeroRetryTimes;
            if (serverRetZeroRetryTimes <= MaxServerRetZeroRetryTimes) {
                // cout << "current ret 0 times " << serverRetZeroRetryTimes << endl;
                return 0;
            }
        }
        return -1;
    }

    recvBufferLength = ret;
    serverTotalBytes += recvBufferLength;
    return ret;
}

int LbLink::on_recv(int fd) {
    if (is_client_side(fd)) {
        return on_client_recv();
    } else {
        return on_server_recv();
    }
}

int LbLink::on_send(int fd) {
    if (is_client_side(fd)) {
        return on_client_send();
    } else {
        return on_server_send();
    }
}

int LbLink::on_client_send() {
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
int LbLink::on_server_send() {
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

void LbLink::on_leave() {
    // socket
    close(clientFd);
    close(serverFd);
}

/**
 * clientTotalBytes guaranteed to > 0 since it called from data_in_event
 * @return -1 error
 *          0 msg not complete, can not make decision
 *          1 msg parsed complete
 */
int LbLink::parse_client_content() {
    if (clientHeaderParsed) return 1;
    if (clientTotalBytes > PACKET_BUFFER_SIZE) return -1;

    clientSendBuffer[clientTotalBytes] = '\0';
    HttpParser parser(clientSendBuffer, clientTotalBytes);

    parser.parse_method();
    if (parser.has_complete_method()) {
        if (std::strcmp(parser.get_query_path().c_str(), AsyncCallQueryPath) == 0) {
            isAsyncCall = true;
        }

        parser.parse_header();
        if (parser.has_complete_header()) {
            string agent = parser.get_header("User-Agent");
            if (!agent.empty() && agent.find("python") != string::npos) {
                source = LbClientSource::PythonClient;
            }

            if (isAsyncCall && source == LbClientSource::PythonClient) {
                parser.parse_body();
                if (parser.has_complete_body()) {
                    asyncHost = parser.get_header("host");
                    clientHeaderParsed = true;
                    return 1;
                } else {
                    return 0;
                }
            } else {
                clientHeaderParsed = true;
                return 1;
            }
        } else {
            return 0;  // no complete header
        }
    } else {
        return 0;  // no complete method line
    }
}

void LbLink::reset_server_side_for_failover(Upstream* newOne, int newServerFd_) {
    sendBufferOffset = 0;
    sendBufferLength = clientTotalBytes;
    pUpstream = newOne;
    serverRetZeroRetryTimes = 0;
    serverFd = newServerFd_;
}

bool LbLink::check_on_link_retry_count_exceed() {
    if (onLinkRetryServerCount >= MaxServerOnLinkRetryCount) {
        cerr << clientEndpoint << " exceed max on link retry count" << endl;
        return true;
    }
    return false;
}

bool LbLink::check_random_retry_count_exceed() {
    if (randomRetryServerCount >= MaxRandomPickCount) {
        cerr << clientEndpoint << " exceed max random retry count" << endl;
        return true;
    }
    return false;
}
