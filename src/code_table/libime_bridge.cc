#define _POSIX_C_SOURCE 200809L

#include "libime_bridge.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <sstream>
#include <string>

#include <libime/table/tablebaseddictionary.h>

extern "C" int waytinyime_libime_decode_table(
    const char *path,
    char **text,
    size_t *size,
    char **error_message
) {
    if (path == nullptr || text == nullptr || size == nullptr) {
        return -1;
    }
    *text = nullptr;
    *size = 0;
    if (error_message != nullptr) {
        *error_message = nullptr;
    }

    try {
        libime::TableBasedDictionary dictionary;
        dictionary.load(path, libime::TableFormat::Binary);

        std::ostringstream output;
        dictionary.save(output, libime::TableFormat::Text);
        const std::string decoded = output.str();
        if (decoded.empty()) {
            throw std::runtime_error("libime produced an empty table");
        }

        char *buffer = static_cast<char *>(std::malloc(decoded.size() + 1));
        if (buffer == nullptr) {
            throw std::bad_alloc();
        }
        std::memcpy(buffer, decoded.data(), decoded.size());
        buffer[decoded.size()] = '\0';
        *text = buffer;
        *size = decoded.size();
        return 0;
    } catch (const std::exception &exception) {
        if (error_message != nullptr) {
            *error_message = ::strdup(exception.what());
        }
        return -1;
    } catch (...) {
        if (error_message != nullptr) {
            *error_message = ::strdup("unknown libime error");
        }
        return -1;
    }
}
