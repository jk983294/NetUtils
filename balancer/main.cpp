#include <pthread.h>
#include <signal.h>
#include <boost/program_options.hpp>
#include <iostream>
#include <string>
#include <vector>
#include "LbManager.h"
#include "RollingLog.h"
#include "Utils.h"

using namespace std;
namespace po = boost::program_options;

ILbManager *manager{nullptr};
int signo = 0;
LbPolicy policy{LbPolicy::IP_HASHED};

void *proc(void *arg) {
    auto sp = (ILbManager *)arg;
    sp->serve();
    sp->shutdown();
    return nullptr;
}

void on_signal(int sig) {
    signo = sig;
    cout << "recv signal:" << signo << " going to shutdown system gracefully" << endl;
}

void clear(bool force) {
    if (manager) {
        if (force) {
            pthread_cancel(manager->thread);
            pthread_join(manager->thread, nullptr);
            cout << "shutdown manager thread" << endl;
            delete manager;
            manager = nullptr;
        } else {
            int ret = pthread_kill(manager->thread, 0);  // check if thread died
            if (ret != 0) {                              // thread already died
                pthread_join(manager->thread, nullptr);
                cout << "manager thread already gone\n";
                delete manager;
                manager = nullptr;
            }
        }
    }
}

void serve_forever() {
    signo = 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    usleep(1000 * 1000);  // wait for manager thread up
    while (true) {
        if (signo != 0) {
            cout << "exit by signo " << signo << endl;
            break;
        }
        clear(false);
        usleep(500);
    }

    if (manager) {
        // 1 sec to clean up everything
        // signal pipe to manager, manager get signal will gracefully shutdown
        close(manager->pipeFd[1]);

        // clean closed thread
        int count = 8;
        while (count--) {
            clear(false);
            usleep(125);
        }
        // if there still some
        clear(true);
    }
}

int main(int argc, char *argv[]) {
    uint16_t listenPort;
    string upstreams;
    string method;
    string logPrefix;
    po::options_description desc("Program options");
    desc.add_options()
    ("help,h", "listen on port and direct request to upstream server")
    ("port,p", po::value<uint16_t>(&listenPort)->default_value(8081), "port to listen")
    ("upstreams,u", po::value<string>(&upstreams)->default_value("localhost:8080"), "upstream servers for load balance")
    ("method,m", po::value<string>(&method)->default_value("ip_hashed"), "method to load balance (ip_hashed|random)")
    ("log,l", po::value<string>(&logPrefix)->default_value("/tmp/rolling.log."), "create log file with this prefix");

    po::variables_map vm;
    auto parsed = po::parse_command_line(argc, argv, desc);
    po::store(parsed, vm);
    po::notify(vm);

    if (vm.count("help")) {
        cout << desc << endl;
        return 0;
    }

    RollingLog logger(logPrefix);

    if (method == "random") {
        policy = LbPolicy::RANDOMED;
        *logger.ofs << "lb policy use random method" << endl;
    } else {
        policy = LbPolicy::IP_HASHED;
        *logger.ofs << "lb policy use ip hashed method" << endl;
    }

    if (policy == LbPolicy::IP_HASHED)
        manager = new LbManager<LbPolicy::IP_HASHED>(listenPort, upstreams, logger);
    else
        manager = new LbManager<LbPolicy::RANDOMED>(listenPort, upstreams, logger);

    if (manager->startup()) {
        pthread_create(&(manager->thread), nullptr, &proc, manager);  // remember to pthread_join
        *logger.ofs << "manager localhost:" << listenPort << " <--> " << upstreams << endl;
    } else {
        cerr << "manager start up failed " << endl;
        return -1;
    }

    serve_forever();
    return 0;
}
