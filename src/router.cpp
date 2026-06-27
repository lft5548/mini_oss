#include "mini_oss/router.h"

#include <utility>

namespace mini_oss {

void Router::addRoute(HttpMethod method, std::string path, Handler handler)
{
    routes_.push_back(Route {method, std::move(path), std::move(handler), false});
}

void Router::addPrefixRoute(HttpMethod method, std::string prefix, Handler handler)
{
    routes_.push_back(Route {method, std::move(prefix), std::move(handler), true});
}

HttpResponse Router::route(const HttpRequest& request) const
{
    std::string path = request.path;
    const auto query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }

    bool path_exists = false;
    for (const auto& route : routes_) {
        const bool matched = route.prefix_match
            ? path.rfind(route.path, 0) == 0 && path.size() > route.path.size()
            : route.path == path;

        if (!matched) {
            continue;
        }

        path_exists = true;
        if (route.method == request.method) {
            return route.handler(request);
        }
    }

    if (path_exists) {
        return HttpResponse::methodNotAllowed();
    }
    return HttpResponse::notFound();
}

} // namespace mini_oss
