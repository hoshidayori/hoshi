#include "sas.h"

#include <algorithm>
#include <stdexcept>

namespace NHoshi::NSas {

namespace {

using namespace NCrypto;

// The spec's sas-emoji.json, in order; checked against the copy Element bundles by interop/sas_emoji_from_element.py
constexpr std::array<TEmoji, 64> Emoji = {{
    {"🐶", "Dog"}, {"🐱", "Cat"}, {"🦁", "Lion"}, {"🐎", "Horse"}, {"🦄", "Unicorn"}, {"🐷", "Pig"},
    {"🐘", "Elephant"}, {"🐰", "Rabbit"}, {"🐼", "Panda"}, {"🐓", "Rooster"}, {"🐧", "Penguin"}, {"🐢", "Turtle"},
    {"🐟", "Fish"}, {"🐙", "Octopus"}, {"🦋", "Butterfly"}, {"🌷", "Flower"}, {"🌳", "Tree"}, {"🌵", "Cactus"},
    {"🍄", "Mushroom"}, {"🌏", "Globe"}, {"🌙", "Moon"}, {"☁️", "Cloud"}, {"🔥", "Fire"}, {"🍌", "Banana"},
    {"🍎", "Apple"}, {"🍓", "Strawberry"}, {"🌽", "Corn"}, {"🍕", "Pizza"}, {"🎂", "Cake"}, {"❤️", "Heart"},
    {"😀", "Smiley"}, {"🤖", "Robot"}, {"🎩", "Hat"}, {"👓", "Glasses"}, {"🔧", "Spanner"}, {"🎅", "Santa"},
    {"👍", "Thumbs Up"}, {"☂️", "Umbrella"}, {"⌛", "Hourglass"}, {"⏰", "Clock"}, {"🎁", "Gift"}, {"💡", "Light Bulb"},
    {"📕", "Book"}, {"✏️", "Pencil"}, {"📎", "Paperclip"}, {"✂️", "Scissors"}, {"🔒", "Lock"}, {"🔑", "Key"},
    {"🔨", "Hammer"}, {"☎️", "Telephone"}, {"🏁", "Flag"}, {"🚂", "Train"}, {"🚲", "Bicycle"}, {"✈️", "Aeroplane"},
    {"🚀", "Rocket"}, {"🏆", "Trophy"}, {"⚽", "Ball"}, {"🎸", "Guitar"}, {"🎺", "Trumpet"}, {"🔔", "Bell"},
    {"⚓", "Anchor"}, {"🎧", "Headphones"}, {"📁", "Folder"}, {"📌", "Pin"},
}};

// The first count bytes as a big-endian number
std::uint64_t BigEndian(const TBytes& bytes, size_t count) {
    if (bytes.size() < count) {
        throw std::invalid_argument("too few SAS bytes");
    }
    std::uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

} // namespace

TSas::TSas()
    : Ephemeral(TCurve25519KeyPair::Generate())
{}

std::string TSas::PublicKey() const {
    return Base64Encode(ToBytes(Ephemeral.Public));
}

bool TSas::SetTheirKey(const std::string& base64) {
    const auto key = Base64Decode(base64);
    if (!key || key->size() != 32) {
        return false;
    }
    TKey32 theirs{};
    std::copy(key->begin(), key->end(), theirs.begin());
    try {
        SharedSecret = Ephemeral.SharedSecret(theirs);
    } catch (const std::runtime_error&) {
        return false;  // a low-order key, whose all-zero secret an attacker could predict
    }
    return true;
}

TBytes TSas::Bytes(const std::string& info, size_t count) const {
    if (!SharedSecret) {
        throw std::logic_error("SAS without the other key");
    }
    return HkdfSha256({}, ToBytes(*SharedSecret), ToBytes(info), count);
}

std::string TSas::Mac(const std::string& input, const std::string& info) const {
    const TBytes key = Bytes(info, 32);
    return Base64Encode(ToBytes(HmacSha256(key, ToBytes(input))));
}

std::array<int, 7> EmojiIndices(const TBytes& bytes) {
    const std::uint64_t bits = BigEndian(bytes, 6) >> 6;  // 48 bits -> the first 42
    std::array<int, 7> indices{};
    for (int i = 0; i < 7; ++i) {
        indices[i] = static_cast<int>((bits >> (6 * (6 - i))) & 63);
    }
    return indices;
}

std::array<TEmoji, 7> EmojiFromBytes(const TBytes& bytes) {
    std::array<TEmoji, 7> result{};
    const auto indices = EmojiIndices(bytes);
    for (int i = 0; i < 7; ++i) {
        result[i] = Emoji[indices[i]];
    }
    return result;
}

std::array<int, 3> DecimalsFromBytes(const TBytes& bytes) {
    const std::uint64_t bits = BigEndian(bytes, 5) >> 1;  // 40 bits -> the first 39
    return {
        static_cast<int>((bits >> 26) & 0x1FFF) + 1000,
        static_cast<int>((bits >> 13) & 0x1FFF) + 1000,
        static_cast<int>(bits & 0x1FFF) + 1000,
    };
}

} // namespace NHoshi::NSas
