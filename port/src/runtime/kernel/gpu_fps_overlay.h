// Options -> Display Settings -> FPS COUNTER: the panel the presenter draws over the finished picture (gpu_hw_d3d11.h,
// DrawFpsCounter). It follows the look of the game's own fight money box (Customize, FrameCustomizeItemEquip): a dark
// teal bevelled plate with a light lower edge, bold extended italic white digits and a gold unit. Nothing here comes from
// the game's files: the glyphs are strokes drawn from the skeletons below (ship no game data), at the window's size,
// and only the digits' cells are composed again when the number changes (twice a second).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fps_overlay {

// Premultiplied RGBA bytes, red in the low byte, top row first.
struct Bitmap { uint32_t w = 0, h = 0; std::vector<uint32_t> px; };

class Panel
{
public:
    // The panel's size for a picture `pictureHeight` pixels high (1: 126 x 36 pixels): small at every window size, 63 x 18
    // at 720p and below, where the digits are still clear.
    static float Scale(float pictureHeight) { return std::max(0.5f, pictureHeight / 1440.0f); }

    // The panel showing `fps` for a picture `pictureHeight` pixels high. The bitmap stays valid until the next call. Two
    // digits: the game never runs faster than its 60 fps.
    const Bitmap& Draw(int fps, uint32_t pictureHeight)
    {
        const float scale = Scale(float(pictureHeight));
        if (scale != scale_) Layout(scale);
        fps = std::clamp(fps, 0, 99);
        if (fps == shown_) return image_;
        shown_ = fps;
        image_.px = plate_;
        if (fps >= 10) Stamp(digitCell_[size_t(fps / 10)], digitX_, digitY_, 0xFFFFFFu, 0xC9D6DEu);
        Stamp(digitCell_[size_t(fps % 10)], digitX_ + digitAdvance_, digitY_, 0xFFFFFFu, 0xC9D6DEu);
        for (int i = 0; i < 3; ++i)
            Stamp(unitCell_[size_t(i)], unitX_ + float(i) * unitAdvance_, unitY_, 0xFFF3B4u, 0xB8933Au);
        return image_;
    }

private:
    struct Point { float x, y; };
    struct Cell { uint32_t w = 0, h = 0; std::vector<float> distance; };     // to the stroke's edge, in pixels; < 0 inside

    using Path = std::vector<Point>;
    using Glyph = std::vector<Path>;
    // Skeletons in a 1 x 1 box (y down). Ends on the box's sides run out of it and are cut flat, as the game's lettering.
    static const Glyph& Shape(int which)
    {
        static const std::vector<Glyph> shapes = {
            { { { 0.5f, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { 0, 0 }, { 0.5f, 0 } } },                              // 0
            { { { 0.20f, 0.30f }, { 0.60f, 0 }, { 0.60f, 1 } } },                                                  // 1
            { { { 0, 0 }, { 1, 0 }, { 1, 0.5f }, { 0, 0.5f }, { 0, 1 }, { 1, 1 } } },                              // 2
            { { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }, { { 0.24f, 0.5f }, { 1, 0.5f } } },                      // 3
            { { { 0.74f, 1 }, { 0.74f, 0 }, { 0, 0.66f }, { 1, 0.66f } } },                                        // 4
            { { { 1, 0 }, { 0, 0 }, { 0, 0.47f }, { 1, 0.47f }, { 1, 1 }, { 0, 1 } } },                            // 5
            { { { 1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0.5f }, { 0, 0.5f } } },                              // 6
            { { { 0, 0 }, { 1, 0 }, { 0.42f, 1 } } },                                                              // 7
            { { { 0.5f, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { 0, 0 }, { 0.5f, 0 } }, { { 0, 0.5f }, { 1, 0.5f } } },  // 8
            { { { 1, 0.5f }, { 0, 0.5f }, { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } } },                              // 9
            { { { 1, 0 }, { 0, 0 }, { 0, 1 } }, { { 0, 0.5f }, { 0.82f, 0.5f } } },                                // F
            { { { 0, 1 }, { 0, 0 }, { 1, 0 }, { 1, 0.56f }, { 0, 0.56f } } },                                      // P
            { { { 1, 0 }, { 0, 0 }, { 0, 0.47f }, { 1, 0.47f }, { 1, 1 }, { 0, 1 } } },                            // S
        };
        return shapes[size_t(which)];
    }
    // The skeleton in pixels with its corners rounded (arcs of short chords), its free ends run out past the box.
    static std::vector<Path> Rounded(const Glyph& glyph, float w, float h, float half, float corner)
    {
        std::vector<Path> out;
        for (const Path& source : glyph)
        {
            Path p;
            for (const Point& s : source) p.push_back({ half + s.x * (w - 2 * half), half + s.y * (h - 2 * half) });
            auto reach = [&](Point& end, const Point& next) {
                const float dx = end.x - next.x, dy = end.y - next.y, length = std::max(std::hypot(dx, dy), 0.001f);
                end.x += dx / length * half * 1.5f; end.y += dy / length * half * 1.5f;
            };
            const bool closed = source.front().x == source.back().x && source.front().y == source.back().y;
            if (!closed) { reach(p.front(), p[1]); reach(p.back(), p[p.size() - 2]); }
            Path round;
            round.push_back(p.front());
            for (size_t i = 1; i + 1 < p.size(); ++i)
            {
                const Point a = p[i - 1], b = p[i], c = p[i + 1];
                const float la = std::hypot(a.x - b.x, a.y - b.y), lc = std::hypot(c.x - b.x, c.y - b.y);
                const float r = std::min({ corner, la * 0.5f, lc * 0.5f });
                const Point from{ b.x + (a.x - b.x) / la * r, b.y + (a.y - b.y) / la * r };
                const Point to{ b.x + (c.x - b.x) / lc * r, b.y + (c.y - b.y) / lc * r };
                for (int k = 0; k <= 6; ++k)
                {
                    const float t = float(k) / 6.0f, u = 1.0f - t;
                    round.push_back({ u * u * from.x + 2 * u * t * b.x + t * t * to.x, u * u * from.y + 2 * u * t * b.y + t * t * to.y });
                }
            }
            round.push_back(p.back());
            out.push_back(std::move(round));
        }
        return out;
    }
    static float SegmentDistance(float px, float py, const Point& a, const Point& b)
    {
        const float dx = b.x - a.x, dy = b.y - a.y, dd = dx * dx + dy * dy;
        const float t = dd > 0 ? std::clamp(((px - a.x) * dx + (py - a.y) * dy) / dd, 0.0f, 1.0f) : 0.0f;
        return std::hypot(px - a.x - t * dx, py - a.y - t * dy);
    }
    // One glyph's distances, sheared to the game's italic, with `pad` pixels around it for the outline.
    static Cell MakeCell(int which, float w, float h, float pad)
    {
        const float half = h * 0.135f, shear = 0.26f;
        const std::vector<Path> paths = Rounded(Shape(which), w, h, half, h * 0.20f);
        Cell c;
        c.w = uint32_t(std::ceil(w + h * shear + pad * 2));
        c.h = uint32_t(std::ceil(h + pad * 2));
        c.distance.resize(size_t(c.w) * c.h);
        for (uint32_t y = 0; y < c.h; ++y)
            for (uint32_t x = 0; x < c.w; ++x)
            {
                const float gy = float(y) + 0.5f - pad;
                const float gx = float(x) + 0.5f - pad - (h - gy) * shear;      // back to the upright glyph
                float d = 1e9f;
                for (const Path& p : paths)
                    for (size_t i = 0; i + 1 < p.size(); ++i) d = std::min(d, SegmentDistance(gx, gy, p[i], p[i + 1]));
                const float box = std::max({ -gx, gx - w, -gy, gy - h });       // ends cut flat on the box's sides
                c.distance[size_t(y) * c.w + x] = std::max(d - half, box);
            }
        return c;
    }
    static float Mix(float a, float b, float t) { return a + (b - a) * t; }
    static float Cover(float distance) { return std::clamp(0.5f - distance, 0.0f, 1.0f); }
    static uint32_t Pack(float r, float g, float b, float a)        // premultiplied values 0..1
    {
        auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return byte(r) | byte(g) << 8 | byte(b) << 16 | byte(a) << 24;
    }
    void Layout(float scale)
    {
        scale_ = scale;
        shown_ = -1;
        const float digitH = 17.0f * scale, digitW = 21.0f * scale, unitH = 11.0f * scale, unitW = 12.5f * scale;
        outline_ = 1.3f * scale;
        pad_ = std::ceil(outline_) + 1.0f;
        digitAdvance_ = digitW + 3.0f * scale;
        unitAdvance_ = unitW + 2.5f * scale;
        const float w = std::round(126.0f * scale), h = std::round(36.0f * scale);
        digitX_ = 13.0f * scale; digitY_ = std::round((h - digitH) * 0.5f - 0.5f * scale);
        unitX_ = digitX_ + digitAdvance_ * 2.0f + 2.0f * scale; unitY_ = digitY_ + digitH - unitH;
        for (int i = 0; i < 10; ++i) digitCell_[size_t(i)] = MakeCell(i, digitW, digitH, pad_);
        for (int i = 0; i < 3; ++i) unitCell_[size_t(i)] = MakeCell(10 + i, unitW, unitH, pad_);
        image_.w = uint32_t(w); image_.h = uint32_t(h);
        plate_.assign(size_t(image_.w) * image_.h, 0);
        // The plate: an octagon, dark edge, teal running lighter down to a pale lower rim, a bright line under the top.
        const float cx = w * 0.5f, cy = h * 0.5f, halfW = cx - 1.0f, halfH = cy - 1.0f, chamfer = 7.0f * scale;
        const auto plate = [&](float px, float py, float inset) {
            const float qx = std::fabs(px - cx), qy = std::fabs(py - cy);
            return std::max({ qx - (halfW - inset), qy - (halfH - inset), (qx + qy - (halfW + halfH - chamfer - inset * 1.2f)) * 0.7071f });
        };
        for (uint32_t y = 0; y < image_.h; ++y)
            for (uint32_t x = 0; x < image_.w; ++x)
            {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f, t = py / h;
                const float outer = Cover(plate(px, py, 0.0f)), inner = Cover(plate(px, py, 1.5f * scale));
                const float rim = Cover(plate(px, py, 3.0f * scale));
                const float fade = std::pow(std::clamp((t - 0.2f) / 0.8f, 0.0f, 1.0f), 2.2f);
                float r = Mix(0.035f, 0.13f, fade), g = Mix(0.12f, 0.42f, fade), b = Mix(0.17f, 0.56f, fade);
                const float pale = (1.0f - rim) * std::clamp((t - 0.5f) * 2.0f, 0.0f, 1.0f);    // the lower rim
                r = Mix(r, 0.56f, pale); g = Mix(g, 0.80f, pale); b = Mix(b, 0.92f, pale);
                const float line = (1.0f - rim) * std::clamp((0.5f - t) * 2.0f, 0.0f, 1.0f);    // the upper rim
                r = Mix(r, 0.14f, line); g = Mix(g, 0.52f, line); b = Mix(b, 0.66f, line);
                r *= inner; g *= inner; b *= inner;                                             // the dark edge around it
                const float a = outer * 0.94f;
                plate_[size_t(y) * image_.w + x] = Pack(r * a, g * a, b * a, a);
            }
        image_.px = plate_;
    }
    // A glyph over the picture: its dark outline, then the fill, `top` to `bottom` colours (0xRRGGBB) down the cell.
    void Stamp(const Cell& cell, float atX, float atY, uint32_t top, uint32_t bottom)
    {
        const int x0 = int(std::round(atX)) - int(pad_), y0 = int(std::round(atY)) - int(pad_);
        auto channel = [](uint32_t rgb, int shift) { return float((rgb >> shift) & 0xFF) / 255.0f; };
        for (uint32_t y = 0; y < cell.h; ++y)
        {
            const int py = y0 + int(y);
            if (py < 0 || py >= int(image_.h)) continue;
            const float t = float(y) / float(std::max(cell.h - 1, 1u));
            const float r = Mix(channel(top, 16), channel(bottom, 16), t), g = Mix(channel(top, 8), channel(bottom, 8), t),
                        b = Mix(channel(top, 0), channel(bottom, 0), t);
            for (uint32_t x = 0; x < cell.w; ++x)
            {
                const int px = x0 + int(x);
                if (px < 0 || px >= int(image_.w)) continue;
                const float d = cell.distance[size_t(y) * cell.w + x];
                const float fill = Cover(d), edge = Cover(d - outline_);
                if (edge <= 0.0f) continue;
                uint32_t& out = image_.px[size_t(py) * image_.w + uint32_t(px)];
                float dr = float(out & 0xFF) / 255.0f, dg = float((out >> 8) & 0xFF) / 255.0f, db = float((out >> 16) & 0xFF) / 255.0f,
                      da = float(out >> 24) / 255.0f;
                // the outline: near black
                dr = Mix(dr, 0.02f, edge); dg = Mix(dg, 0.03f, edge); db = Mix(db, 0.04f, edge); da = Mix(da, 1.0f, edge);
                dr = Mix(dr, r, fill); dg = Mix(dg, g, fill); db = Mix(db, b, fill);
                out = Pack(dr, dg, db, da);
            }
        }
    }

    float scale_ = 0, digitAdvance_ = 0, unitAdvance_ = 0, digitX_ = 0, digitY_ = 0, unitX_ = 0, unitY_ = 0, outline_ = 1, pad_ = 2;
    int shown_ = -1;
    Cell digitCell_[10], unitCell_[3];
    std::vector<uint32_t> plate_;
    Bitmap image_;
};

} // namespace fps_overlay
