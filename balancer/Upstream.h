#ifndef BEAUTY_UPSTREAM_H
#define BEAUTY_UPSTREAM_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <string>

using namespace std;

struct Upstream {
    string endpoint;
    string serverHost;
    unsigned int serverPort;
    struct sockaddr_in serverAddr;
    bool good{true};

    Upstream(const string& endpoint_);
    bool check();
};

#endif
