#pragma once

#include <string>

// Conversion between the `file://` URIs an LSP client speaks and the paths the
// compiler reads. Pure string functions with no dependence on the platform they
// run on, so the Windows shapes are tested on Linux (tests/unit_tests.cpp).
//
// What editors send and expect:
//   POSIX     file:///home/me/my%20project/a.tr    <->  /home/me/my project/a.tr
//   Windows   file:///c%3A/Users/me/a.tr           <->  C:/Users/me/a.tr
//   UNC       file://server/share/a.tr             <->  //server/share/a.tr
// Spaces, non-ASCII characters and anything else outside RFC 3986's unreserved
// set are percent-encoded UTF-8. VS Code writes a drive letter in lower case
// and its colon as %3A, and compares URIs as strings, so pathToUri() produces
// exactly that form.
namespace turf_lsp {

// The path a `file:` URI names, percent-decoded, with forward slashes. A
// Windows drive path comes back as `C:/...` (upper-case letter). A string that
// is not a `file:` URI is returned unchanged (a path is its own path).
std::string uriToPath(const std::string& uri);

// The `file:` URI for a path, in either separator style. A string that
// already is a `file:` URI is returned unchanged.
std::string pathToUri(const std::string& path);

} // namespace turf_lsp
