#pragma once

#include "mini_oss/http.h"

#include <functional>
#include <string>
#include <vector>

namespace mini_oss {

class Router {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    void addRoute(HttpMethod method, std::string path, Handler handler);
    void addPrefixRoute(HttpMethod method, std::string prefix, Handler handler);
    HttpResponse route(const HttpRequest& request) const;

private:
    struct Route {
        HttpMethod method = HttpMethod::Unknown;
        std::string path;
        Handler handler;
        bool prefix_match = false;
    };

    std::vector<Route> routes_;
};

} // namespace mini_oss
