#include "HttpParser.h"

using namespace std;

HttpParser::HttpParser(char* const msg_, int len_) : msg(msg_), len(len_) {
    if (len) {
        endChar = msg + len;
    }
}

void HttpParser::init(char* const msg_, int len_) {
    msg = msg_;
    len = len_;
    if (len) {
        endChar = msg + len;
    }
    methodEndPos = -1;
    headerEndPos = -1;
    completeBody = false;
    keyValues.clear();
}

bool HttpParser::update_length(int newLen) {
    if (len < newLen) {
        len = newLen;
        return true;
    }
    return false;
}

void HttpParser::parse_method() {
    if (len > 0) {
        char* itr = strchr(msg, '\n');
        if (itr != nullptr) {
            methodEndPos = static_cast<int>(itr - msg);
        }
    }
}

string HttpParser::get_method_line() {
    if (methodEndPos >= 0) {
        return string(msg, methodEndPos);
    }
    return "";
}

string HttpParser::get_query_path() {
    if (methodEndPos > 0) {
        char* itr1 = strchr(msg, ' ');
        if (itr1 == nullptr || (itr1 - msg) > methodEndPos) return "";
        char* itr2 = strchr(itr1 + 1, ' ');
        if (itr2 == nullptr || (itr2 - msg) > methodEndPos) return "";
        return string(itr1 + 1, itr2 - itr1 - 1);
    }
    return "";
}

void HttpParser::parse_header() {
    if (methodEndPos >= 0 and methodEndPos + 1 < len) {
        char* itr = msg + methodEndPos + 1;
        char* prevItr = itr;
        while (itr < endChar) {
            itr = strchr(itr, '\n');
            if (itr == nullptr) break;
            int pos = static_cast<int>(itr - msg);

            string header(prevItr, (itr - prevItr) - 1);
            size_t posColon = header.find_first_of(':', 0);
            if (posColon != string::npos) {
                if (posColon + 2 < header.size()) keyValues[header.substr(0, posColon)] = header.substr(posColon + 2);
            }
            ++itr;
            prevItr = itr;
            if ((itr + 1) < endChar && *itr == '\r' && *(itr + 1) == '\n') {
                headerEndPos = pos + 2;
                break;
            }
        }
    }
}

string HttpParser::get_header(const string& key) {
    auto itr = keyValues.find(key);
    if (itr != keyValues.end()) {
        return itr->second;
    }
    return "";
}

void HttpParser::print_all_headers() {
    for (auto itr : keyValues) {
        cout << "__" << itr.first << "__ __" << itr.second << "__" << endl;
    }
}

void HttpParser::parse_body() {
    if (headerEndPos > 0) {
        char* itr = msg + headerEndPos + 1;
        if (itr < endChar) {
            picojson::value v;
            std::string err;
            picojson::parse(v, itr, endChar, &err);
            if (!err.empty()) {
                return;
            }

            completeBody = true;
            if (v.is<picojson::object>()) {
                // obtain a const reference to the map, and print the contents
                const picojson::value::object& obj = v.get<picojson::object>();
                for (auto i = obj.begin(); i != obj.end(); ++i) {
                    keyValues[i->first] = i->second.to_str();
                }
            }
        }
    }
}
