#include "mini_oss/router.h"

#include <utility>

namespace mini_oss {

void Router::addRoute(HttpMethod method, std::string path, Handler handler)
{
    routes_.push_back(Route {method, std::move(path), std::move(handler)});
}

HttpResponse Router::route(const HttpRequest& request) const
{
    bool path_exists = false;
    for (const auto& route : routes_) {
        if (route.path != request.path) {
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
