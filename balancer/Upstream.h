#ifndef BEAUTY_UPSTREAM_H
#define BEAUTY_UPSTREAM_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <string>

using namespace std;

struct Upstream {
    string endpoint;
    string serverHost;
    uint16_t serverPort;
    struct sockaddr_in serverAddr;
    bool good{true};
    time_t badTimestamp{0};

    Upstream(const string& endpoint_);
    bool check();
    void set_status(bool status);
};

#endif
