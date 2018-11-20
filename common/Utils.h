#ifndef BEAUTY_UTILS_H
#define BEAUTY_UTILS_H

#include <sys/epoll.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

inline std::vector<std::string> split(const std::string &s, char delim = ' ') {
    std::vector<std::string> elems;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

inline std::string time_t2string(time_t t1) {
    struct tm tm {};
    localtime_r(&t1, &tm);
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%4u%02u%02u.%02u%02u%02u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return string(buffer);
}

inline std::string now_string() {
    time_t tNow = time(nullptr);
    return time_t2string(tNow);
}

#endif
