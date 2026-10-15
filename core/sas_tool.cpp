// Drives Hoshi's SAS from interop/sas_interop.py: one JSON command per stdin line, one JSON answer per line.
// Holds one SAS in memory.
#include "sas.h"
#include "json.h"

#include <iostream>
#include <string>

using namespace NHoshi;

int main() {
    NSas::TSas sas;
    std::string line;
    while (std::getline(std::cin, line)) {
        TJson::TObject answer;
        try {
            const TJson command = TJson::Parse(line);
            const std::string cmd = command.StringOr("cmd");
            if (cmd == "key") {
                answer["key"] = sas.PublicKey();
            } else if (cmd == "their_key") {
                answer["ok"] = sas.SetTheirKey(command.StringOr("key"));
            } else if (cmd == "bytes") {
                const auto bytes = sas.Bytes(command.StringOr("info"), 6);
                TJson::TArray emoji;
                for (int index : NSas::EmojiIndices(bytes)) {
                    emoji.push_back(static_cast<std::int64_t>(index));
                }
                TJson::TArray decimals;
                for (int number : NSas::DecimalsFromBytes(bytes)) {
                    decimals.push_back(static_cast<std::int64_t>(number));
                }
                answer["emoji"] = TJson(std::move(emoji));
                answer["decimals"] = TJson(std::move(decimals));
            } else if (cmd == "emoji_table") {
                // Each 6-bit value, put in the top bits of the bytes, gives emoji number value at every position
                TJson::TArray table;
                for (int index = 0; index < 64; ++index) {
                    const std::uint8_t top = static_cast<std::uint8_t>(index << 2);
                    const auto emoji = NSas::EmojiFromBytes({top, 0, 0, 0, 0, 0})[0];
                    table.push_back(TJson::TObject{{"emoji", emoji.Symbol}, {"description", emoji.Description}});
                }
                answer["table"] = TJson(std::move(table));
            } else if (cmd == "mac") {
                answer["mac"] = sas.Mac(command.StringOr("input"), command.StringOr("info"));
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
