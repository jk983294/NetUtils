#include <iostream>
#include "Upstream.h"
#include "Utils.h"

using namespace std;

Upstream::Upstream(const string& endpoint_) : endpoint(endpoint_) {}

bool Upstream::check() {
    vector<string> result = split(endpoint, ':');
    if (result.size() == 2) {
        serverHost = result[0];
        if (serverHost == "localhost") {
            serverHost = "0.0.0.0";
        }
        serverPort = static_cast<unsigned int>(std::atoi(result[1].c_str()));

        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);

        if (inet_pton(AF_INET, serverHost.c_str(), &serverAddr.sin_addr) <= 0) {
            perror("inet_pton convert address from text to binary form failed");
            good = false;
        }
    } else {
        cerr << "unknown upstream " << endpoint << endl;
        good = false;
    }
    return good;
}
