// The update notice's pure part (runtime/src/platform/update_notice_core.h), on a computer:
//   c++ -std=c++20 -O1 -Iruntime/src/platform tools/switch/update_notice_host_test.cpp \
//       -o build/update_notice_host_test && build/update_notice_host_test
#include <cstdio>
#include <string>

#include "update_notice_core.h"

static int g_failed = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "%s:%d: FAILED %s\n", __FILE__, __LINE__, #c); \
            g_failed++;                                                  \
        }                                                                \
    } while (0)

int main() {
    using namespace update_notice;
    // versions: build::version()'s forms and the release tags; "dev" never checks
    {
        Version v, r;
        CHECK(parse_version("v0.4.7", &v) && v.major == 0 && v.minor == 4 && v.patch == 7 && v.after == 0);
        CHECK(parse_version("v0.4.7+55", &v) && v.patch == 7 && v.after == 55);
        CHECK(parse_version("0.4.8", &v) && v.patch == 8);
        CHECK(!parse_version("dev", &v));
        CHECK(!parse_version("093c96cf", &v));
        CHECK(!parse_version("v0.4", &v));
        CHECK(!parse_version("v0.4.7x", &v));
        CHECK(!parse_version("", &v));
        parse_version("v0.4.7+55", &v);
        CHECK(parse_version("v0.4.8", &r) && is_newer(r, v));
        CHECK(parse_version("v0.5.0", &r) && is_newer(r, v));
        CHECK(parse_version("v1.0.0", &r) && is_newer(r, v));
        CHECK(parse_version("v0.4.7", &r) && !is_newer(r, v));  // a build past the tag is not older
        CHECK(parse_version("v0.4.6", &r) && !is_newer(r, v));
    }
    // HTTP answers: plain, chunked, an error status, not HTTP
    {
        int status = 0;
        std::string body;
        CHECK(parse_http("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"tag_name\":\"v0.4.8\"}", &status, &body));
        CHECK(status == 200 && body == "{\"tag_name\":\"v0.4.8\"}");
        CHECK(parse_http("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n5\r\nhello\r\n7\r\n, world\r\n0\r\n\r\n", &status, &body));
        CHECK(status == 200 && body == "hello, world");
        CHECK(!parse_http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n50\r\nshort", &status, &body));
        CHECK(parse_http("HTTP/1.1 403 rate limit exceeded\r\nX: y\r\n\r\n{\"message\":\"API rate limit\"}", &status, &body) && status == 403);
        CHECK(!parse_http("<html>", &status, &body));
        CHECK(!parse_http("HTTP/1.1 200 OK\r\nno end of headers", &status, &body));
    }
    // the release's fields in GitHub's JSON
    {
        const std::string json =
            "{\"url\":\"https://api.github.com/x\",\"author\":{\"login\":\"Pulpparty\"},\"tag_name\" : \"v0.4.8\","
            "\"name\":\"SwitchWakerHD v0.4.8 \\\"Minimap\\\"\",\"draft\":false,\"body\":null}";
        CHECK(json_string(json, "tag_name") == "v0.4.8");
        CHECK(json_string(json, "name") == "SwitchWakerHD v0.4.8 \"Minimap\"");
        CHECK(json_string(json, "body").empty());
        CHECK(json_string(json, "missing").empty());
        CHECK(json_string("{\"name\":\"caf\\u00e9\"}", "name") == "caf?");
        CHECK(json_string("{\"tag_name\":\"v1", "tag_name").empty());
    }
    if (g_failed) {
        std::fprintf(stderr, "%d checks failed\n", g_failed);
        return 1;
    }
    std::printf("update_notice: all checks passed\n");
    return 0;
}
