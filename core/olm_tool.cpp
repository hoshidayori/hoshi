// Drives Hoshi's Olm from interop/olm_interop.py: one JSON command per stdin line, one JSON answer per line.
// Holds one account and one session in memory.
#include "olm.h"

#include <iostream>
#include <optional>
#include <string>

using namespace NHoshi;

int main() {
    std::optional<NOlm::TAccount> account;
    std::optional<NOlm::TSession> session;
    std::string line;
    while (std::getline(std::cin, line)) {
        TJson::TObject answer;
        try {
            const TJson command = TJson::Parse(line);
            const std::string cmd = command.StringOr("cmd");
            if (cmd == "account") {
                account = NOlm::TAccount::Create();
                account->GenerateOneTimeKeys(1);
                answer["identity"] = account->IdentityKey();
                answer["one_time_key"] = account->UnpublishedOneTimeKeys().begin()->second;
                account->MarkKeysAsPublished();
            } else if (cmd == "outbound") {
                session = NOlm::TSession::CreateOutbound(*account, command.StringOr("identity"), command.StringOr("one_time_key"));
            } else if (cmd == "inbound") {
                auto inbound = NOlm::TSession::CreateInbound(*account, command.StringOr("body"));
                if (inbound) {
                    session = std::move(inbound->Session);
                    answer["plaintext"] = inbound->Plaintext;
                    answer["one_time_keys_left"] = static_cast<std::int64_t>(account->UnpublishedOneTimeKeys().size() +
                        account->Save()["one_time_keys"].AsArray().size());
                } else {
                    answer["error"] = "cannot create inbound session";
                }
            } else if (cmd == "encrypt") {
                const auto message = session->Encrypt(command.StringOr("text"));
                answer["type"] = static_cast<std::int64_t>(message.Type);
                answer["body"] = message.Body;
            } else if (cmd == "decrypt") {
                const auto plaintext = session->Decrypt({static_cast<int>(command["type"].AsInt()), command.StringOr("body")});
                if (plaintext) {
                    answer["plaintext"] = *plaintext;
                } else {
                    answer["error"] = "cannot decrypt";
                }
            } else if (cmd == "session_id") {
                answer["session_id"] = session->SessionId();
            } else if (cmd == "reload") {
                // Persistence: the session must survive a save and load in the middle of a conversation
                session = NOlm::TSession::Load(TJson::Parse(session->Save().Dump()));
                account = NOlm::TAccount::Load(TJson::Parse(account->Save().Dump()));
                answer["ok"] = session.has_value() && account.has_value();
            } else {
                answer["error"] = "unknown command";
            }
        } catch (const std::exception& e) {
            answer["error"] = e.what();
        }
        std::cout << TJson(std::move(answer)).Dump() << std::endl;
    }
    return 0;
}
