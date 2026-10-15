// Writes a Megolm session made by Hoshi, for interop/megolm_vectors.py check: vodozemac must read it.
// Usage: megolm_tool OUT.json
#include "megolm.h"
#include "json.h"

#include <cstdio>
#include <fstream>

using namespace NHoshi;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: megolm_tool OUT.json\n");
        return 2;
    }
    NMegolm::TOutboundSession session;
    const std::string sessionKey = session.SessionKey();
    TJson::TArray messages;
    for (std::uint32_t index = 0; index < 260; ++index) {
        const std::string text = index == 0 ? "первое от Hoshi" : index == 2 ? "🌠 третье" : "message " + std::to_string(index);
        const std::string ciphertext = session.Encrypt(text);
        if (index == 0 || index == 1 || index == 2 || index == 255 || index == 256 || index == 259) {
            messages.push_back(TJson::TObject{{"index", static_cast<std::int64_t>(index)}, {"plaintext", text}, {"ciphertext", ciphertext}});
        }
    }
    const auto inbound = NMegolm::TInboundSession::FromSessionKey(sessionKey);
    const TJson out(TJson::TObject{
        {"session_id", session.SessionId()},
        {"session_key", sessionKey},
        {"export", inbound ? *inbound->Export(256) : std::string()},
        {"messages", std::move(messages)},
    });
    std::ofstream(argv[1]) << out.Dump();
    return 0;
}
