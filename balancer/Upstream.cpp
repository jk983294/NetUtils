#include <iostream>
#include "RawSocket.h"
#include "Upstream.h"
#include "Utils.h"

using namespace std;

Upstream::Upstream(const string& endpoint_) : endpoint(endpoint_) {}

bool Upstream::check() {
    vector<string> result = split(endpoint, ':');
    if (result.size() == 2) {
        serverHost = result[0];
        aliasedEndpoint = serverHost;
        if (serverHost == "localhost") {
            serverHost = "0.0.0.0";
        }
        if (serverHost == "0.0.0.0") {
            aliasedEndpoint = get_ipv4_address();
        }
        // cout << "alias endpoint " << aliasedEndpoint << endl;
        aliasedEndpoint += result[1];
        serverPort = static_cast<uint16_t>(std::atoi(result[1].c_str()));

        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);

        if (inet_pton(AF_INET, serverHost.c_str(), &serverAddr.sin_addr) <= 0) {
            perror("inet_pton convert address from text to binary form failed");
            set_status(false);
        }
    } else {
        cerr << "unknown upstream " << endpoint << endl;
        set_status(false);
    }
    return good;
}

void Upstream::set_status(bool status) {
    if (status) {
        if (!good) good = true;
    } else {
        if (good) {
            good = false;
        }
        badTimestamp = time(nullptr);  // update bad time every time
    }
}

bool Upstream::is_host_match(const string& host_) { return endpoint == host_ || aliasedEndpoint == host_; }
