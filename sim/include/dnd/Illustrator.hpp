#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dnd/Json.hpp"
#include "dnd/Llm.hpp"

namespace dnd {

// ---------------------------------------------------------------- the story's own history, for pictures

// What the story is so far, condensed by a language model into a short history that a picture can be drawn from: who the
// people are and what they look like and wear, where they are, what has happened and what is happening now. It is rewritten
// (not appended to) each time a picture is asked for and new events have happened since, so it stays short however long the
// game runs, and the newest events win over older facts. Kept per game, on disk as story.json in the game's history folder.
struct StoryDigest {
    std::string text;  // the condensed history, plain prose
    int lastSeq = 0;   // the newest game event it has read
    int updates = 0;   // how many times it has been rewritten
    Json ToJson() const;
    static StoryDigest FromJson(const Json& j);
};

// What a picture of the moment is drawn from, gathered from Game::View() while the game is locked.
struct StoryMaterial {
    struct Line {
        int seq = 0;
        std::string text;
    };
    std::vector<Line> lines;  // the newest story events (narration first, else the event text), oldest first
    int lastSeq = 0;          // the game's newest event: lower than the digest's means the game was rewound
    std::string place;        // location name and description
    std::string party;        // one line per party member: name, sex, race, class, how hurt
    std::string camera;       // what the camera sees (DescribeView)
    bool mature = false;
};
// The newest story lines (at most maxLines) and the rest of the material from a game view.
StoryMaterial GatherStory(const Json& view, const std::string& cameraWords, bool mature, std::size_t maxLines = 60);

// The second language model pass: reads the condensed history and the newest events and writes (1) the new condensed history
// and (2) a Stable Diffusion prompt for a realistic photograph of the moment as the camera sees it. One call does both, so a
// picture costs one model call. The model can be a different one from the story's own (images.json "story": "model", "url").
class Illustrator {
public:
    struct Config {
        int storyWords = 220;   // the condensed history's length
        int promptWords = 60;   // Stable Diffusion 1.5 reads about 75 tokens well
    };
    Illustrator(std::shared_ptr<ILlmClient> client, Config cfg);

    struct Shot {
        bool ok = false;
        std::string prompt;  // the subject to draw (Stable Diffusion phrases)
        std::string error;
    };
    // Updates digest with the material's new lines (or rebuilds it after a rewind) and writes the picture's prompt. On a model
    // failure the digest is left as it was and ok is false; the caller then draws from the camera words alone.
    Shot Compose(StoryDigest& digest, const StoryMaterial& m);

    std::shared_ptr<ILlmClient> Client() const { return client_; }
    std::string Describe() const { return client_ ? client_->Describe() : "none"; }

    // Exposed for tests.
    std::string System(bool mature) const;
    static std::string User(const StoryDigest& digest, const StoryMaterial& m, bool& rebuilt, int& newest);
    static bool Parse(const std::string& reply, std::string& story, std::string& prompt, std::string& err);
    // One line of Stable Diffusion phrases: no quotes, brackets (A1111 weights) or line breaks, cut to at most `words` words.
    static std::string CleanPrompt(const std::string& s, int words);

private:
    std::shared_ptr<ILlmClient> client_;
    Config cfg_;
};

// One game's digest, shared by the HTTP handlers and the picture worker. Loaded from and saved to <dir>/story.json.
struct StoryState {
    std::mutex m;
    StoryDigest digest;
    std::string dir;  // the game's history folder; empty: memory only
    bool loaded = false;
    void LoadIfNeeded();  // call with m held
    void Save() const;    // call with m held
};

}  // namespace dnd
