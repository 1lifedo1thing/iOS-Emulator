#include "lighttouch.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
    char temporary[] = "/tmp/lighttouch-file-hash-XXXXXX";
    char *directory = mkdtemp(temporary);
    assert(directory);
    const fs::path root(directory), path = root / "input";
    struct Cleanup {
        fs::path root;
        ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};
    struct Vector {
        size_t bytes;
        const char *digest;
    };
    // Golden SHA256 values computed independently using Python hashlib.
    // Cover SHA padding and the production file reader's 64KiB chunk edges.
    const Vector vectors[] = {
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {1, "e7cf46a078fed4fafd0b5e3aff144802b853f8ae459a4f0c14add3314b7cc3a6"},
        {55, "2900465fcb533e05a158fd2b3be0e5e3b03740d83060aa3580e0d98a96bf2384"},
        {56, "31454ff48ef36af2f08fd511bdc37d9d5855ac23e992e5ff5445cb6b7674a674"},
        {63, "5f6401b96532c36de4e65beec0409b69b1d181864c8009b7a04f43e5d56350d1"},
        {64, "94eb5de4943613fd048dc93393ab06877405faa39c11f53e9386083339833e7e"},
        {65, "fc518669b6eb4b4dd91827ecacef86689c725bd5bab888fd3b26dbb196eec954"},
        {65535, "11cad04d25d7e2ae2bde1131397c1ee623dcf0731e3e123f278fa192a9d72976"},
        {65536, "6fc179cfd193754e6109ad043f56d146c7e7d7c3623ffceae318266286f58388"},
        {65537, "3123afca912e156e19bcd2055eb92e944cf6e6fdb0eef16d65464342ad1f5a1a"},
        {1048593, "f29a3c3c1b0a783a1e51a92b74707d00ff6fbedc83ccc862a5c65b61c2f06064"},
    };
    lt_error error{sizeof(error), 1, 0, {}};
    char digest[65]{};
    for (const auto &vector : vectors) {
        std::string input(vector.bytes, '\0');
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = char((i * 37 + 11) & 255);
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(input.data(), input.size());
            assert(file.good());
        }
        assert(lt_sha256_file(path.c_str(), digest, &error) == LT_OK);
        assert(digest[64] == '\0' && std::strcmp(digest, vector.digest) == 0);
    }
    fs::create_symlink(path, root / "link");
    assert(lt_sha256_file((root / "link").c_str(), digest, &error) == LT_IO);
    assert(lt_sha256_file((root / "missing").c_str(), digest, &error) == LT_IO);
    assert(lt_sha256_file(nullptr, digest, &error) == LT_INVALID);
    std::cout << "C ABI file SHA256 golden padding/chunk vectors and path failures PASS\n";
}
