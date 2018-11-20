#ifndef NETUTILS_LB_CONSTANTS_H
#define NETUTILS_LB_CONSTANTS_H

#include <cstdint>

#define PACKET_BUFFER_SIZE 2048
#define EPOLL_BUFFER_SIZE 256
constexpr int MaxServerRetZeroRetryTimes = 3;
constexpr int MaxServerOnLinkRetryCount = 1;

/**
 * within this threshold, we won't retry the server which identified bad last time
 */
constexpr int64_t FirstUpstreamBadRetryTimeThreshold = 10;  // second

#endif
