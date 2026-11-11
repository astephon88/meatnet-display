#include "http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace linux_unit {

static constexpr size_t kMaxBody = 64 * 1024;

bool HttpServer::listen()
{
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);    // "Reboot" re-execs us
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port_));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || ::listen(fd_, 8) != 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

void HttpServer::run()
{
    while (!stop_ && fd_ >= 0) {
        pollfd p = {fd_, POLLIN, 0};
        if (poll(&p, 1, 500) <= 0) continue;
        int c = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) continue;
        timeval tv = {5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        serve(c);
        close(c);
    }
}

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    default: return "Error";
    }
}

void HttpServer::serve(int fd)
{
    std::string in;
    char buf[4096];
    size_t head_end;
    while ((head_end = in.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0 || in.size() > 16384) return;
        in.append(buf, static_cast<size_t>(n));
    }
    Request req;
    std::string head = in.substr(0, head_end);
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    req.method = head.substr(0, sp1);
    std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?');
    req.path = target.substr(0, q);
    if (q != std::string::npos) req.query = target.substr(q + 1);
    size_t len = 0;
    for (size_t pos = head.find("\r\n"); pos != std::string::npos; pos = head.find("\r\n", pos + 2)) {
        if (strncasecmp(head.c_str() + pos + 2, "content-length:", 15) == 0)
            len = strtoul(head.c_str() + pos + 17, nullptr, 10);
    }
    if (len > kMaxBody) return;
    req.body = in.substr(head_end + 4);
    while (req.body.size() < len) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        req.body.append(buf, static_cast<size_t>(n));
    }
    Response res = handler_(req);
    std::string out = "HTTP/1.0 " + std::to_string(res.status) + " " + reason(res.status) +
                      "\r\nContent-Type: " + res.type + "\r\nContent-Length: " + std::to_string(res.body.size()) +
                      "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + res.body;
    for (size_t off = 0; off < out.size();) {
        ssize_t n = send(fd, out.data() + off, out.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

std::string query_value(const std::string &query, const std::string &key)
{
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? std::string() : kv.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

}  // namespace linux_unit
