#ifndef NETUTILS_ROLLINGLOG_H
#define NETUTILS_ROLLINGLOG_H

#include <fstream>
#include <iostream>
#include "Utils.h"

struct RollingLog {
    std::ofstream* ofs{nullptr};
    double ts{-1};
    double rollingInterval{1.0};
    std::string logPrefix;

    RollingLog(const std::string& logPrefix_ = "/tmp/rolling.log.") : logPrefix(logPrefix_) { update(); }

    void init(const std::string& logPrefix_) {
        logPrefix = logPrefix_;
        if (ofs) {
            delete ofs;
            ofs = nullptr;
        }
        update();
    }

    ~RollingLog() {
        if (ofs) {
            ofs->flush();
            delete ofs;
        }
    }

    std::ofstream* update() {
        std::string nowTsStr = now_string();
        double nowTs = std::atof(nowTsStr.c_str());

        if (ts < 0 || (nowTs - ts) > rollingInterval) {
            if (ofs) {
                ofs->flush();
                delete ofs;
                ofs = nullptr;
            }

            string logName = logPrefix + nowTsStr;
            ofs = new std::ofstream(logPrefix + nowTsStr, ofstream::out | ofstream::trunc);

            if (!(*ofs)) {
                throw "open file failed";
            }
            *ofs << "open log " << logName << endl;
            std::cout << "open log " << logName << endl;
            ts = nowTs;
        }
        return ofs;
    }
};

#endif
