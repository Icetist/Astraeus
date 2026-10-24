#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct IDataObject;

namespace sys {

struct ShelfItem { // a stashed file, by its path and never a copy, or a text note
    std::wstring path, text; // exactly one is set
    uint64_t id = 0; // stable while it's on the shelf: previews and ui ids follow it
    bool dir = false; // a folder, read once when it lands
    bool File() const { return !path.empty(); }
    std::wstring Name() const; // file name, or the note's first line
    UINT thumbW = 0, thumbH = 0; // preview size in pixels, 0 until the worker has made one
    std::vector<uint32_t> thumb; // premultiplied bgra
    bool asked = false; // a preview was requested
};

class Shelf { // up to ten things dropped on the island, newest first; costs nothing while nobody drops
public:
    static constexpr size_t kMax = 10;
    static constexpr UINT kThumbPx = 96; // previews are made this big, drawn at 64 dips
    Shelf();
    ~Shelf();
    Shelf(const Shelf&) = delete;
    Shelf& operator=(const Shelf&) = delete;

    void Start(HWND notify, UINT msg); // loads the saved shelf if keeping is on; msg is posted when a preview is ready
    static bool Accepts(IDataObject* data); // files or text
    bool Add(IDataObject* data); // true when something new landed
    void Remove(uint64_t id);
    void Clear();
    void OnPreview(); // ui thread: take the finished previews
    const std::vector<ShelfItem>& Items() const { return items; }
    bool Keep() const { return keep; }
    void SetKeep(bool on); // off deletes the saved file
    bool DragOut(uint64_t id, HWND owner); // modal shell drag, true if it was dropped somewhere
    bool Copy(uint64_t id, HWND owner); // a note's text, or the file itself, onto the clipboard

private:
    void Push(ShelfItem item); // to the front, a repeat moves up instead of doubling
    void Previews(); // queue the files without one; one worker, gone once the queue is empty
    void Save() const;
    void Load();
    ShelfItem* Find(uint64_t id);
    std::vector<ShelfItem> items;
    bool keep = false;
    uint64_t nextId = 1;
    HWND notify = nullptr;
    UINT msg = 0;
    struct Work;
    std::shared_ptr<Work> work; // shared with the preview worker so a late finish never touches a freed shelf
};

}
