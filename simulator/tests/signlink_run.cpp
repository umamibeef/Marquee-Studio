// Run a boot-flash image from RAM on the sign, through the Studio's sign link (src/studio/signlink.*).
// Usage: sign-link-run PORT IMAGE [TIMEOUT_SCALE]   (power the sign on while it waits)
#include "signlink.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: sign-link-run PORT IMAGE [TIMEOUT_SCALE]\n");
        return 2;
    }
    std::ifstream f(argv[2], std::ios::binary);
    host::Bytes8 image{std::istreambuf_iterator<char>(f), {}};
    try {
        host::SerialPort port(argv[1]);
        host::SignWindow w(port);
        if (argc > 3)
            w.timeout_scale = atof(argv[3]);
        printf("waiting for the sign's window...\n");
        fflush(stdout);
        printf("%s\n", w.wait_window(600 * w.timeout_scale).c_str());
        std::string info = w.hello();
        printf("%s", info.c_str());
        uint32_t last = 0;
        uint32_t n = w.stage(image, [&](uint32_t done, uint32_t total) {
            if (done - last >= 65536 || done == total) {
                printf("  staged %u / %u\n", done, total);
                fflush(stdout);
                last = done;
            }
        });
        w.run(image, n);
        printf("RUN sent: the sign boots %s from RAM\n", argv[2]);
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
