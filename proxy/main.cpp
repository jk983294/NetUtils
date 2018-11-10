#include <boost/program_options.hpp>
#include <iostream>
#include <string>
#include "SocketProxy.h"
#include "Utils.h"

using namespace std;
namespace po = boost::program_options;

SocketProxy *proxy{nullptr};
int signo = 0;

void *proc(void *arg) {
    auto sp = (SocketProxy *)arg;
    sp->serve();
    sp->shutdown();
    return nullptr;
}

void on_signal(int sig) {
    signo = sig;
    cout << "recv signal:" << signo << " going to shutdown system gracefully" << endl;
}

void clear(bool force) {
    if (proxy) {
        if (force) {
            pthread_cancel(proxy->thread);
            pthread_join(proxy->thread, nullptr);
            cout << "shutdown proxy thread" << endl;
            delete proxy;
            proxy = nullptr;
        } else {
            int ret = pthread_kill(proxy->thread, 0);  // check if thread died
            if (ret != 0) {                            // thread already died
                pthread_join(proxy->thread, nullptr);
                cout << "proxy thread already gone\n";
                delete proxy;
                proxy = nullptr;
            }
        }
    }
}

void serve_forever() {
    signo = 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    usleep(1000 * 1000);  // wait for proxy thread up
    while (true) {
        if (signo != 0) {
            cout << "exit by signo " << signo << endl;
            break;
        }
        clear(false);
        usleep(500);
    }

    if (proxy) {
        // 1 sec to clean up everything
        // signal pipe to proxy, proxy get signal will gracefully shutdown
        close(proxy->pipefd[1]);

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
    string upstream;
    po::options_description desc("Program options");
    desc.add_options()("help,h", "listen on port and direct request to upstream server")(
        "port,p", po::value<unsigned int>(&listenPort)->default_value(8081), "port to listen")(
        "upstream,u", po::value<string>(&upstream)->default_value("localhost:8080"), "upstream server for proxy");

    po::variables_map vm;
    auto parsed = po::parse_command_line(argc, argv, desc);
    po::store(parsed, vm);
    po::notify(vm);

    if (vm.count("help")) {
        cout << desc << endl;
        return 0;
    }

    vector<string> result = split(upstream, ':');
    if (result.size() == 2) {
        string upstreamHost = result[0];
        unsigned int upstreamPort = static_cast<unsigned int>(std::atoi(result[1].c_str()));
        proxy = new SocketProxy(listenPort, upstreamHost, upstreamPort);
        if (proxy->startup()) {
            pthread_create(&(proxy->thread), nullptr, &proc, proxy);  // remember to pthread_join
            cout << "proxy localhost:" << listenPort << " <--> " << upstreamHost << ":" << upstreamPort << endl;
            serve_forever();
        } else {
            cerr << "proxy start up failed " << endl;
        }
    } else {
        cerr << "unknown upstream " << upstream << endl;
    }
    return 0;
}
