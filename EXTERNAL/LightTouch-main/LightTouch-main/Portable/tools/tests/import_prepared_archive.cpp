#include "lighttouch.h"
#include <iostream>

// A tiny command adapter to the production decoder/integrity/publish path.
// No duplicate archive parser or test-local storage implementation.
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: import-prepared-archive SOURCE ABSOLUTE_DESTINATION SHA256\n";
        return 2;
    }
    lt_import_config config{sizeof(config), LT_ABI_VERSION, argv[1], argv[2], argv[3], 64ull << 30, nullptr,
                            nullptr};
    lt_error error{sizeof(error), LT_ABI_VERSION, 0, {}};
    auto status = lt_import_archive(&config, &error);
    std::cout << int(status) << " " << error.message << "\n";
    return status == LT_OK ? 0 : 1;
}
