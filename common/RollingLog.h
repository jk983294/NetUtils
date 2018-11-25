#ifndef NETUTILS_ROLLINGLOG_H
#define NETUTILS_ROLLINGLOG_H

#include <fstream>
#include "Utils.h"

struct RollingLog {
    std::ofstream* ofs;
    double ts{-1};
    double rollingInterval{1.0};
    std::string logPrefix;

    RollingLog(std::string logPrefix_ = "/tmp/rolling.log.") : logPrefix(logPrefix_) {}

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
            }

            ofs = new std::ofstream(logPrefix + nowTsStr, ofstream::out | ofstream::trunc);

            if (!(*ofs)) {
                throw "open file failed";
            }
        }
        return ofs;
    }
};

#endif
