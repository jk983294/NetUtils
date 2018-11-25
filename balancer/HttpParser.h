#ifndef NETUTILS_HTTP_PARSER_H
#define NETUTILS_HTTP_PARSER_H

#include <picojson.h>
#include <cstring>
#include <string>
#include <unordered_map>

struct HttpParser {
    char* msg{nullptr};
    char* endChar{nullptr};
    int len{0};
    int methodEndPos{-1};
    int headerEndPos{-1};
    bool completeBody{false};
    std::unordered_map<std::string, std::string> keyValues;

    HttpParser(char* const msg_, int len_);

    void init(char* const msg_, int len_);
    bool update_length(int newLen);
    bool has_complete_method() { return methodEndPos > 0; }
    bool has_complete_header() { return headerEndPos > 0; }
    bool has_complete_body() { return completeBody; }

    void parse_method();
    void parse_header();
    void parse_body();

    std::string get_method_line();
    std::string get_query_path();
    std::string get_header(const std::string& key);
    void print_all_headers();
};

#endif
