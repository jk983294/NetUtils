#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <random>

using namespace std;

int main() {
    char* msg = {
        "POST /test_empty_response HTTP/1.1\n"
        "Host: localhost:18180\n"
        "Accept: */*\n"
        "Accept-Encoding: gzip, deflate\n"
        "Connection: keep-alive\n"
        "User-Agent: python-requests/2.19.1\n"
        "Content-Length: 17\n"
        "Content-Type: application/json\n\n"
        "{\"type\": \"query\"}"};
    int len = strlen(msg);
    char* itr = strchr(msg, '\n');
    if (itr == nullptr) {
        cout << "format wrong" << endl;
    }
    string firstLine(msg, itr - msg);
    char* prevItr = itr;
    cout << firstLine << endl;
    while ((itr = strchr(itr, '\n')) != nullptr) {
        string header(prevItr, itr - prevItr);
        cout << header << endl;
        cout << "find at position " << (itr - msg) << endl;
        ++itr;
        prevItr = itr;
    }
    cout << len << endl;
    return 0;
}
