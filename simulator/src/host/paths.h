// Where the studio's files live (studio/marquee_studio/paths.py): the content folder, the vendored fonts and
// the emoji (the bought emoji pack, local only and never committed; firmware/emoji, the committed sheet).
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace host {

struct Paths {
    std::filesystem::path repo, content, third_party, fonts, emoji_pack, emoji_dir;
    explicit Paths(const std::filesystem::path& repo_root) : repo(repo_root) {
        const char* c = getenv("MARQUEE_STUDIO_CONTENT");
        content = c && *c ? std::filesystem::path(c) : repo / "studio" / "content";
        third_party = repo / "third_party";
        fonts = third_party / "fonts";
        const char* p = getenv("MARQUEE_EMOJI_PACK");
        emoji_pack = p && *p ? std::filesystem::path(p) : repo / "local" / "emoji-pack";
        emoji_dir = repo / "firmware" / "emoji";
    }
    std::string font(const std::string& name) const { return (content / "fonts" / name).string(); }
    std::string animation(const std::string& name) const { return (content / "animations" / name).string(); }
};

}  // namespace host
