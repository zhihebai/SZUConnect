#pragma once

#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace szu::app {

struct TextLayoutFacts {
    UINT32 lines = 0;
    float width = 0;
    float height = 0;
    float layout_width = 0;
};

namespace text_layout_detail {

inline TextLayoutFacts facts(IDWriteTextLayout* layout) {
    TextLayoutFacts result;
    if (!layout) return result;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return result;
    result.lines = metrics.lineCount;
    result.width = metrics.widthIncludingTrailingWhitespace;
    result.height = metrics.height;
    result.layout_width = layout->GetMaxWidth();
    return result;
}

inline Microsoft::WRL::ComPtr<IDWriteTextLayout> create(IDWriteFactory* factory, IDWriteTextFormat* format,
                                                       std::wstring_view text, float width) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> result;
    if (factory && format && std::isfinite(width) && width > 0)
        factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, width, 100000.0f, result.GetAddressOf());
    return result;
}

}  // namespace text_layout_detail

// Measurement and painting share the same layout at the actual available width.
// The cache is local to one canvas and cleared whenever its font formats change.
class TextLayoutCache {
public:
    struct Result {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        TextLayoutFacts facts;
    };

    Result get(IDWriteFactory* factory, IDWriteTextFormat* format, std::wstring_view text, float width) {
        width = std::max(1.0f, width);
        for (auto& entry : entries_) {
            if (entry.format == format && entry.width == width && entry.text == text) {
                entry.used = ++clock_;
                return entry.result;
            }
        }
        Result result;
        result.layout = text_layout_detail::create(factory, format, text, width);
        result.facts = text_layout_detail::facts(result.layout.Get());
        if (text.size() <= 1024) {
            if (entries_.size() == kLimit) {
                const auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
                *oldest = Entry{format, width, std::wstring(text), ++clock_, result};
            } else entries_.push_back(Entry{format, width, std::wstring(text), ++clock_, result});
        }
        return result;
    }

    void clear() { entries_.clear(); clock_ = 0; }
    std::size_t size() const { return entries_.size(); }

private:
    static constexpr std::size_t kLimit = 64;
    struct Entry {
        IDWriteTextFormat* format;
        float width;
        std::wstring text;
        std::uint64_t used;
        Result result;
    };
    std::vector<Entry> entries_;
    std::uint64_t clock_ = 0;
};

}  // namespace szu::app
