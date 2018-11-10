#include <pthread.h>
#include <signal.h>
#include <boost/program_options.hpp>
#include <iostream>
#include <string>
#include <vector>
#include "LbManager.h"
#include "Utils.h"

using namespace std;
namespace po = boost::program_options;

LbManager *manager{nullptr};
int signo = 0;

void *proc(void *arg) {
    auto sp = (LbManager *)arg;
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
        close(manager->pipefd[1]);

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
    unsigned int listenPort;
    string upstreams;
    po::options_description desc("Program options");
    desc.add_options()("help,h", "listen on port and direct request to upstream server")(
        "port,p", po::value<unsigned int>(&listenPort)->default_value(8081), "port to listen")(
        "upstreams,u", po::value<string>(&upstreams)->default_value("localhost:8080"),
        "upstream servers for load balance");

    po::variables_map vm;
    auto parsed = po::parse_command_line(argc, argv, desc);
    po::store(parsed, vm);
    po::notify(vm);

    if (vm.count("help")) {
        cout << desc << endl;
        return 0;
    }

    manager = new LbManager(listenPort, upstreams);
    if (manager->startup()) {
        pthread_create(&(manager->thread), nullptr, &proc, manager);  // remember to pthread_join
        cout << "manager localhost:" << listenPort << " <--> " << upstreams << endl;
    } else {
        cerr << "manager start up failed " << endl;
        return -1;
    }

    serve_forever();
    return 0;
}
