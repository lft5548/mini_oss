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
    bool path_exists = false;
    for (const auto& route : routes_) {
        bool matched = route.path == request.path;
        if (!matched && route.prefix_match) {
            matched = request.path.rfind(route.path, 0) == 0 && request.path.size() > route.path.size();
        }

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
