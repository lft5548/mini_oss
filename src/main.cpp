#include "mini_oss/version.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    std::string config_path = "config.ini";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--version") {
            std::cout << "mini_oss " << MINI_OSS_VERSION << '\n';
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return 1;
        }
    }

    std::cout << "Mini-OSS starting...\n";
    std::cout << "Version: " << MINI_OSS_VERSION << '\n';
    std::cout << "Config: " << config_path << '\n';
    std::cout << "Next step: implement the HTTP server MVP.\n";
    return 0;
}
