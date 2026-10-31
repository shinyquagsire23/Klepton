#include "Utils/ResponseValidation.hpp"
#include <cassert>
#include <cstdio>
int main() {
    using SongDownloader::ResponseValidation::HasBeatmapPayload;
    rapidjson::Document document;
    for (const char* payload : {"{}","[]","null","1","\"text\"","{\"error\":\"Not Found\"}"}) {
        document.Parse(payload);assert(!document.HasParseError() && !HasBeatmapPayload(document));
    }
    document.Parse("{\"id\":\"1\",\"name\":\"fixture\"}");
    assert(!document.HasParseError() && HasBeatmapPayload(document));
    document.Parse("{");assert(document.HasParseError() && !HasBeatmapPayload(document));
    puts("SONGDOWNLOADER_RESPONSE_1453_OK valid object accepted; empty/error/scalar/array/malformed responses rejected without end-iterator access.");
}
