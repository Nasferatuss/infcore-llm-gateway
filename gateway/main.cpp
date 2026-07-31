// infcore gateway — лицензия MIT (см. LICENSE).
// Точка входа: загрузка конфига → запуск OpenAI-совместимого gateway (control-plane
// перед бэкендами llama-server). Никаких исходящих соединений за пределы контура.
#include <cstdio>
#include <cstring>
#include <exception>

#include "config.hpp"
#include "server.hpp"

namespace {

constexpr const char* kDefaultConfig = "infcore/config/gateway.yaml";
constexpr const char* kVersion = "infcore gateway 0.1.0";

void usage() {
    std::printf(
        "%s\n\n"
        "usage: infcore_gateway [--config PATH | PATH]\n\n"
        "  --config PATH   gateway config (default: %s)\n"
        "  -h, --help      this message\n"
        "  -V, --version   version only\n\n"
        "The config is validated against config/schema/gateway.schema.json at\n"
        "startup; an invalid config fails fast rather than half-starting.\n",
        kVersion, kDefaultConfig);
}

}  // namespace

int main(int argc, char** argv) {
    const char* cfg_path = kDefaultConfig;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            usage();
            return 0;
        }
        if (!std::strcmp(a, "-V") || !std::strcmp(a, "--version")) {
            std::printf("%s\n", kVersion);
            return 0;
        }
        if (!std::strcmp(a, "--config")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "infcore: --config needs a path\n");
                return 2;
            }
            cfg_path = argv[++i];
            continue;
        }
        if (a[0] == '-') {
            std::fprintf(stderr, "infcore: unknown option: %s\n", a);
            usage();
            return 2;
        }
        cfg_path = a;   // positional form, as used by the systemd unit and Dockerfile
    }

    std::printf("%s\n", kVersion);

    try {
        infcore::GatewayConfig cfg = infcore::load_config(cfg_path);
        infcore::GatewayServer server(std::move(cfg));
        return server.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "infcore: fatal error: %s\n", e.what());
        return 1;
    }
}
