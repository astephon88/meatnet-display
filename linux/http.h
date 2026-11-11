// A small HTTP/1.0 server for the web page and its JSON API: one request
// per connection, handled in turn on the server's own thread.
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace linux_unit {

struct Request {
    std::string method, path, query, body;
};

struct Response {
    int status = 200;
    std::string type = "application/json", body;
};

class HttpServer {
public:
    using Handler = std::function<Response(const Request &)>;
    HttpServer(int port, Handler h) : port_(port), handler_(std::move(h)) {}
    bool listen();                      // false if the port can't be bound
    void run();                         // blocks until stop()
    void stop() { stop_ = true; }

private:
    void serve(int fd);
    int port_, fd_ = -1;
    Handler handler_;
    std::atomic<bool> stop_{false};
};

std::string query_value(const std::string &query, const std::string &key);

}  // namespace linux_unit
