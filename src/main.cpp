#include "mini_oss/version.h"
#include "mini_oss/http_server.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>

namespace {
std::atomic_bool g_running {true};

void handleSignal(int)
{
    g_running = false;
}
}

int main(int argc, char* argv[])
{
    std::string config_path = "config.ini";
    std::uint16_t port = 8080;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            const int value = std::stoi(argv[++i]);
            if (value <= 0 || value > 65535) {
                std::cerr << "Invalid port: " << value << '\n';
                return 1;
            }
            port = static_cast<std::uint16_t>(value);
        } else if (arg == "--version") {
            std::cout << "mini_oss " << MINI_OSS_VERSION << '\n';
            return 0;
        } else if (arg == "--help") {
            std::cout << "Usage: mini_oss [--config config.ini] [--port 8080] [--version]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return 1;
        }
    }

    std::cout << "Mini-OSS starting...\n";
    std::cout << "Version: " << MINI_OSS_VERSION << '\n';
    std::cout << "Config: " << config_path << '\n';

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    mini_oss::HttpServer server(port);
    if (!server.start()) {
        return 1;
    }

    server.run([] {
        return g_running.load();
    });

    std::cout << "Mini-OSS stopped.\n";
    return 0;
}
