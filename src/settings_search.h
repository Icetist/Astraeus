#pragma once
#include <string>
#include <vector>

// Settings' search: every card's name and the words on it, lowered once, so a keystroke is a quick pass over a few dozen
// short strings. Results come back best first, with where the match is so it can be lit.
namespace setsearch {

struct Card {
    const wchar_t* title; // as its card names itself
    const wchar_t* words; // what's on it, phrases split by '|'
    bool advanced; // folded under advanced until opened
};

struct Hit {
    int card;
    int score;
    std::wstring line; // what to show under the title: the phrase that matched, or the card's first
    size_t at = 0, len = 0; // the match in line, or in the title when inTitle
    bool inTitle = false;
};

int Count();
const Card& At(int i);
void Find(const std::wstring& query, std::vector<Hit>& out); // best first, at most a screenful

}
