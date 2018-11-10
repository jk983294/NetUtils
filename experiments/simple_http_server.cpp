#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

using namespace std;

#define BUF_LEN 1028
#define SERVER_PORT 8080

const static char http_error_hdr[] = "HTTP/1.1 404 Not Found\r\nContent-type: text/html\r\n\r\n";
const static char http_html_hdr[] = "HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n";
const static char http_index_html[] =
    "<html><head><title>Congrats!</title></head>"
    "<body><h1>Welcome to our HTTP server demo!</h1>"
    "<p>This is a just small test page.</body></html>";

int http_send_file(char *filename, int sockfd) {
    if (!strcmp(filename, "/")) {
        write(sockfd, http_html_hdr, strlen(http_html_hdr));
        write(sockfd, http_index_html, strlen(http_index_html));
    } else {
        cerr << filename << " file not find!" << endl;
        write(sockfd, http_error_hdr, strlen(http_error_hdr));
    }
    return 0;
}

void serve(int sockfd) {
    char buf[BUF_LEN];
    int ret = read(sockfd, buf, BUF_LEN);
    if (ret >= 0) {
        cout << ret << " bytes read\n" << buf << endl;
        cout << "**************************" << endl;
        if (!strncmp(buf, "GET", 3)) {
            char *file = buf + 4;
            char *space = strchr(file, ' ');
            *space = '\0';
            http_send_file(file, sockfd);
        } else {
            cerr << "unsupported request!" << endl;
            return;
        }
    } else {
        cout << "read error " << ret << endl;
    }
}

int main() {
    int sockfd, newfd;
    struct sockaddr_in addr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed!\n");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in))) {
        perror("socket binding failed!\n");
        return -1;
    }

    listen(sockfd, 128);
    for (;;) {
        newfd = accept(sockfd, NULL, NULL);
        serve(newfd);
        close(newfd);
    }
}
