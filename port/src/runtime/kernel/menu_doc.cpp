// BX01 editing for the PC menu rows (see menu_doc.h, port/MENU_PLAN.md). The edits mirror
// port/tools/menu_proto.py, which produces the same bytes from a dumped document.
//
// BX01 (from the game's reader sub_82A8C170 / sub_82A8BFB0, big-endian u32): header "BX01",
// version, element-name table offset, attribute-name table offset; body = node list (count, then
// per element: nameIndex, len, text[len] padded to 4, attrCount, attributes {nameIndex, len,
// value padded to 4}, child list); tables = count, {offset, length, index}, NUL-terminated names.
// Event handlers are compiled Xtal blobs stored as the event element's text: "xtal", version,
// code size, bytecode, then a table of {type, u32 length, chars} (0x0E identifier, 0x0B string,
// 0x07 int) that the bytecode indexes - renaming an entry retargets the script.
#include "menu_doc.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <functional>
#include <map>
#include <stdexcept>
#include <utility>

namespace menu_doc {
namespace {

struct Attr { uint32_t index; std::string value; };      // value as stored (NUL-terminated)
struct Node
{
    uint32_t index = 0;
    std::string text;
    std::vector<Attr> attrs;
    std::vector<Node> children;
};

struct Fail : std::runtime_error { using std::runtime_error::runtime_error; };

uint32_t Be32(const uint8_t* b, size_t size, size_t p)
{
    if (p + 4 > size) throw Fail("truncated document");
    return (uint32_t(b[p]) << 24) | (uint32_t(b[p + 1]) << 16) | (uint32_t(b[p + 2]) << 8) | b[p + 3];
}
void PutBe32(std::string& out, uint32_t v)
{
    out.push_back(char(v >> 24)); out.push_back(char(v >> 16)); out.push_back(char(v >> 8)); out.push_back(char(v));
}
size_t Align4(size_t n) { return (n + 3) & ~size_t(3); }

std::string Bytes(const uint8_t* b, size_t size, size_t p, size_t n)
{
    if (p + n > size) throw Fail("truncated document");
    return std::string(reinterpret_cast<const char*>(b + p), n);
}

std::vector<Node> ReadList(const uint8_t* b, size_t size, size_t& p, int depth)
{
    if (depth > 64) throw Fail("document too deep");
    const uint32_t count = Be32(b, size, p);
    p += 4;
    if (count > 200000) throw Fail("bad node count");
    std::vector<Node> nodes(count);
    for (Node& n : nodes)
    {
        n.index = Be32(b, size, p);
        const uint32_t len = Be32(b, size, p + 4);
        n.text = Bytes(b, size, p + 8, len);
        p += 8 + Align4(len);
        const uint32_t attrs = Be32(b, size, p);
        p += 4;
        if (attrs > 1000) throw Fail("bad attribute count");
        n.attrs.resize(attrs);
        for (Attr& a : n.attrs)
        {
            a.index = Be32(b, size, p);
            const uint32_t vl = Be32(b, size, p + 4);
            a.value = Bytes(b, size, p + 8, vl);
            p += 8 + Align4(vl);
        }
        n.children = ReadList(b, size, p, depth + 1);
    }
    return nodes;
}

void WriteList(const std::vector<Node>& nodes, std::string& out)
{
    PutBe32(out, uint32_t(nodes.size()));
    for (const Node& n : nodes)
    {
        PutBe32(out, n.index);
        PutBe32(out, uint32_t(n.text.size()));
        out += n.text;
        out.append(Align4(n.text.size()) - n.text.size(), '\0');
        PutBe32(out, uint32_t(n.attrs.size()));
        for (const Attr& a : n.attrs)
        {
            PutBe32(out, a.index);
            PutBe32(out, uint32_t(a.value.size()));
            out += a.value;
            out.append(Align4(a.value.size()) - a.value.size(), '\0');
        }
        WriteList(n.children, out);
    }
}

std::map<std::string, uint32_t> NameTable(const uint8_t* b, size_t size, size_t off)
{
    std::map<std::string, uint32_t> names;
    const uint32_t count = Be32(b, size, off);
    if (count > 1000) throw Fail("bad name table");
    const size_t strings = off + 4 + 12 * size_t(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t e = off + 4 + 12 * size_t(i);
        const uint32_t so = Be32(b, size, e), len = Be32(b, size, e + 4), index = Be32(b, size, e + 8);
        std::string s = Bytes(b, size, strings + so, len);
        s.resize(std::strlen(s.c_str()));
        names[s] = index;
    }
    return names;
}

// ---- document model helpers
struct Doc
{
    std::map<std::string, uint32_t> elem, attr;
    std::vector<Node> roots;

    uint32_t E(const char* name) const
    {
        const auto it = elem.find(name);
        if (it == elem.end()) throw Fail(std::string("no element ") + name);
        return it->second;
    }
    uint32_t A(const char* name) const
    {
        const auto it = attr.find(name);
        if (it == attr.end()) throw Fail(std::string("no attribute ") + name);
        return it->second;
    }
    std::string Get(const Node& n, const char* key) const
    {
        const uint32_t k = A(key);
        for (const Attr& a : n.attrs)
            if (a.index == k) return std::string(a.value.c_str());
        return {};
    }
    void Set(Node& n, const char* key, const char* value) const      // value nullptr = remove
    {
        const uint32_t k = A(key);
        for (size_t i = 0; i < n.attrs.size(); ++i)
            if (n.attrs[i].index == k)
            {
                if (value) n.attrs[i].value = std::string(value) + '\0';
                else n.attrs.erase(n.attrs.begin() + std::ptrdiff_t(i));
                return;
            }
        if (value) n.attrs.push_back({ k, std::string(value) + '\0' });
    }
    // The list holding the element called `name` and its position there.
    bool Locate(std::vector<Node>& list, const std::string& name, std::vector<Node>*& owner, size_t& at)
    {
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (Get(list[i], "name") == name) { owner = &list; at = i; return true; }
            if (Locate(list[i].children, name, owner, at)) return true;
        }
        return false;
    }
    Node& Find(const char* name)
    {
        std::vector<Node>* owner = nullptr; size_t at = 0;
        if (!Locate(roots, name, owner, at)) throw Fail(std::string("no node ") + name);
        return (*owner)[at];
    }
    std::vector<Node>& ListOf(const char* name, size_t& at)
    {
        std::vector<Node>* owner = nullptr;
        if (!Locate(roots, name, owner, at)) throw Fail(std::string("no node ") + name);
        return *owner;
    }
    Node& Event(Node& n, const char* event)
    {
        const uint32_t e = E(event);
        for (Node& c : n.children)
            if (c.index == e) return c;
        throw Fail(std::string("no ") + event + " in " + Get(n, "name"));
    }
};

// ---- compiled Xtal edits
void XtalRename(std::string& blob, const std::string& from, const std::string& to)
{
    bool hit = false;
    for (const char tag : { '\x0e', '\x0b' })
    {
        std::string pat(1, tag), rep(1, tag);
        PutBe32(pat, uint32_t(from.size())); pat += from;
        PutBe32(rep, uint32_t(to.size())); rep += to;
        for (size_t at = blob.find(pat); at != std::string::npos; at = blob.find(pat, at + rep.size()))
        {
            blob.replace(at, pat.size(), rep);
            hit = true;
        }
    }
    if (!hit) throw Fail("script has no " + from);
}

void XtalReplaceInt(std::string& blob, uint32_t from, uint32_t to)
{
    std::string pat(1, '\x07'), rep(1, '\x07');
    PutBe32(pat, from);
    PutBe32(rep, to);
    const size_t at = blob.find(pat);
    if (at == std::string::npos || blob.find(pat, at + 1) != std::string::npos) throw Fail("int constant not unique");
    blob.replace(at, pat.size(), rep);
}

// Grow each array literal of a straight-line OnLoad script to `size` elements. An array is
// `42 01 02` (make) then per element `02 00 k` (load constant k) + `43 01 02` (append), every
// element after the first preceded by `02`; so elements = occurrences of 02 00 k 43 01 02, and
// one more is the unit 02 02 00 k 43 01 02 after the last. The code size sits at blob[8..12) and
// the debug tail stores the last pc (code size - 1).
void XtalExtendArrays(std::string& blob, std::initializer_list<uint8_t> consts, size_t size)
{
    const auto* b = reinterpret_cast<const uint8_t*>(blob.data());
    const uint32_t codeSize = Be32(b, blob.size(), 8);
    if (12 + size_t(codeSize) > blob.size()) throw Fail("bad script");
    std::string code = blob.substr(12, codeSize), tail = blob.substr(12 + codeSize);
    for (const uint8_t k : consts)
    {
        const std::string element{ '\x02', '\x00', char(k), '\x43', '\x01', '\x02' };
        const std::string unit = '\x02' + element;
        size_t have = 0;
        for (size_t at = code.find(element); at != std::string::npos; at = code.find(element, at + 1)) ++have;
        if (have < 2 || have > size) throw Fail("unexpected icon array");
        const size_t last = code.rfind(unit) + unit.size();
        std::string extra;
        for (size_t i = have; i < size; ++i) extra += unit;
        code.insert(last, extra);
    }
    std::string oldLast, newLast;
    PutBe32(oldLast, codeSize - 1);
    PutBe32(newLast, uint32_t(code.size()) - 1);
    const size_t at = tail.find(oldLast);
    if (at == std::string::npos || tail.find(oldLast, at + 1) != std::string::npos) throw Fail("script tail pc not unique");
    tail.replace(at, 4, newLast);
    std::string head = blob.substr(0, 8);
    PutBe32(head, uint32_t(code.size()));
    blob = head + code + tail;
}

// One more element at `index` (>= 1) in every array literal of a script - a copy of the element before it - with the
// script's jumps moved past the insertions: 1b = jump (+2-byte offset), 1d = conditional jump (+3 operand bytes, 2-byte
// offset), both relative to the opcode (MEASURED 2026-10-25 on the ONLINE MODE menu's OnLoad: its three build variants
// each make the two icon arrays, the jumps land on the next variant / the end). Throws when a jump does not land inside
// the code; the debug tail's last pc follows the new code size.
void XtalInsertArrayElements(std::string& blob, size_t index)
{
    const auto* b = reinterpret_cast<const uint8_t*>(blob.data());
    const uint32_t codeSize = Be32(b, blob.size(), 8);
    if (12 + size_t(codeSize) > blob.size() || index < 1) throw Fail("bad script");
    const std::string code = blob.substr(12, codeSize), tail = blob.substr(12 + codeSize);
    const std::string make{ '\x42', '\x01', '\x02' };
    std::vector<std::pair<size_t, std::string>> inserts;           // (offset in the old code, bytes)
    for (size_t at = code.find(make); at != std::string::npos; at = code.find(make, at + 1))
    {
        size_t p = at + make.size();
        // the first element: 02 00 k 43 01 02; each later one: 02 02 00 k 43 01 02
        if (p + 6 > code.size() || code[p] != '\x02' || code[p + 1] != '\x00' || code.compare(p + 3, 3, "\x43\x01\x02", 3) != 0)
            continue;
        uint8_t k = uint8_t(code[p + 2]);
        p += 6;
        size_t n = 1;
        while (n < index && p + 7 <= code.size() && code.compare(p, 3, "\x02\x02\x00", 3) == 0 && code.compare(p + 4, 3, "\x43\x01\x02", 3) == 0)
        {
            k = uint8_t(code[p + 3]);
            p += 7;
            ++n;
        }
        if (n < index) throw Fail("an array shorter than the new element's place");
        inserts.push_back({ p, std::string{ '\x02', '\x02', '\x00', char(k), '\x43', '\x01', '\x02' } });
    }
    if (inserts.empty()) throw Fail("no array");
    auto moved = [&](size_t from, size_t to) {                     // bytes inserted in (from, to]
        size_t add = 0;
        for (const auto& [at, bytes] : inserts)
            if (at > from && at <= to) add += bytes.size();
        return add;
    };
    std::string out = code;
    // jumps first (old offsets), then the insertions from the end
    for (size_t i = 0; i < code.size(); ++i)
    {
        const uint8_t op = uint8_t(code[i]);
        size_t field = 0;
        if (op == 0x1b && i + 3 <= code.size()) field = i + 1;
        else if (op == 0x1d && i + 6 <= code.size()) field = i + 4;
        else continue;
        const size_t off = (uint8_t(code[field]) << 8) | uint8_t(code[field + 1]);
        const size_t target = i + off;
        if (target >= code.size()) throw Fail("a jump outside the script");
        const size_t add = moved(i, target);
        if (!add) continue;
        const size_t now = off + add;
        out[field] = char(now >> 8);
        out[field + 1] = char(now & 0xFF);
    }
    for (auto it = inserts.rbegin(); it != inserts.rend(); ++it) out.insert(it->first, it->second);
    std::string oldLast, newLast, t = tail;
    PutBe32(oldLast, codeSize - 1);
    PutBe32(newLast, uint32_t(out.size()) - 1);
    const size_t at = t.find(oldLast);
    if (at == std::string::npos || t.find(oldLast, at + 1) != std::string::npos) throw Fail("script tail pc not unique");
    t.replace(at, 4, newLast);
    std::string head = blob.substr(0, 8);
    PutBe32(head, uint32_t(out.size()));
    blob = head + out + t;
}

// The same, to any size >= 1: elements beyond `size` are removed from the end (each later element
// is one 7-byte unit 02 02 00 k 43 01 02).
void XtalResizeArrays(std::string& blob, std::initializer_list<uint8_t> consts, size_t size)
{
    const auto* b = reinterpret_cast<const uint8_t*>(blob.data());
    const uint32_t codeSize = Be32(b, blob.size(), 8);
    if (12 + size_t(codeSize) > blob.size() || size < 1) throw Fail("bad script");
    std::string code = blob.substr(12, codeSize), tail = blob.substr(12 + codeSize);
    for (const uint8_t k : consts)
    {
        const std::string element{ '\x02', '\x00', char(k), '\x43', '\x01', '\x02' };
        const std::string unit = '\x02' + element;
        size_t have = 0;
        for (size_t at = code.find(element); at != std::string::npos; at = code.find(element, at + 1)) ++have;
        if (have < 2) throw Fail("unexpected icon array");
        const size_t last = code.rfind(unit) + unit.size();
        if (have < size)
        {
            std::string extra;
            for (size_t i = have; i < size; ++i) extra += unit;
            code.insert(last, extra);
        }
        else if (have > size)
            code.erase(last - (have - size) * unit.size(), (have - size) * unit.size());
    }
    std::string oldLast, newLast;
    PutBe32(oldLast, codeSize - 1);
    PutBe32(newLast, uint32_t(code.size()) - 1);
    const size_t at = tail.find(oldLast);
    if (at == std::string::npos || tail.find(oldLast, at + 1) != std::string::npos) throw Fail("script tail pc not unique");
    tail.replace(at, 4, newLast);
    std::string head = blob.substr(0, 8);
    PutBe32(head, uint32_t(code.size()));
    blob = head + code + tail;
}

// One more row in a straight-line OnLoad's icon arrays: the small-icon array gets `icon` (a new
// string constant), the large-icon array a copy of its last element. Arrays as above; the
// constant table ends the blob: u16 count (entry 0 is null), then 0B + u32 length + text each.
void XtalAppendIconRow(std::string& blob, uint8_t lastSmall, uint8_t large, const std::string& icon)
{
    const auto* b = reinterpret_cast<const uint8_t*>(blob.data());
    const uint32_t codeSize = Be32(b, blob.size(), 8);
    if (12 + size_t(codeSize) > blob.size()) throw Fail("bad script");
    std::string code = blob.substr(12, codeSize), tail = blob.substr(12 + codeSize);
    auto unit = [](uint8_t k) { return std::string{ '\x02', '\x02', '\x00', char(k), '\x43', '\x01', '\x02' }; };
    // the constant table: find the count that precedes entry 0 (null, tag 03) and entry 1
    const size_t first = tail.find(std::string{ '\x03', '\x0b', '\x00', '\x00', '\x00' });
    if (first == std::string::npos || first < 2) throw Fail("no constant table");
    const uint8_t count = uint8_t(tail[first - 1]);
    if (tail[first - 2] != '\0' || count >= 0xFF) throw Fail("unexpected constant table");
    const uint8_t k = count;                            // the new constant's index
    tail[first - 1] = char(count + 1);
    std::string entry(1, '\x0b');
    PutBe32(entry, uint32_t(icon.size()));
    tail += entry + icon;
    const size_t largeAt = code.rfind(unit(large));
    if (largeAt == std::string::npos) throw Fail("no large icon array");
    code.insert(largeAt + 7, unit(large));
    const size_t smallAt = code.rfind(unit(lastSmall));
    if (smallAt == std::string::npos || smallAt > largeAt) throw Fail("no small icon array");
    code.insert(smallAt + 7, unit(k));
    std::string oldLast, newLast;
    PutBe32(oldLast, codeSize - 1);
    PutBe32(newLast, uint32_t(code.size()) - 1);
    const size_t at = tail.find(oldLast);
    if (at == std::string::npos || tail.find(oldLast, at + 1) != std::string::npos) throw Fail("script tail pc not unique");
    tail.replace(at, 4, newLast);
    std::string head = blob.substr(0, 8);
    PutBe32(head, uint32_t(code.size()));
    blob = head + code + tail;
}

void AddSystemFrames(Doc& d);
void AddSwitchProfile(Doc& d);
void AddKeyboardMode(Doc& d);
void AddInputDelay(Doc& d);
void AddOnlineNetcode(Doc& d);
void AddDisplayImage(Doc& d);
void AddGraphicsPage(Doc& d);

void Edit(Doc& d, const PcLists& lists, std::string* note)
{
    // ---- Options -> Display Settings: DISPLAY MODE + RESOLUTION before DEFAULT. Rows are clones
    // of 3D DEPTH, whose OnTrigger passes the value cursor as an int:
    //   if (lib::DisplaySettingIsMonitor3DCapable()) {
    //     lib::MenuOptionDisplay3DPallarax(lib::MenuSelectValueGetCursor(sv));          // preview
    //     lib::DisplaySettingSet3DDepth(lib::MenuSelectValueGetCursor(sv).to_i()); }   // store
    // with the natives renamed to the PC ones (same lengths; menu_ext.cpp registers them).
    const Node depth = d.Find("MenuItemMode007-04-04");
    std::vector<Node> rows;
    const struct { int n; const char* label; const char* preview; const char* store; } rowDefs[] = {
        { 6, "DISPLAY MODE", "MenuOptionDisplayWindowMode", "DisplaySettingSetWinMode" },
        { 7, "RESOLUTION", "MenuOptionDisplayResolution", "DisplaySettingSetDispRes" },
    };
    for (const auto& r : rowDefs)
    {
        Node row = depth;
        const std::string sv = "SelectValueMenuItemMode007-04-0" + std::to_string(r.n);
        d.Set(row, "name", ("MenuItemMode007-04-0" + std::to_string(r.n)).c_str());
        d.Set(row, "text", r.label);
        d.Set(row, "textid", nullptr);
        d.Set(row, "value", sv.c_str());
        std::string& trig = d.Event(row, "OnTrigger").text;
        XtalRename(trig, "DisplaySettingIsMonitor3DCapable", "DisplaySettingIsPCDisplayCapable");
        XtalRename(trig, "MenuOptionDisplay3DPallarax", r.preview);
        XtalRename(trig, "DisplaySettingSet3DDepth", r.store);
        XtalRename(trig, "SelectValueMenuItemMode007-04-04", sv);
        rows.push_back(std::move(row));
    }
    {
        size_t at = 0;
        std::vector<Node>& items = d.ListOf("MenuItemMode007-04-05", at);
        items.insert(items.begin() + std::ptrdiff_t(at), rows.begin(), rows.end());
    }
    // The menu's OnLoad builds the icon arrays IFMainMenuL_Cursor loops over (row icons and which
    // help text is visible): one entry per row.
    XtalExtendArrays(d.Event(d.Find("MenuMode007-04-01"), "OnLoad").text, { 1, 2 }, 7);

    // Value lists, in row order right after 3D DEPTH's (value columns are laid out in document
    // order, not by name).
    const Node svTemplate = d.Find("SelectValueMenuItemMode007-04-02");
    const uint32_t itemIndex = d.E("selectvalueitem");
    const std::vector<std::string>* values[] = { &lists.modes, &lists.resolutions };
    for (int i = 1; i >= 0; --i)
    {
        Node sv = svTemplate;
        d.Set(sv, "name", ("SelectValueMenuItemMode007-04-0" + std::to_string(6 + i)).c_str());
        sv.children.clear();
        for (size_t k = 0; k < values[i]->size(); ++k)
        {
            Node item;
            item.index = itemIndex;
            item.text = (*values[i])[k] + '\0';
            // the list's default (the game draws it green, any other value yellow): WINDOWED; 1280x720, display_settings.cpp's
            // default (third since 640x360 and 960x540 came first)
            if (i == 0 ? k == 0 : (*values[i])[k] == "1280x720") d.Set(item, "default", "true");
            sv.children.push_back(std::move(item));
        }
        size_t at = 0;
        std::vector<Node>& list = d.ListOf("SelectValueMenuItemMode007-04-04", at);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, std::move(sv));
    }

    // Help texts are found by position ("StaticMode007-04-0" + row number): the new rows take 5
    // and 6, DEFAULT's moves to 7.
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf("StaticMode007-04-05", at);
        Node a = list[at], b = list[at];
        d.Set(a, "name", "StaticMode007-04-05");
        d.Set(a, "text", "Play in a window, a borderless window or exclusive fullscreen.");
        d.Set(a, "textid", nullptr);
        d.Set(b, "name", "StaticMode007-04-06");
        d.Set(b, "text", "Set the resolution the game renders at.");
        d.Set(b, "textid", nullptr);
        d.Set(list[at], "name", "StaticMode007-04-07");
        list.insert(list.begin() + std::ptrdiff_t(at), { std::move(a), std::move(b) });
    }

    // ---- Main menu: the hidden 9th item (STORE) becomes EXIT.
    Node& store = d.Find("MenuItemMode009");
    d.Set(store, "text", "EXIT");
    d.Set(store, "textid", nullptr);
    d.Set(store, "textid360", nullptr);
    {
        // OnEnter: the main menu's B (OnCancel) path, which plays the menu out and switches to
        // the return-to-title confirmation, retargeted at the exit confirmation.
        std::string enter = d.Event(d.Find("FrameMainMenu"), "OnCancel").text;
        XtalRename(enter, "FrameReturnToTitleDialog", "FrameExitTheGameDialog01");
        XtalRename(enter, "SoundIdMenuCancel", "SoundIdMenuDecide");
        d.Event(d.Find("MenuItemMode009"), "OnEnter").text = enter;
    }
    Node& storeDesc = d.Find("StaticMode009");
    d.Set(storeDesc, "text", "Exit the game.");
    d.Set(storeDesc, "textid", nullptr);
    d.Set(storeDesc, "textid360", nullptr);
    XtalRename(d.Event(d.Find("MenuMainMenu"), "OnFocus").text, "IsOnlineStoreUnlocked", "IsExitMenuItemVisible");

    // ---- The exit confirmation: a copy of the return-to-title one. OnFocus shows text 0x634,
    // retargeted to 0x7F01 (menu_ext.cpp supplies "Exit the game?" and YES / NO labels); OnTick
    // reads the answer through lib::ExitGameWindow, which quits on YES.
    {
        size_t at = 0;
        std::vector<Node>& top = d.ListOf("FrameReturnToTitleDialog", at);
        Node dialog = top[at];
        d.Set(dialog, "name", "FrameExitTheGameDialog01");
        XtalReplaceInt(d.Event(dialog, "OnFocus").text, 0x634, 0x7F01);
        XtalRename(d.Event(dialog, "OnTick").text, "WindowGetValue", "ExitGameWindow");
        top.insert(top.begin() + std::ptrdiff_t(at) + 1, std::move(dialog));
    }

    // VSYNC / SHARPNESS / ANTI-ALIASING / TEXTURE FILTERING: an edit of a copy too.
    Doc image = d;
    try { AddDisplayImage(image); d = std::move(image); }
    catch (const Fail& e) { if (note) *note = std::string("Display Settings image rows left out: ") + e.what(); }

    AddSwitchProfile(d);
    // GRAPHICS SETTINGS (the picture options' page): an edit of a copy too.
    Doc graphics = d;
    try { AddGraphicsPage(graphics); d = std::move(graphics); }
    catch (const Fail& e) { if (note) *note += std::string(note->empty() ? "" : "; ") + "Graphics Settings left out: " + e.what(); }
    AddSystemFrames(d);
    // KEYBOARD edits a copy: if the screen is not as measured, only that part is left out.
    Doc keyboard = d;
    try { AddKeyboardMode(keyboard); d = std::move(keyboard); }
    catch (const Fail& e) { if (note) *note = std::string("Controller Setup KEYBOARD mode left out: ") + e.what(); }
    // So does INPUT DELAY.
    Doc inputDelay = d;
    try { AddInputDelay(inputDelay); d = std::move(inputDelay); }
    catch (const Fail& e) { if (note) *note += std::string(note->empty() ? "" : "; ") + "Game Options INPUT DELAY left out: " + e.what(); }
    // And ONLINE NETCODE (after INPUT DELAY: only with it).
    Doc netcode = d;
    try { AddOnlineNetcode(netcode); d = std::move(netcode); }
    catch (const Fail& e) { if (note) *note += std::string(note->empty() ? "" : "; ") + "Game Options ONLINE NETCODE left out: " + e.what(); }
}

// ---- Options -> Game Options: INPUT DELAY (OFF / ON) before DEFAULT - the game's 2-frame offline
// input delay (input_delay.cpp). The row is a copy of GUARD DAMAGE (OFF / ON, the game's own text
// ids, no script of its own): like GUARD DAMAGE and BATTLE POSITION it is read when the page opens
// and stored when it closes, through menu_ext.cpp's hooks of BATTLE POSITION's cursor and setter.
// The page's icon arrays already hold 7 entries (MEASURED 2026-10-18), so 7 rows need no script edit.
void AddInputDelay(Doc& d)
{
    Node row = d.Find("MenuItemMode007-01-04");
    d.Set(row, "name", "MenuItemMode007-01-07");
    d.Set(row, "text", "INPUT DELAY");
    d.Set(row, "textid", nullptr);
    d.Set(row, "textid360", nullptr);
    d.Set(row, "value", "SelectValueMenuItemMode007-01-07");
    {
        size_t at = 0;
        std::vector<Node>& items = d.ListOf("MenuItemMode007-01-06", at);
        items.insert(items.begin() + std::ptrdiff_t(at), std::move(row));
    }
    // Its value list right after BATTLE POSITION's (value columns follow document order).
    Node sv = d.Find("SelectValueMenuItemMode007-01-04");
    d.Set(sv, "name", "SelectValueMenuItemMode007-01-07");
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf("SelectValueMenuItemMode007-01-05", at);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, std::move(sv));
    }
    // Help texts go by position: the new row is the 6th, DEFAULT's moves to 7.
    size_t at = 0;
    std::vector<Node>& list = d.ListOf("StaticMode007-01-06", at);
    Node help = list[at];
    d.Set(help, "text", "Turn the original 2-frame input delay in offline modes on or off.");
    d.Set(help, "textid", nullptr);
    d.Set(help, "textid360", nullptr);
    d.Set(list[at], "name", "StaticMode007-01-07");
    list.insert(list.begin() + std::ptrdiff_t(at), std::move(help));
}

// ---- Options -> Game Options: ONLINE NETCODE, CUSTOM INPUT DELAY, CUSTOM ROLLBACK after INPUT DELAY
// (display_settings.h; rollback_session.cpp applies them). Copies of GUARD DAMAGE with value lists of
// their own, read and stored like INPUT DELAY (menu_ext.cpp). The page's icon arrays grow to the 10
// rows; help texts go by position ("StaticMode007-01-0" + row: DEFAULT's becomes "...-010").
void AddOnlineNetcode(Doc& d)
{
    const struct { int n; const char* label; const char* help; std::vector<std::string> values; int def; } rows[] = {
        // Help texts stay short: the focused row shows its help under the label, and the long
        // values (PRIORITIZE ...) start at about 60 characters (MEASURED 2026-10-19).
        { 8, "ONLINE NETCODE", "Input delay versus rollback in online matches.",
          { "STANDARD", "PRIORITIZE RESPONSE", "PRIORITIZE GRAPHICS", "CUSTOM" }, 0 },
        { 9, "CUSTOM INPUT DELAY", "Online input delay when ONLINE NETCODE is CUSTOM.",
          { "1 FRAME", "2 FRAMES", "3 FRAMES", "4 FRAMES", "5 FRAMES", "6 FRAMES" }, 0 },
        { 10, "CUSTOM ROLLBACK", "Frames the game may predict online when ONLINE NETCODE is CUSTOM.",
          { "1 FRAME", "2 FRAMES", "3 FRAMES", "4 FRAMES", "5 FRAMES", "6 FRAMES", "7 FRAMES", "8 FRAMES" }, 7 },
    };
    const Node rowTemplate = d.Find("MenuItemMode007-01-04");
    const Node svTemplate = d.Find("SelectValueMenuItemMode007-01-04");
    const uint32_t itemIndex = d.E("selectvalueitem");
    std::vector<Node> items, lists;
    for (const auto& r : rows)
    {
        const std::string n = (r.n < 10 ? "0" : "") + std::to_string(r.n);
        Node row = rowTemplate;
        d.Set(row, "name", ("MenuItemMode007-01-" + n).c_str());
        d.Set(row, "text", r.label);
        d.Set(row, "textid", nullptr);
        d.Set(row, "textid360", nullptr);
        d.Set(row, "value", ("SelectValueMenuItemMode007-01-" + n).c_str());
        items.push_back(std::move(row));
        Node sv = svTemplate;
        d.Set(sv, "name", ("SelectValueMenuItemMode007-01-" + n).c_str());
        sv.children.clear();
        for (size_t k = 0; k < r.values.size(); ++k)
        {
            Node item;
            item.index = itemIndex;
            item.text = r.values[k] + '\0';
            if (int(k) == r.def) d.Set(item, "default", "true");
            sv.children.push_back(std::move(item));
        }
        lists.push_back(std::move(sv));
    }
    // Rows before DEFAULT, value lists after INPUT DELAY's (value columns follow document order).
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf("MenuItemMode007-01-06", at);
        list.insert(list.begin() + std::ptrdiff_t(at), items.begin(), items.end());
    }
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf("SelectValueMenuItemMode007-01-07", at);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, lists.begin(), lists.end());
    }
    XtalExtendArrays(d.Event(d.Find("MenuMode007-01-01"), "OnLoad").text, { 1, 2 }, 10);
    // Help texts: rows 7..9, DEFAULT's moves to 10.
    size_t at = 0;
    std::vector<Node>& list = d.ListOf("StaticMode007-01-07", at);
    std::vector<Node> helps;
    for (size_t i = 0; i < 3; ++i)
    {
        Node help = list[at];
        d.Set(help, "name", ("StaticMode007-01-0" + std::to_string(7 + i)).c_str());
        d.Set(help, "text", rows[i].help);
        d.Set(help, "textid", nullptr);
        d.Set(help, "textid360", nullptr);
        helps.push_back(std::move(help));
    }
    d.Set(list[at], "name", "StaticMode007-01-010");
    list.insert(list.begin() + std::ptrdiff_t(at), helps.begin(), helps.end());
}

// A PC row: a clone of 3D DEPTH (its OnTrigger passes the value cursor as an int) with the natives renamed to PC ones
// (menu_ext.cpp registers them) and a value list of its own - the row (`item`, value `sv`) and the list (a clone of
// `svTemplate`, value `defaultValue` its default).
struct PcRow
{
    const char* label; const char* preview; const char* store; const char* help; std::vector<std::string> values;
    size_t defaultValue = 0;
};
void MakePcRow(Doc& d, const Node& depth, const Node& svTemplate, const PcRow& r, const std::string& item,
               const std::string& sv, std::vector<Node>& items, std::vector<Node>& lists)
{
    Node row = depth;
    d.Set(row, "name", item.c_str());
    d.Set(row, "text", r.label);
    d.Set(row, "textid", nullptr);
    d.Set(row, "value", sv.c_str());
    // 3D DEPTH may already be off the page (AddDisplayImage): the copy is laid out automatically
    d.Set(row, "postype", "auto");
    d.Set(row, "posx", nullptr);
    d.Set(row, "posy", nullptr);
    d.Set(row, "visible", nullptr);
    std::string& trig = d.Event(row, "OnTrigger").text;
    XtalRename(trig, "DisplaySettingIsMonitor3DCapable", "DisplaySettingIsPCDisplayCapable");
    XtalRename(trig, "MenuOptionDisplay3DPallarax", r.preview);
    XtalRename(trig, "DisplaySettingSet3DDepth", r.store);
    XtalRename(trig, "SelectValueMenuItemMode007-04-04", sv);
    items.push_back(std::move(row));
    Node list = svTemplate;
    d.Set(list, "name", sv.c_str());
    list.children.clear();
    const uint32_t itemIndex = d.E("selectvalueitem");
    for (size_t k = 0; k < r.values.size(); ++k)
    {
        Node value;
        value.index = itemIndex;
        value.text = r.values[k] + '\0';
        if (k == r.defaultValue) d.Set(value, "default", "true");
        list.children.push_back(std::move(value));
    }
    lists.push_back(std::move(list));
}

// ---- Options -> Display Settings: VSYNC, BRIGHTNESS, LOW LATENCY and QUICK START after RESOLUTION (display_settings.h;
// the presenter and the frame kick apply them as the cursor moves, QUICK START at the next start). PC rows like DISPLAY
// MODE / RESOLUTION; the page's icon arrays grow to 11 rows (the hidden 3D rows among them); help texts go by position
// (rows 7 to 10, DEFAULT's becomes "StaticMode007-04-011"). The page stops here: with 12 entries in those arrays the
// main menu came up without its rows (MEASURED 2026-11-04; 11 entries with 12 rows left DEFAULT's help text on screen).
// The picture options have a page of their own (AddGraphicsPage).
void AddDisplayImage(Doc& d)
{
    const PcRow rows[] = {
        { "VSYNC", "MenuOptionDisplayVSyncOnOff", "DisplaySettingSetVSyncOn",
          "OFF: less input lag, the picture may tear. Still 60 fps.", { "ON", "OFF" } },
        { "BRIGHTNESS", "MenuOptionDisplayBrightness", "DisplaySettingSetBrightn",
          "Brighter or darker picture. 0: as the game draws it.",
          { "-5", "-4", "-3", "-2", "-1", "0", "+1", "+2", "+3", "+4", "+5" }, 5 },
        { "LOW LATENCY", "MenuOptionDisplayLowLatency", "DisplaySettingSetLowLatn",
          "ON: a frame less input lag in fights, can cost fps. AUTO: if it fits.", { "AUTO", "ON", "OFF" }, 2 },   // OFF since TEST 17
        { "QUICK START", "MenuOptionDisplayQuickStart", "DisplaySettingSetQuickGo",
          "ON: skip the logos and the intro movie when the game starts.", { "OFF", "ON" } },
    };
    const char* const numbers[] = { "11", "12", "13", "14" };
    const Node depth = d.Find("MenuItemMode007-04-04");
    const Node svTemplate = d.Find("SelectValueMenuItemMode007-04-02");
    std::vector<Node> items, lists;
    for (size_t i = 0; i < std::size(rows); ++i)
        MakePcRow(d, depth, svTemplate, rows[i], std::string("MenuItemMode007-04-") + numbers[i],
                  std::string("SelectValueMenuItemMode007-04-") + numbers[i], items, lists);
    // Rows before DEFAULT (after RESOLUTION), value lists after RESOLUTION's (document order).
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf("MenuItemMode007-04-05", at);
        list.insert(list.begin() + std::ptrdiff_t(at), items.begin(), items.end());
    }
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf("SelectValueMenuItemMode007-04-07", at);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, lists.begin(), lists.end());
    }
    XtalExtendArrays(d.Event(d.Find("MenuMode007-04-01"), "OnLoad").text, { 1, 2 }, 7 + std::size(rows));
    // 3D MODE / 3D DEPTH (stereoscopic 3D: never on a PC, the rows stayed greyed out) leave the page. The page's
    // scripts look them up by name, so they stay in the document, out of the automatic layout and hidden (row, value,
    // help text); the cursor already skipped them. MEASURED: only hiding them left their help texts and frames in place.
    for (const char* name : { "MenuItemMode007-04-03", "MenuItemMode007-04-04", "SelectValueMenuItemMode007-04-03",
                              "SelectValueMenuItemMode007-04-04", "StaticMode007-04-03", "StaticMode007-04-04" })
    {
        Node& n = d.Find(name);
        d.Set(n, "postype", "abs");
        d.Set(n, "posx", "-4000");
        d.Set(n, "posy", "-4000");
        d.Set(n, "visible", "false");
    }
    size_t at = 0;
    std::vector<Node>& list = d.ListOf("StaticMode007-04-07", at);
    std::vector<Node> helps;
    for (size_t i = 0; i < std::size(rows); ++i)
    {
        Node help = list[at];
        d.Set(help, "name", ("StaticMode007-04-0" + std::to_string(7 + i)).c_str());
        d.Set(help, "text", rows[i].help);
        d.Set(help, "textid", nullptr);
        helps.push_back(std::move(help));
    }
    d.Set(list[at], "name", ("StaticMode007-04-0" + std::to_string(7 + std::size(rows))).c_str());
    list.insert(list.begin() + std::ptrdiff_t(at), helps.begin(), helps.end());
}

// ---- Options -> Controller Setup: KEYBOARD mode (KEYBOARD_PLAN.md). The screen's artwork has room
// for three rows only (a fourth has no bar and hides EXIT), so player 1's BUTTON SETTINGS row shows
// CONTROLLER / KEYBOARD in its value text, which the game leaves empty (menu_ext.cpp writes it);
// it gets the TYPE value's font and alignment.
void AddKeyboardMode(Doc& d)
{
    Node& value = d.Find("StaticTextMenuItemMode007-03-02-1");
    d.Set(value, "font", "1");
    d.Set(value, "fontalign", "right");
}

// ---- Options: SWITCH PROFILE (6th row, the main menu's PROFILE icon). OnEnter is DISPLAY
// SETTINGS' (play the bars out, switch frame) aimed at FrameSwitchProfileDialog: a copy of the
// return-to-title confirmation, its text 0x634 retargeted to 0x7F04 (menu_ext.cpp: "Switch to
// another profile?" with YES / NO). Its answer is read through lib::SwitchProfileWindow (on YES
// the profile signs out once the title is back, and Select Profile opens); YES then requests the
// game's own SIGNOUT mode change (back to the title), NO returns to Options.
void AddSwitchProfile(Doc& d)
{
    Node row = d.Find("MenuItemMode007-04");
    d.Set(row, "name", "MenuItemMode007-06");
    d.Set(row, "text", "SWITCH PROFILE");
    d.Set(row, "textid", nullptr);
    d.Set(row, "textid360", nullptr);
    XtalRename(d.Event(row, "OnEnter").text, "FrameMode007-04-01", "FrameSwitchProfileDialog");
    {
        size_t at = 0;
        std::vector<Node>& items = d.ListOf("MenuItemMode007-05", at);
        items.insert(items.begin() + std::ptrdiff_t(at) + 1, std::move(row));
    }
    XtalAppendIconRow(d.Event(d.Find("MenuMode007-01"), "OnLoad").text, 5, 6, "icon_s_006");
    for (const char* prefix : { "StaticMode007-0", "StaticValueMode007-0" })
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf((std::string(prefix) + "5").c_str(), at);
        Node copy = list[at];
        d.Set(copy, "name", (std::string(prefix) + "6").c_str());
        d.Set(copy, "text", std::string(prefix) == "StaticMode007-0" ? "Sign out and play as another profile." : "");
        d.Set(copy, "textid", nullptr);
        d.Set(copy, "textid360", nullptr);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, std::move(copy));
    }
    size_t at = 0;
    std::vector<Node>& top = d.ListOf("FrameReturnToTitleDialog", at);
    Node dialog = top[at];
    d.Set(dialog, "name", "FrameSwitchProfileDialog");
    XtalReplaceInt(d.Event(dialog, "OnFocus").text, 0x634, 0x7F04);
    std::string& tick = d.Event(dialog, "OnTick").text;
    XtalRename(tick, "WindowGetValue", "SwitchProfileWindow");
    XtalRename(tick, "FrameMainMenu", "FrameMode007-01");
    top.insert(top.begin() + std::ptrdiff_t(at) + 1, std::move(dialog));
}

bool XtalHas(const std::string& blob, const std::string& name)
{
    for (const char tag : { '\x0e', '\x0b' })
    {
        std::string pat(1, tag);
        PutBe32(pat, uint32_t(name.size()));
        if (blob.find(pat + name) != std::string::npos) return true;
    }
    return false;
}

// ---- Options -> GRAPHICS SETTINGS (5th row, after DISPLAY SETTINGS): SHARPNESS, ANTI-ALIASING, TEXTURE FILTERING,
// SHADOWS, DEPTH OF FIELD, BLOOM, LIGHT SHAFTS, WEATHER EFFECTS and DEFAULT (display_settings.h; the presenter, the
// texture samplers, the renderer, the game's post filters and particle jobs apply them as the cursor moves; TEST 17
// removed DYNAMIC RESOLUTION, post_dof.cpp: WEATHER EFFECTS moved up to row 8, DEFAULT to 9). Display Settings fits 10 rows
// above the info bar (MEASURED 2026-10-29: an 11th put DEFAULT half behind it, and the automatic layout ignores
// positions), so the picture options have a page of their own: FrameMode007-07-01, built from Display Settings' parts
// with names of its own -
//   the frame's OnFocus, OnLostFocus, OnCancel and OnTick are Display Settings'. OnFocus initialises Display
//     Settings' own lists (lib::MenuSelectValueSetCursor, which menu_ext.cpp follows with this page's values) and
//     its hidden 3D rows, as Display Settings does; OnCancel returns to Options. There is no frame OnEnter (Display
//     Settings' stores MOTION BLUR and 3D from its lists; these rows store as they change);
//   the menu's scripts are Display Settings' (the icon arrays are globals, renamed: Display Settings' would be
//     overwritten), the arrays sized to the rows; help texts go by position ("StaticMode007-07-0" + row);
//   the rows are PC rows (MakePcRow); DEFAULT is Display Settings' with lib::GraphicsSettingResetToDefaults for
//     lib::DisplaySettingResetToDefaults (it then re-initialises the lists like OnFocus).
// The Options row is DISPLAY SETTINGS' with OnEnter retargeted; its icons and help / value texts are inserted at
// position 5 (COPYRIGHT's and SWITCH PROFILE's help texts move to 6 and 7).
void AddGraphicsPage(Doc& d)
{
    const PcRow rows[] = {
        { "SHARPNESS", "MenuOptionDisplaySharpening", "DisplaySettingSetSharpen",
          "Sharpen the picture (contrast adaptive sharpening).",
          { "OFF", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" } },
        { "ANTI-ALIASING", "MenuOptionDisplayEdgeSmooth", "DisplaySettingSetEdgesAA",
          "Smooth jagged edges. SMAA: light. SSAA: above RESOLUTION, heavier.",
          { "OFF", "SMAA", "SSAA 1.5X", "SSAA 2X" } },
        { "TEXTURE FILTERING", "MenuOptionDisplayTexFilters", "DisplaySettingSetTexFilt",
          "TRILINEAR: smoother distant textures. ANISOTROPIC: sharp at an angle.",
          { "STANDARD", "TRILINEAR", "ANISOTROPIC 2X", "ANISOTROPIC 4X", "ANISOTROPIC 8X", "ANISOTROPIC 16X" } },
        { "SHADOWS", "MenuOptionDisplayShadowsOff", "DisplaySettingSetShadows",
          "OFF: fighters and stage objects cast no shadows. Lighter on the GPU.", { "ON", "OFF" } },
        { "DEPTH OF FIELD", "MenuOptionDisplayDepthField", "DisplaySettingSetDofBlur",
          "OFF: no depth of field blur, the stage stays sharp. Lighter on the GPU.", { "ON", "OFF" } },
        { "BLOOM", "MenuOptionDisplayBloomGlare", "DisplaySettingSetBloomFx",
          "OFF: no glow around bright light. A little lighter on the GPU.", { "ON", "OFF" } },
        { "LIGHT SHAFTS", "MenuOptionDisplayLightShaft", "DisplaySettingSetShaftFx",
          "OFF: no rays of light on the stages that have them.", { "ON", "OFF" } },
        { "WEATHER EFFECTS", "MenuOptionDisplayRainSnowFx", "DisplaySettingSetWeather",
          "OFF: no falling rain or other stage particles. Lighter on the CPU.", { "ON", "OFF" } },
        { "FPS COUNTER", "MenuOptionDisplayFpsCounter", "DisplaySettingSetFpsShow",
          "Show the frames per second in a corner of the screen.", { "OFF", "TOP LEFT", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM RIGHT" } },
    };
    const size_t count = std::size(rows) + 1;                   // + DEFAULT
    const std::string frameName = "FrameMode007-07-01", menuName = "MenuMode007-07-01", helpPrefix = "StaticMode007-07-0";

    size_t at = 0;
    std::vector<Node>& top = d.ListOf("FrameMode007-04-01", at);
    const Node source = top[at];
    auto child = [&](const Node& parent, const char* name) -> const Node& {
        for (const Node& c : parent.children)
            if (d.Get(c, "name") == name) return c;
        throw Fail(std::string("no ") + name + " in Display Settings");
    };
    auto event = [&](const Node& parent, const char* name) -> Node {
        return d.Event(const_cast<Node&>(parent), name);
    };
    const Node& menuSource = child(source, "MenuMode007-04-01");
    // the menu's scripts with this page's names
    auto rename = [&](std::string& blob) {
        const std::pair<const char*, std::string> names[] = {
            { "MenuMode007-04-01", menuName },
            { "StaticIcon007-04", "StaticIcon007-07" },
            { "StaticMode007-04-0", helpPrefix },
            { "MenuMode007_04_icon_S_array", "MenuMode007_07_icon_S_array" },
            { "MenuMode007_04_icon_L_array", "MenuMode007_07_icon_L_array" } };
        for (const auto& [from, to] : names)
            if (XtalHas(blob, from)) XtalRename(blob, from, to);
    };

    Node frame;
    frame.index = source.index;
    frame.text = source.text;
    frame.attrs = source.attrs;
    d.Set(frame, "name", frameName.c_str());
    for (const char* name : { "OnFocus", "OnLostFocus", "OnCancel", "OnTick" })
    {
        Node e = event(source, name);
        if (std::string(name) == "OnFocus") XtalRename(e.text, "StaticTelopMode007-04", "StaticTelopMode007-07");
        if (std::string(name) == "OnTick") XtalRename(e.text, "MenuMode007-04-01", menuName);
        frame.children.push_back(std::move(e));
    }

    Node menu;
    menu.index = menuSource.index;
    menu.text = menuSource.text;
    menu.attrs = menuSource.attrs;
    d.Set(menu, "name", menuName.c_str());
    const Node depth = d.Find("MenuItemMode007-04-04");
    const Node svTemplate = d.Find("SelectValueMenuItemMode007-04-02");
    std::vector<Node> items, lists;
    for (size_t i = 0; i < std::size(rows); ++i)
        MakePcRow(d, depth, svTemplate, rows[i], "MenuItemMode007-07-0" + std::to_string(i + 1),
                  "SelectValueMenuItemMode007-07-0" + std::to_string(i + 1), items, lists);
    Node reset = child(menuSource, "MenuItemMode007-04-05");
    d.Set(reset, "name", ("MenuItemMode007-07-0" + std::to_string(count)).c_str());
    XtalRename(d.Event(reset, "OnEnter").text, "DisplaySettingResetToDefaults", "GraphicsSettingResetToDefaults");
    items.push_back(std::move(reset));
    for (Node& item : items) menu.children.push_back(std::move(item));
    for (const Node& c : menuSource.children)
    {
        if (c.index == d.E("menuitemtext")) continue;
        Node e = c;                                              // the menu's events
        rename(e.text);
        if (c.index == d.E("OnLoad")) XtalResizeArrays(e.text, { 1, 2 }, count);
        menu.children.push_back(std::move(e));
    }
    frame.children.push_back(std::move(menu));

    // value column (document order): the lists, then DEFAULT's empty value; then the help texts, the icon, the info
    // bar text and the hidden "from" text the Options row writes
    for (Node& list : lists) frame.children.push_back(std::move(list));
    Node resetValue = child(source, "StaticTextMenuItemMode007-04-05");
    d.Set(resetValue, "name", ("StaticTextMenuItemMode007-07-0" + std::to_string(count)).c_str());
    frame.children.push_back(std::move(resetValue));
    const Node& helpTemplate = child(source, "StaticMode007-04-01");
    for (size_t i = 0; i < std::size(rows); ++i)
    {
        Node h = helpTemplate;
        d.Set(h, "name", (helpPrefix + std::to_string(i + 1)).c_str());
        d.Set(h, "text", rows[i].help);
        d.Set(h, "textid", nullptr);
        frame.children.push_back(std::move(h));
    }
    const Node* resetHelp = nullptr;                             // DEFAULT's: the game's text 4,643
    for (const Node& c : source.children)
        if (d.Get(c, "textid") == "4,643") resetHelp = &c;
    if (!resetHelp) throw Fail("no DEFAULT help text");
    Node h = *resetHelp;
    d.Set(h, "name", (helpPrefix + std::to_string(count)).c_str());
    frame.children.push_back(std::move(h));
    for (const char* name : { "StaticIconMode007-04", "StaticTelopMode007-04", "StaticMode007-04From" })
    {
        Node s = child(source, name);
        std::string renamed = name;
        renamed.replace(renamed.find("007-04"), 6, "007-07");
        d.Set(s, "name", renamed.c_str());
        frame.children.push_back(std::move(s));
    }
    top.insert(top.begin() + std::ptrdiff_t(at) + 1, std::move(frame));

    // The Options row, after DISPLAY SETTINGS.
    Node row = d.Find("MenuItemMode007-04");
    d.Set(row, "name", "MenuItemMode007-07");
    d.Set(row, "text", "GRAPHICS SETTINGS");
    d.Set(row, "textid", nullptr);
    d.Set(row, "textid360", nullptr);
    std::string& enter = d.Event(row, "OnEnter").text;
    XtalRename(enter, "FrameMode007-04-01", frameName);
    XtalRename(enter, "StaticMode007-04From", "StaticMode007-07From");
    {
        std::vector<Node>& list = d.ListOf("MenuItemMode007-04", at);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, std::move(row));
    }
    XtalInsertArrayElements(d.Event(d.Find("MenuMode007-01"), "OnLoad").text, 4);  // a copy of DISPLAY SETTINGS' icons
    for (const char* prefix : { "StaticMode007-0", "StaticValueMode007-0" })
    {
        const std::string p = prefix;
        d.Set(d.Find((p + "6").c_str()), "name", (p + "7").c_str());
        d.Set(d.Find((p + "5").c_str()), "name", (p + "6").c_str());
        std::vector<Node>& list = d.ListOf((p + "4").c_str(), at);
        Node copy = list[at];
        d.Set(copy, "name", (p + "5").c_str());
        // as short as the game's own (a longer text scrolls - MEASURED 2026-10-29)
        d.Set(copy, "text", p == "StaticMode007-0" ? "Adjust picture quality, such as sharpness and shadows." : "");
        d.Set(copy, "textid", nullptr);
        d.Set(copy, "textid360", nullptr);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, std::move(copy));
    }
}

// ---- System UI frames (menu_ext.cpp, PROFILE_PLAN.md): the console-side screens - Select Profile
// and profile name entry - drawn in Display Settings' style (IFMainMenuL: long bars, a value
// column, one icon on every row, here the main menu's PROFILE icon). The interface draws one bar
// per icon-array entry, so there is one frame per row count: FrameSysMenu<N> (N = 2..9) with rows
// MenuItemSysMenu<N>_01.., value texts StaticValueSysMenu<N>_01.. and help texts
// StaticSysMenu<N>_01.., all set at run time. Only the visual scripts are kept (the menu's OnLoad /
// OnFocus / OnTrigger run IFMainMenuL_Cursor for the icons and the visible help text; the frame's
// OnTick shows the key hints): the system UI moves the cursor itself, because menus receive no
// input while the title screen runs. FrameSysEmpty (no menu) closes the screens.
void AddSystemFrames(Doc& d)
{
    size_t at = 0;
    std::vector<Node>& top = d.ListOf("FrameMode007-04-01", at);
    const Node source = top[at];
    auto two = [](int i) { return std::string(i < 10 ? "0" : "") + std::to_string(i); };
    auto find = [&](const char* name) -> const Node& {
        for (const Node& c : source.children)
            if (d.Get(c, "name") == name) return c;
        throw Fail(std::string("no ") + name + " in Display Settings");
    };
    const Node& menuSource = find("MenuMode007-04-01");
    const Node* itemTemplate = nullptr;
    for (const Node& c : menuSource.children)
        if (d.Get(c, "name") == "MenuItemMode007-04-05") itemTemplate = &c;
    if (!itemTemplate) throw Fail("no DEFAULT row");
    const Node& valueTemplate = find("StaticTextMenuItemMode007-04-05");
    const Node& descTemplate = find("StaticMode007-04-06");

    std::vector<Node> frames;
    for (int rows = 2; rows <= 9; ++rows)
    {
        const std::string n = std::to_string(rows);
        const std::string menuName = "MenuSysMenu" + n;
        auto renameAll = [&](std::string& blob) {
            const std::pair<std::string, std::string> names[] = {
                { "MenuMode007-04-01", menuName },
                { "StaticIcon007-04", "StaticIconSysMenu" + n },
                { "StaticMode007-04-0", "StaticSysMenu" + n + "_0" },
                { "MenuMode007_04_icon_S_array", "SysMenu" + n + "_icon_S_array" },
                { "MenuMode007_04_icon_L_array", "SysMenu" + n + "_icon_L_array" } };
            for (const auto& [from, to] : names)
                if (blob.find(from) != std::string::npos) XtalRename(blob, from, to);
        };

        Node frame;
        frame.index = source.index;
        frame.text = source.text;
        frame.attrs = source.attrs;
        d.Set(frame, "name", ("FrameSysMenu" + n).c_str());
        for (const Node& c : source.children)
            if (c.index == d.E("OnTick"))
            {
                Node tick = c;
                XtalRename(tick.text, "MenuMode007-04-01", menuName);
                frame.children.push_back(std::move(tick));
            }

        Node menu;
        menu.index = menuSource.index;
        menu.text = menuSource.text;
        menu.attrs = menuSource.attrs;
        d.Set(menu, "name", menuName.c_str());
        d.Set(menu, "value", nullptr);
        for (int i = 1; i <= rows; ++i)
        {
            Node item;
            item.index = itemTemplate->index;
            item.text = itemTemplate->text;
            item.attrs = itemTemplate->attrs;              // DEFAULT: no value list
            d.Set(item, "name", ("MenuItemSysMenu" + n + "_" + two(i)).c_str());
            d.Set(item, "text", "-");
            d.Set(item, "textid", nullptr);
            menu.children.push_back(std::move(item));
        }
        for (const char* event : { "OnLoad", "OnFocus", "OnTrigger" })
        {
            Node e = d.Event(const_cast<Node&>(menuSource), event);
            renameAll(e.text);
            if (std::string(event) == "OnLoad")
            {
                XtalRename(e.text, "icon_s_046", "icon_s_006");    // the main menu's PROFILE icon
                XtalResizeArrays(e.text, { 1, 2 }, size_t(rows));
            }
            menu.children.push_back(std::move(e));
        }
        frame.children.push_back(std::move(menu));

        for (int i = 1; i <= rows; ++i)                    // value column: by document order
        {
            Node v = valueTemplate;
            d.Set(v, "name", ("StaticValueSysMenu" + n + "_" + two(i)).c_str());
            d.Set(v, "text", "");
            d.Set(v, "font", "1");                         // as the value lists: right-aligned
            d.Set(v, "fontalign", "right");
            frame.children.push_back(std::move(v));
        }
        for (int i = 1; i <= rows; ++i)
        {
            Node h = descTemplate;
            d.Set(h, "name", ("StaticSysMenu" + n + "_" + two(i)).c_str());
            d.Set(h, "text", "-");
            frame.children.push_back(std::move(h));
        }
        Node icon = find("StaticIconMode007-04");
        d.Set(icon, "name", ("StaticIconSysMenu" + n).c_str());
        frame.children.push_back(std::move(icon));
        Node telop = find("StaticTelopMode007-04");
        d.Set(telop, "name", ("StaticTelopSysMenu" + n).c_str());
        frame.children.push_back(std::move(telop));
        frames.push_back(std::move(frame));
    }

    Node empty;
    empty.index = source.index;
    empty.text = source.text;
    d.Set(empty, "name", "FrameSysEmpty");
    frames.push_back(std::move(empty));
    top.insert(top.begin() + std::ptrdiff_t(at) + 1, frames.begin(), frames.end());
}

// ---- ONLINE MODE menu (document 001): SERVER, a 7th row after LEADERBOARDS - the network the online
// modes use (STEAM or a community server, port/COMMUNITY_SERVER_PLAN.md; server_ui.cpp writes its name
// in the value column). A copy of LEADERBOARDS whose OnEnter (play the bars out, switch frame) opens the
// server list; help and value texts go by position ("StaticOnlineMode00" / "StaticOnlineModeValue00" +
// row).
// ---- the Player Match room's Friend Invite (same document): its OnEnter's native, the room's operation for the
// console's friends list, becomes a PC native of the same length (menu_ext.cpp: a Steam friend picker, server_ui.cpp).
// MEASURED 2026-10-23: the original does nothing on PC (XamShowFriendsUI is never reached).
void AddFriendInvite(Doc& d)
{
    XtalRename(d.Event(d.Find("MenuItemPlayerMatch04-Friend"), "OnEnter").text, "OnlinePlayerMatchOperationMenuSetValue",
               "OnlinePlayerMatchSteamFriendInviteOpen");
}

// ---- the room's REMATCH MENU row (port/ONLINE_2V2_PLAN.md "Start and rematch"): after KICK, a copy of LICENSE (a row
// whose OnEnter calls one native) with a native of its own that turns the rematch menu on or off (online_rematch.cpp; the
// value column says ON / OFF). The room host's alone, as KICK - online_2v2_team.cpp shows it only there - so for everyone
// else both are hidden at the end of the list and no help line moves (help texts go by place: "StaticPlayerMatchRoom0" +
// row, value texts by document order). The page's icon array grows to the 6 rows.
void AddRematchRow(Doc& d)
{
    // copies, not references: the inserts below move the document's nodes
    Node row = d.Find("MenuItemPlayerMatch03-License");
    Node value = d.Find("StaticPlayerMatchRoomValue05");
    Node help = d.Find("StaticPlayerMatchRoom05");
    d.Set(row, "name", "MenuItemPlayerMatch06-Rematch");
    d.Set(row, "text", "REMATCH MENU");
    d.Set(row, "textid", nullptr);
    d.Set(row, "textid360", nullptr);
    XtalRename(d.Event(row, "OnEnter").text, "OnlinePlayerMatchOperationMenuSetValue", "OnlineRoomRematchToggle");
    {
        size_t at = 0;
        std::vector<Node>& items = d.ListOf("MenuItemPlayerMatch05-KickOut", at);
        items.insert(items.begin() + std::ptrdiff_t(at) + 1, std::move(row));
    }
    d.Set(value, "name", "StaticPlayerMatchRoomValue06");
    d.Set(value, "fontalign", "right");                 // in the value column, as the rows' value lists
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf("StaticPlayerMatchRoomValue05", at);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, std::move(value));
    }
    d.Set(help, "name", "StaticPlayerMatchRoom06");
    d.Set(help, "text", "After each match the players choose: rematch, stage select, character select or the room.");
    d.Set(help, "textid", nullptr);
    d.Set(help, "textid360", nullptr);
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf("StaticPlayerMatchRoom05", at);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, std::move(help));
    }
    Node& frame = d.Find("FrameOnlinePlayerMatchRoom");
    XtalResizeArrays(d.Event(frame, "OnLoad").text, { 1 }, 6);
}

// ---- the rematch menu (online_rematch.cpp, port/ONLINE_2V2_PLAN.md "Start and rematch"): after a Player Match or PAIR
// PLAY match, the ranked match's end menu (FrameRankMatchResult, IFPauseMenuS) with the room's choices - one frame per row
// count, as the interface draws a bar per icon-array entry: FrameRematch5 (REMATCH, STAGE SELECT, CHARACTER SELECT,
// RETURN TO ROOM, LEAVE) and FrameRematch6 (PAIR PLAY: CHANGE TEAMS before RETURN TO ROOM). The menu's scripts are the ranked
// menu's with names of their own (the icon arrays are globals: the ranked menu's would be overwritten), the arrays sized
// to the rows; help texts go by place ("StaticRematch<N>_0" + row), the value texts StaticRematch<N>Value<row> show who
// chose what. Its OnEnter (a decide sound and a delayed frame switch) is the room's LICENSE row's, the native renamed:
// lib::OnlineRematchChoose (menu_ext.cpp) takes the focused row and the menu stays up.
struct RematchRow { const char* text; const char* help; };
const RematchRow kRematchRows[6] = {                   // by choice (online_rematch.cpp's order)
    { "REMATCH", "Same characters, same stage." },
    { "STAGE SELECT", "Same characters, a new stage." },
    { "CHARACTER SELECT", "New characters and stage." },
    { "CHANGE TEAMS", "New teams, then characters." },
    { "RETURN TO ROOM", "Back to the room." },
    { "LEAVE", "Return to Online Mode menu." } };     // the game's own words (online texts 347 / 315)
// LEAVE's help for the room host: its leave closes the session (the game has no host migration - texts 348 / 350)
const char* const kRematchHostLeaveHelp = "Close the session. Everyone returns to the Online Mode menu.";
void AddRematchFrames(Doc& d)
{
    size_t at = 0;
    std::vector<Node>& top = d.ListOf("FrameRankMatchResult", at);
    const Node source = top[at];
    Node enter = d.Event(d.Find("MenuItemPlayerMatch03-License"), "OnEnter");
    XtalRename(enter.text, "OnlinePlayerMatchOperationMenuSetValue", "OnlineRematchChoose");
    const RematchRow* const kRows = kRematchRows;
    std::vector<Node> frames;
    for (const int rows : { 5, 6 })
    {
        const std::string n = std::to_string(rows), menuName = "MenuRematch" + n;
        std::vector<int> shown = { 0, 1, 2, 4, 5 };
        if (rows == 6) shown.insert(shown.begin() + 3, 3);
        auto rename = [&](std::string& blob) {
            const std::pair<std::string, std::string> names[] = {
                { "MenuRankMatchResult", menuName },
                { "MenuRankMatchResult-03", menuName + "-03" },
                { "StaticIconRankMatchResult", "StaticIconRematch" + n },
                { "StaticRankMatchResult00", "StaticRematch" + n + "_0" },
                { " ResultMenuRankMatch_icon_S_array", " Rematch" + n + "_icon_S_array" },
                { " ResultMenuRankMatch_icon_L_array", " Rematch" + n + "_icon_L_array" } };
            for (const auto& [from, to] : names)
                if (XtalHas(blob, from)) XtalRename(blob, from, to);
        };
        Node frame;
        frame.index = source.index;
        frame.text = source.text;
        frame.attrs = source.attrs;
        d.Set(frame, "name", ("FrameRematch" + n).c_str());
        bool helps = false, values = false;
        for (const Node& c : source.children)
        {
            const std::string name = d.Get(c, "name");
            if (name == "MenuRankMatchResult")
            {
                Node menu;
                menu.index = c.index;
                menu.text = c.text;
                menu.attrs = c.attrs;
                d.Set(menu, "name", menuName.c_str());
                bool items = false;
                for (const Node& e : c.children)
                {
                    const std::string item = d.Get(e, "name");
                    if (item.rfind("MenuRankMatchResult-", 0) == 0)
                    {
                        if (std::exchange(items, true)) continue;
                        for (int i = 1; i <= rows; ++i)
                        {
                            Node it = e;
                            const RematchRow& r = kRows[shown[size_t(i - 1)]];
                            d.Set(it, "name", (menuName + "-0" + std::to_string(i)).c_str());
                            d.Set(it, "text", r.text);
                            d.Set(it, "textid", nullptr);
                            d.Set(it, "textid360", nullptr);
                            menu.children.push_back(std::move(it));
                        }
                        continue;
                    }
                    Node ev = e.index == d.E("OnEnter") ? enter : e;
                    rename(ev.text);
                    if (e.index == d.E("OnLoad")) XtalResizeArrays(ev.text, { 1, 2 }, size_t(rows));
                    menu.children.push_back(std::move(ev));
                }
                if (!items) throw Fail("no rows in the ranked end menu");
                frame.children.push_back(std::move(menu));
            }
            else if (name.rfind("StaticRankMatchResult00", 0) == 0)
            {
                if (std::exchange(helps, true)) continue;
                for (int i = 1; i <= rows; ++i)
                {
                    Node h = c;
                    d.Set(h, "name", ("StaticRematch" + n + "_0" + std::to_string(i)).c_str());
                    d.Set(h, "text", kRows[shown[size_t(i - 1)]].help);
                    d.Set(h, "textid", nullptr);
                    d.Set(h, "textid360", nullptr);
                    frame.children.push_back(std::move(h));
                }
            }
            else if (name.rfind("StaticRankMatchValue", 0) == 0)
            {
                if (std::exchange(values, true)) continue;
                for (int i = 1; i <= rows; ++i)
                {
                    Node v = c;
                    d.Set(v, "name", ("StaticRematch" + n + "Value" + std::to_string(i)).c_str());
                    d.Set(v, "text", "");
                    d.Set(v, "font", "1");
                    d.Set(v, "fontalign", "right");
                    frame.children.push_back(std::move(v));
                }
            }
            else
            {
                Node other = c;
                if (name == "StaticIconRankMatchResult") d.Set(other, "name", ("StaticIconRematch" + n).c_str());
                else if (!name.empty()) throw Fail("unexpected " + name + " in the ranked end menu");
                rename(other.text);
                frame.children.push_back(std::move(other));
            }
        }
        if (!helps || !values) throw Fail("no help or value texts in the ranked end menu");
        frames.push_back(std::move(frame));
    }
    top.insert(top.begin() + std::ptrdiff_t(at) + 1, frames.begin(), frames.end());
}

// ---- PAIR PLAY's place screen: the players' names on its stones (the user, 2026-10-25: "instead of showing the controller
// you can show the player name"; 2026-11-09: "we didn't know who's player 1 or 2 or 3 or 4"). The screen is a Lumen movie
// with no texts of its own (online_2v2_places.cpp) and the room's frame is closed by then (the room's battle start puts it
// away; CHANGE TEAMS closes the end menu's) - so a frame of their own, FrameOnlinePairNames (no interface, no scripts or
// menu: as FrameSysEmpty), with four texts at the stones' faces (absolute places on the 1280x720 screen, the game's font),
// empty until online_2v2_places.cpp writes a name; it is shown only while the screen takes presses. Place 1 / 2 = the red
// side's lower / upper stone, 3 / 4 the blue side's.
void AddPairNames(Doc& d)
{
    const Node model = d.Find("StaticIconPlayerMatchRoom");      // a statictext of the room's frame
    static const struct { int x, y; } kAt[4] = { { 422, 440 }, { 225, 275 }, { 852, 440 }, { 1057, 275 } };
    size_t at = 0;
    std::vector<Node>& top = d.ListOf("FrameOnlinePlayerMatchRoom", at);
    Node frame;
    frame.index = top[at].index;
    frame.text = top[at].text;
    d.Set(frame, "name", "FrameOnlinePairNames");
    for (int i = 0; i < 4; ++i)
    {
        Node n = model;
        n.children.clear();
        const std::string name = "TextPairPlace" + std::to_string(i + 1), x = std::to_string(kAt[i].x), y = std::to_string(kAt[i].y);
        d.Set(n, "name", name.c_str());
        d.Set(n, "text", "");
        d.Set(n, "font", "1");
        d.Set(n, "fontalign", "center");
        d.Set(n, "postype", "abs");
        d.Set(n, "posx", x.c_str());
        d.Set(n, "posy", y.c_str());
        d.Set(n, "poslabel", nullptr);
        d.Set(n, "visible", nullptr);
        frame.children.push_back(std::move(n));
    }
    top.insert(top.begin() + std::ptrdiff_t(at) + 1, std::move(frame));
}

// Every script of the document that names `from` names `to` instead.
void RenameInScripts(Doc& d, const std::string& from, const std::string& to)
{
    std::function<void(std::vector<Node>&)> walk = [&](std::vector<Node>& list) {
        for (Node& n : list)
        {
            if (n.text.size() > 12 && n.text.compare(0, 4, "xtal") == 0)
                try { XtalRename(n.text, from, to); } catch (const Fail&) {}
            walk(n.children);
        }
    };
    walk(d.roots);
}

// ---- PAIR PLAY in the ONLINE MODE menu (the user, 2026-10-25: "Pair play should be a separate option and not part of
// player match"): a copy of PLAYER MATCH right after it, opening the same pages - online_2v2.cpp tells which row opened them
// (pair mode: PAIR PLAY rooms, apart from Player Match's). Help and value texts go by place ("StaticOnlineMode00" + place
// + 1): those after it move down one (in the scripts that name them too), as do the menu's icon arrays (its OnLoad).
// After AddServerRow (SERVER's texts are 007).
void AddPairPlayRow(Doc& d)
{
    Node row = d.Find("MenuItemOnline02-player-match");
    d.Set(row, "name", "MenuItemOnline02b-pair-play");
    d.Set(row, "text", "PAIR PLAY");
    d.Set(row, "textid", nullptr);
    d.Set(row, "textid360", nullptr);
    for (int k = 7; k >= 3; --k)
        for (const char* stem : { "StaticOnlineMode00", "StaticOnlineModeValue00" })
        {
            const std::string from = stem + std::to_string(k), to = stem + std::to_string(k + 1);
            d.Set(d.Find(from.c_str()), "name", to.c_str());
            RenameInScripts(d, from, to);
        }
    const struct { const char* after; const char* name; const char* text; } statics[] = {
        { "StaticOnlineMode002", "StaticOnlineMode003", "2v2 tag battles: team up with friends, each on their own PC." },
        { "StaticOnlineModeValue002", "StaticOnlineModeValue003", "" },
    };
    for (const auto& st : statics)
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf(st.after, at);
        Node copy = list[at];
        d.Set(copy, "name", st.name);
        d.Set(copy, "text", st.text);
        d.Set(copy, "textid", nullptr);
        d.Set(copy, "textid360", nullptr);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, std::move(copy));
    }
    XtalInsertArrayElements(d.Event(d.Find("MenuOnlineMenu"), "OnLoad").text, 2);
    size_t at = 0;
    std::vector<Node>& items = d.ListOf("MenuItemOnline02-player-match", at);
    items.insert(items.begin() + std::ptrdiff_t(at) + 1, std::move(row));
}

void AddServerRow(Doc& d)
{
    Node row = d.Find("MenuItemOnline07-leaderboard");
    d.Set(row, "name", "MenuItemOnline08-server");
    d.Set(row, "text", "SERVER");
    d.Set(row, "textid", nullptr);
    d.Set(row, "textid360", nullptr);
    XtalRename(d.Event(row, "OnEnter").text, "FrameOnline07", "FrameOnlineServerList");
    {
        size_t at = 0;
        std::vector<Node>& items = d.ListOf("MenuItemOnline07-leaderboard", at);
        items.insert(items.begin() + std::ptrdiff_t(at) + 1, std::move(row));
    }
    const struct { const char* from; const char* to; const char* text; } statics[] = {
        { "StaticOnlineMode006", "StaticOnlineMode007", "Play on Steam or a community server." },
        { "StaticOnlineModeValue006", "StaticOnlineModeValue007", "STEAM" },
    };
    for (const auto& s : statics)
    {
        size_t at = 0;
        std::vector<Node>& list = d.ListOf(s.from, at);
        Node copy = list[at];
        d.Set(copy, "name", s.to);
        d.Set(copy, "text", s.text);
        d.Set(copy, "textid", nullptr);
        d.Set(copy, "textid360", nullptr);
        list.insert(list.begin() + std::ptrdiff_t(at) + 1, std::move(copy));
    }
    // The value (a server's name, longer than the menu's own values) right-aligned in the plain font, as the
    // online pages' value columns: it ends where the column starts, short of the focused row's help text
    // (the menu font's wider letters reached it - MEASURED 2026-10-21).
    d.Set(d.Find("StaticOnlineModeValue007"), "fontalign", "right");
}

// ---- The server list in the online menus' own style (the user's pick, phase 0 mock-up B): a copy of
// Search Session's criteria page (IFOnlineMenuM: label + value column, the focused row's help under its
// label, scroll arrows) with kServerRows rows whose texts server_ui.cpp sets (and scrolls); B returns to
// the ONLINE MODE menu. Every name the copied scripts use is renamed to the copy's; the page hides its
// VOICE CHAT row in its OnLoad, which here hides a spare row.
constexpr int kServerRows = 8;                                   // server_ui.cpp kRows
void AddServerListFrame(Doc& d)
{
    size_t at = 0;
    std::vector<Node>& top = d.ListOf("FrameOnline020201-start_search", at);
    const Node& source = top[at];
    const std::pair<std::string, std::string> renames[] = {
        { "MenuOnline020201-start_search", "MenuOnlineServerList" },
        { "StaticTelopOnline0202", "StaticTelopOnlineSrv" },
        { "StaticIconOnline0202", "StaticIconOnlineSrv" },
        { "StaticOnline020200", "StaticOnlineSrv00" },
        { "MenuOnline0202_icon_L_array", "MenuOnlineSrv_icon_L_array" },
        { "MenuOnline020201_VoiceChat", "MenuOnlineSrv_VoiceChat" },
        { "MenuItemOnline020206-voice_chat", "MenuItemOnlineSrvSpare" },
        { "SelectValueMenuItemOnline020206-voice_chat", "SelectValueOnlineSrvSpare" },
        { "StaticOnline0202005", "StaticOnlineSrvSpare" },
        { "MenuItemOnline020201-start_search", "MenuItemOnlineSrv01" },
        { "MenuItemOnline0202-custom_match", "MenuItemOnline08-server" },
        { "FrameOnline0201-quick_match", "FrameOnlineMenu" },
        // the OnTick's key help: the host native adds SERVER INFO / REMOVE in front (menu_ext.cpp)
        { "WindowPrintKeyInfo", "ServerListKeyInfo" } };
    auto has = [](const std::string& blob, const std::string& name) {     // an identifier / string entry
        for (const char tag : { '\x0e', '\x0b' })
        {
            std::string pat(1, tag);
            PutBe32(pat, uint32_t(name.size()));
            if (blob.find(pat + name) != std::string::npos) return true;
        }
        return false;
    };
    auto renameAll = [&](std::string& blob) {
        for (const auto& [from, to] : renames)
            if (has(blob, from)) XtalRename(blob, from, to);
    };
    auto two = [](int i) { return std::string(i < 10 ? "0" : "") + std::to_string(i); };
    auto child = [&](const char* name) -> const Node& {
        for (const Node& c : source.children)
            if (d.Get(c, "name") == name) return c;
        throw Fail(std::string("no ") + name + " in Search Session");
    };

    Node frame;
    frame.index = source.index;
    frame.text = source.text;
    frame.attrs = source.attrs;
    d.Set(frame, "name", "FrameOnlineServerList");
    for (const Node& c : source.children)
        if (d.Get(c, "name").empty())                            // the frame's own events
        {
            Node e = c;
            renameAll(e.text);
            frame.children.push_back(std::move(e));
        }
    const Node& menuSource = child("MenuOnline020201-start_search");
    Node menu;
    menu.index = menuSource.index;
    menu.text = menuSource.text;
    menu.attrs = menuSource.attrs;
    d.Set(menu, "name", "MenuOnlineServerList");
    const Node* rowTemplate = nullptr;
    for (const Node& c : menuSource.children)
        if (d.Get(c, "name") == "MenuItemOnline020201-start_search") rowTemplate = &c;
    if (!rowTemplate) throw Fail("no SEARCH row");
    for (int i = 1; i <= kServerRows + 1; ++i)
    {
        Node row;
        row.index = rowTemplate->index;
        row.text = rowTemplate->text;
        row.attrs = rowTemplate->attrs;
        d.Set(row, "name", i <= kServerRows ? ("MenuItemOnlineSrv" + two(i)).c_str() : "MenuItemOnlineSrvSpare");
        d.Set(row, "text", "-");
        d.Set(row, "textid", nullptr);
        menu.children.push_back(std::move(row));
    }
    for (const Node& c : menuSource.children)
        if (d.Get(c, "name").empty())                            // the menu's events
        {
            Node e = c;
            renameAll(e.text);
            // OnLoad's icon array (constant 1, "icon_L_01"): one entry per row, as the cursor script
            // hides the help texts of the rows it holds
            if (c.index == d.E("OnLoad")) XtalResizeArrays(e.text, { 1 }, kServerRows + 1);
            menu.children.push_back(std::move(e));
        }
    frame.children.push_back(std::move(menu));
    const Node& valueTemplate = child("SelectValueMenuItemOnline020201");
    for (int i = 1; i <= kServerRows; ++i)
    {
        Node v = valueTemplate;
        d.Set(v, "name", ("SelectValueOnlineSrv" + two(i)).c_str());
        frame.children.push_back(std::move(v));
    }
    Node spare = child("SelectValueMenuItemOnline020206-voice_chat");
    d.Set(spare, "name", "SelectValueOnlineSrvSpare");
    frame.children.push_back(std::move(spare));
    const Node& descTemplate = child("StaticOnline0202002");
    for (int i = 1; i <= kServerRows + 1; ++i)
    {
        Node h = descTemplate;
        d.Set(h, "name", i <= kServerRows ? ("StaticOnlineSrv00" + std::to_string(i)).c_str() : "StaticOnlineSrvSpare");
        d.Set(h, "text", "-");
        frame.children.push_back(std::move(h));
    }
    for (const char* name : { "StaticIconOnline0202", "StaticTelopOnline0202" })
    {
        Node s = child(name);
        std::string n = name;
        n.replace(n.find("0202"), 4, "Srv");
        d.Set(s, "name", n.c_str());
        frame.children.push_back(std::move(s));
    }
    top.insert(top.begin() + std::ptrdiff_t(at) + 1, std::move(frame));
}

} // namespace

namespace { std::atomic<bool> g_pairPlayRow{ false }, g_rematchFrames{ false }, g_pairNamesFrame{ false }; }
bool MenuPairNamesFrame() { return g_pairNamesFrame.load(); }
bool MenuPairPlayRow() { return g_pairPlayRow.load(); }
bool MenuRematchFrames() { return g_rematchFrames.load(); }
const char* RematchHelp(int choice, bool host)
{
    if (choice == 5 && host) return kRematchHostLeaveHelp;
    return choice >= 0 && choice < 6 ? kRematchRows[choice].help : "";
}

std::string ApplyPcEdits(const uint8_t* b, size_t size, const PcLists& lists, std::string* why, std::string* note)
{
    try
    {
        if (size < 16 || std::string(reinterpret_cast<const char*>(b), 4) != "BX01") return {};
        const std::string text(reinterpret_cast<const char*>(b), size);
        const bool online = lists.serverRow && text.find("FrameOnlineMenu") != std::string::npos &&
                            text.find("MenuItemOnline07-leaderboard") != std::string::npos;
        if (!online && (text.find("FrameMode007-04-01") == std::string::npos || text.find("FrameMainMenu") == std::string::npos))
            return {};
        const uint32_t elemOff = Be32(b, size, 8), attrOff = Be32(b, size, 12);
        if (elemOff > size || attrOff > size || attrOff < elemOff) throw Fail("bad table offsets");
        Doc d;
        d.elem = NameTable(b, size, elemOff);
        d.attr = NameTable(b, size, attrOff);
        size_t p = 16;
        d.roots = ReadList(b, size, p, 0);
        if (p != elemOff) throw Fail("body does not end at the name tables");
        if (online)
        {
            AddServerRow(d);
            AddServerListFrame(d);
            AddFriendInvite(d);
            try { AddRematchRow(d); }
            catch (const Fail& e) { if (note) *note = std::string("the room's REMATCH MENU row left out: ") + e.what(); }
            try
            {
                AddRematchFrames(d);
                g_rematchFrames = true;
            }
            catch (const Fail& e) { if (note) *note += std::string(note->empty() ? "" : "; ") + "the rematch menu left out: " + e.what(); }
            try
            {
                AddPairNames(d);
                g_pairNamesFrame = true;
            }
            catch (const Fail& e) { if (note) *note += std::string(note->empty() ? "" : "; ") + "PAIR PLAY names left out: " + e.what(); }
            try
            {
                AddPairPlayRow(d);
                g_pairPlayRow = true;
            }
            catch (const Fail& e) { if (note) *note += std::string(note->empty() ? "" : "; ") + "PAIR PLAY row left out: " + e.what(); }
        }
        else Edit(d, lists, note);
        std::string body;
        WriteList(d.roots, body);
        std::string out(reinterpret_cast<const char*>(b), 8);
        const uint32_t newElem = uint32_t(16 + body.size());
        PutBe32(out, newElem);
        PutBe32(out, newElem + (attrOff - elemOff));
        out += body;
        out.append(reinterpret_cast<const char*>(b) + elemOff, size - elemOff);
        return out;
    }
    catch (const std::exception& e)
    {
        if (why) *why = e.what();
        return {};
    }
}

} // namespace menu_doc
