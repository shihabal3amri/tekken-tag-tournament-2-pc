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
#include <cstring>
#include <initializer_list>
#include <map>
#include <stdexcept>

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
            if (k == 0) d.Set(item, "default", "true");
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

// ---- Options -> Display Settings: VSYNC, SHARPNESS, ANTI-ALIASING, TEXTURE FILTERING, SHADOWS after RESOLUTION
// (display_settings.h; the presenter, the texture samplers and the renderer apply them as the cursor moves).
// Clones of 3D DEPTH like DISPLAY MODE / RESOLUTION, natives renamed to PC ones of the same lengths
// (menu_ext.cpp registers them); the page's icon arrays grow to 12 rows; help texts go by position
// (rows 7..11, DEFAULT's becomes "StaticMode007-04-012").
void AddDisplayImage(Doc& d)
{
    const struct { const char* n; const char* label; const char* preview; const char* store; const char* help;
                   std::vector<std::string> values; } rows[] = {
        { "11", "VSYNC", "MenuOptionDisplayVSyncOnOff", "DisplaySettingSetVSyncOn",
          "OFF: less input lag, the picture may tear. Still 60 fps.",
          { "ON", "OFF" } },
        { "08", "SHARPNESS", "MenuOptionDisplaySharpening", "DisplaySettingSetSharpen",
          "Sharpen the picture (contrast adaptive sharpening).",
          { "OFF", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" } },
        { "09", "ANTI-ALIASING", "MenuOptionDisplayEdgeSmooth", "DisplaySettingSetEdgesAA",
          "Smooth jagged edges: FXAA or SMAA 1x (high preset).",
          { "OFF", "FXAA", "SMAA 1X" } },
        { "10", "TEXTURE FILTERING", "MenuOptionDisplayTexFilters", "DisplaySettingSetTexFilt",
          "Anisotropic filtering keeps textures sharp at an angle.",
          { "STANDARD", "ANISOTROPIC 2X", "ANISOTROPIC 4X", "ANISOTROPIC 8X", "ANISOTROPIC 16X" } },
        { "12", "SHADOWS", "MenuOptionDisplayShadowsOff", "DisplaySettingSetShadows",
          "OFF: fighters and stage objects cast no shadows. Lighter on the GPU.",
          { "ON", "OFF" } },
    };
    const Node depth = d.Find("MenuItemMode007-04-04");
    const Node svTemplate = d.Find("SelectValueMenuItemMode007-04-02");
    const uint32_t itemIndex = d.E("selectvalueitem");
    std::vector<Node> items, lists;
    for (const auto& r : rows)
    {
        const std::string sv = std::string("SelectValueMenuItemMode007-04-") + r.n;
        Node row = depth;
        d.Set(row, "name", (std::string("MenuItemMode007-04-") + r.n).c_str());
        d.Set(row, "text", r.label);
        d.Set(row, "textid", nullptr);
        d.Set(row, "value", sv.c_str());
        std::string& trig = d.Event(row, "OnTrigger").text;
        XtalRename(trig, "DisplaySettingIsMonitor3DCapable", "DisplaySettingIsPCDisplayCapable");
        XtalRename(trig, "MenuOptionDisplay3DPallarax", r.preview);
        XtalRename(trig, "DisplaySettingSet3DDepth", r.store);
        XtalRename(trig, "SelectValueMenuItemMode007-04-04", sv);
        items.push_back(std::move(row));
        Node list = svTemplate;
        d.Set(list, "name", sv.c_str());
        list.children.clear();
        for (size_t k = 0; k < r.values.size(); ++k)
        {
            Node item;
            item.index = itemIndex;
            item.text = r.values[k] + '\0';
            if (k == 0) d.Set(item, "default", "true");
            list.children.push_back(std::move(item));
        }
        lists.push_back(std::move(list));
    }
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
    XtalExtendArrays(d.Event(d.Find("MenuMode007-04-01"), "OnLoad").text, { 1, 2 }, 12);
    // 3D MODE / 3D DEPTH (stereoscopic 3D: never on a PC, the rows stayed greyed out) leave the page so 10 rows fit
    // above the info bar (12 overlapped it). The page's scripts look them up by name, so they stay in the document,
    // out of the automatic layout and hidden (row, value, help text); the cursor already skipped them. MEASURED: only
    // hiding them left their help texts and frames in place.
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
    d.Set(list[at], "name", "StaticMode007-04-012");
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
        if (online) { AddServerRow(d); AddServerListFrame(d); AddFriendInvite(d); }
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
