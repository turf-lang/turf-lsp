#include "Uri.hpp"

#include <cctype>

namespace turf_lsp {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Everything but RFC 3986's unreserved characters and the path separator.
std::string percentEncodePath(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

bool isDriveLetterAt(const std::string& s, size_t i) {
    return i + 1 < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) && s[i + 1] == ':';
}

} // namespace

std::string uriToPath(const std::string& uri) {
    if (uri.rfind("file://", 0) != 0) return uri;
    std::string rest = uri.substr(7);

    // file://host/share/x -- an authority names a network host. file:///x and
    // file://localhost/x are local.
    std::string host;
    if (!rest.empty() && rest[0] != '/') {
        size_t slash = rest.find('/');
        host = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash);
        if (host == "localhost") host.clear();
    }

    std::string path = percentDecode(rest);
    // A drive path arrives as /c:/Users/... ; the slash before the letter is
    // the URI's, not the path's.
    if (path.size() >= 3 && path[0] == '/' && isDriveLetterAt(path, 1)) {
        path.erase(0, 1);
        path[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(path[0])));
    }
    if (!host.empty()) return "//" + host + path;
    return path;
}

std::string pathToUri(const std::string& path) {
    if (path.rfind("file://", 0) == 0) return path;

    std::string p = path;
    for (char& c : p)
        if (c == '\\') c = '/';

    // //server/share/x -> file://server/share/x
    if (p.size() > 2 && p[0] == '/' && p[1] == '/') {
        size_t slash = p.find('/', 2);
        std::string host = p.substr(2, slash == std::string::npos ? std::string::npos : slash - 2);
        std::string tail = slash == std::string::npos ? "" : p.substr(slash);
        return "file://" + host + percentEncodePath(tail);
    }
    // C:/Users/x -> file:///c%3A/Users/x  (lower-case letter, encoded colon)
    if (isDriveLetterAt(p, 0)) {
        std::string drive(1, static_cast<char>(std::tolower(static_cast<unsigned char>(p[0]))));
        return "file:///" + drive + "%3A" + percentEncodePath(p.substr(2));
    }
    if (p.empty() || p[0] != '/') p.insert(p.begin(), '/'); // a relative path: not a real file URI, but total
    return "file://" + percentEncodePath(p);
}

} // namespace turf_lsp
