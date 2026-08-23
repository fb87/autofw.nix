#pragma once

// Minimal host-side IME for the Flutter `flutter/textinput` channel.
//
// Flutter's `SystemChannels.textInput` uses a JSON method codec. To let a
// focused TextField receive typed characters on a hardware keyboard we:
//   * parse the inbound JSON method calls (TextInput.setClient / setEditingState
//     / show / hide / clearClient) far enough to track the client + editing
//     value and to ACK them;
//   * turn raw key events into `TextInput.updateEditingState` JSON pushed back
//     to Flutter.
//
// Editing is deliberately simple (single cursor, no composing): printable
// characters are inserted at the caret, backspace deletes before it, and the
// caret then sits at the end of the inserted run.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace shell::textinput
{

// ---- minimal JSON parser (object/array primitives only) --------------------
// Tokenises a JSON document into a small value tree: object, array, string,
// number, bool, null. Enough to interpret Flutter's method-call envelopes.
struct value;
using value_ptr = std::unique_ptr<value>;

enum class kind
{
    null_,
    boolean,
    number,
    string_,
    object,
    array,
};

struct value
{
    kind k{kind::null_};
    bool b{};
    double num{};
    std::string str;
    std::vector<std::pair<std::string, std::unique_ptr<value>>> obj;
    std::vector<std::unique_ptr<value>> arr;

    const value* get(const char* key) const
    {
        if (k != kind::object)
            return nullptr;
        for (auto& [k, v] : obj)
            if (k == key)
                return v.get();
        return nullptr;
    }
    const value* at(std::size_t i) const
    {
        return (k == kind::array && i < arr.size()) ? arr[i].get() : nullptr;
    }
};

namespace parse_detail
{
inline void skip_ws(const char*& p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
}

inline bool parse_string(const char*& p, std::string& out)
{
    if (*p != '"')
        return false;
    ++p;
    out.clear();
    while (*p && *p != '"')
    {
        if (*p == '\\')
        {
            ++p;
            switch (*p)
            {
            case 'n': out += '\n'; ++p; break;
            case 't': out += '\t'; ++p; break;
            case 'r': out += '\r'; ++p; break;
            case 'u':
            {
                // Keep escape escapes simple; skip 4 hex digits.
                out += '#';
                p += 1;
                while (*p && *p != '"' && *p != '\\' && std::strspn(p, "0123456789abcdefABCDEF") < 4 && *p) ++p;
                if (*p) ++p;
                break;
            }
            default: out += *p; ++p; break;
            }
        }
        else
        {
            out += *p++;
        }
    }
    if (*p != '"')
        return false;
    ++p;
    return true;
}

inline value* parse_value(const char*& p);

inline bool parse_array(const char*& p, value& v)
{
    if (*p != '[')
        return false;
    ++p;
    v.k = kind::array;
    skip_ws(p);
    if (*p == ']')
    {
        ++p;
        return true;
    }
    while (true)
    {
        skip_ws(p);
        auto* item = parse_value(p);
        if (!item)
            return false;
        v.arr.push_back(std::unique_ptr<value>(item));
        skip_ws(p);
        if (*p == ',')
            ++p;
        else if (*p == ']')
        {
            ++p;
            return true;
        }
        else
            return false;
    }
}

inline bool parse_object(const char*& p, value& v)
{
    if (*p != '{')
        return false;
    ++p;
    v.k = kind::object;
    skip_ws(p);
    if (*p == '}')
    {
        ++p;
        return true;
    }
    while (true)
    {
        skip_ws(p);
        std::string key;
        if (!parse_string(p, key))
            return false;
        skip_ws(p);
        if (*p != ':')
            return false;
        ++p;
        skip_ws(p);
        auto* val = parse_value(p);
        if (!val)
            return false;
        v.obj.emplace_back(std::move(key), std::unique_ptr<value>(val));
        skip_ws(p);
        if (*p == ',')
            ++p;
        else if (*p == '}')
        {
            ++p;
            return true;
        }
        else
            return false;
    }
}

inline value* parse_value(const char*& p)
{
    skip_ws(p);
    auto* v = new value{};
    if (*p == '{')
    {
        if (!parse_object(p, *v))
        {
            delete v;
            return nullptr;
        }
    }
    else if (*p == '[')
    {
        if (!parse_array(p, *v))
        {
            delete v;
            return nullptr;
        }
    }
    else if (*p == '"')
    {
        v->k = kind::string_;
        if (!parse_string(p, v->str))
        {
            delete v;
            return nullptr;
        }
    }
    else if (std::strncmp(p, "true", 4) == 0)
    {
        v->k = kind::boolean; v->b = true; p += 4;
    }
    else if (std::strncmp(p, "false", 5) == 0)
    {
        v->k = kind::boolean; v->b = false; p += 5;
    }
    else if (std::strncmp(p, "null", 4) == 0)
    {
        v->k = kind::null_; p += 4;
    }
    else if (*p == '-' || (*p >= '0' && *p <= '9'))
    {
        v->k = kind::number;
        v->num = std::strtod(p, (char**)(&p));
    }
    else
    {
        delete v;
        return nullptr;
    }
    return v;
}
} // namespace parse_detail

inline void parse(std::string_view json, value& out)
{
    const char* p = json.data();
    auto* v = parse_detail::parse_value(p);
    if (v)
    {
        out = std::move(*v);
        delete v;
    }
}

// Minimal JSON emitter for the payloads we must build.
inline auto quote(std::string_view s) -> std::string
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

// ---- editing state + behaviour --------------------------------------------
struct state
{
    std::int64_t client{-1};
    std::string text;
    std::int64_t selection_base{-1};
    std::int64_t selection_extent{-1};
    std::int64_t composing_base{-1};   // -1 == no active composing run
    std::int64_t composing_extent{-1};
    bool connected{};
    // Client configuration (from TextInput.setClient args[1]).
    bool multiline{};                  // inputType == TextInputType.multiline
    bool obscured{};                   // inputType == TextInputType.password
    std::string keyboard_type{"text"};
    std::string text_input_action{"done"};
    // True when we pushed an updateEditingState that Flutter hasn't answered
    // with TextInput.setEditingState (so we don't fight Flutter's own state).
    bool dirty{};
    // Action requested by the last control key (Enter) for a single-line field;
    // the engine performs it with TextInputClient.performAction.
    bool action_pending{};
    // Guards all editing state; the engine must hold it while calling into the
    // editing helpers (on_platform_message and send_key run on different
    // threads).
    std::mutex mutex;
};

// ACK response for a handled JSON method call. JSONMethodCodec.decodeEnvelope
// treats a one-element List as a successful result, so the ACK must be a List
// (e.g. [null]), not the bare JSON null.
inline auto ack() -> std::vector<std::uint8_t>
{
    const char* n = "[null]";
    return {n, n + 6};
}

// Build the JSON payload for TextInputClient.updateEditingState.
inline auto update_editing_state(const state& s) -> std::string
{
    if (s.client < 0)
        return {};
    std::string j;
    j = "{\"method\":\"TextInputClient.updateEditingState\",\"args\":[";
    j += std::to_string(s.client);
    j += ",{\"text\":";
    j += quote(s.text);
    std::int64_t sel = s.selection_base < 0 ? (std::int64_t)s.text.size() : s.selection_base;
    std::int64_t ext = s.selection_extent < 0 ? sel : s.selection_extent;
    j += ",\"selectionBase\":" + std::to_string(sel);
    j += ",\"selectionExtent\":" + std::to_string(ext);
    j += ",\"selectionAffinity\":\"TextAffinity.downstream\"";
    j += ",\"selectionIsDirectional\":false";
    if (s.composing_base >= 0 && s.composing_extent >= s.composing_base)
    {
        j += ",\"composingBase\":" + std::to_string(s.composing_base);
        j += ",\"composingExtent\":" + std::to_string(s.composing_extent);
    }
    else
    {
        j += ",\"composingBase\":-1,\"composingExtent\":-1";
    }
    j += "}]}";
    return j;
}

// Handle an inbound TextInput.* method call. Returns the response bytes (an ACK)
// and mutates |s|.
inline auto handle(state& s, std::string_view json) -> std::vector<std::uint8_t>
{
    value root;
    parse(json, root);
    const value* method = root.get("method");
    if (!method || method->k != kind::string_)
        return {};

    const std::string& m = method->str;
    const value* args = root.get("args");

    if (m == "TextInput.setClient")
    {
        // args = [clientId, configuration]
        if (args && args->at(0) && args->at(0)->k == kind::number)
        {
            s.client = static_cast<std::int64_t>(args->at(0)->num);
            s.connected = true;
            const value* cfg = args->at(1);
            const value* act = cfg ? cfg->get("inputAction") : nullptr;
            if (act && act->k == kind::string_ && !act->str.empty())
                s.text_input_action = act->str;
            // Determine single/multi-line and ObscuringText/read-only from the
            // inputType map (or the literal keyboardType string on some hosts).
            s.multiline = false;
            s.obscured = false;
            if (cfg)
            {
                const value* ty = cfg->get("inputType");
                if (ty && ty->k == kind::object)
                {
                    const value* kt = ty->get("name");
                    if (kt && kt->k == kind::string_)
                        s.keyboard_type = kt->str;
                    if (kt && kt->k == kind::string_)
                    {
                        s.multiline = (kt->str == "TextInputType.multiline");
                        s.obscured = (kt->str == "TextInputType.password");
                    }
                }
                else if (ty && ty->k == kind::string_)
                {
                    s.keyboard_type = ty->str;
                    s.multiline = (ty->str == "TextInputType.multiline");
                    s.obscured = (ty->str == "TextInputType.password");
                }
                const value* ob = cfg->get("obscureText");
                if (ob && ob->k == kind::boolean)
                    s.obscured = ob->b;
            }
        }
    }
    else if (m == "TextInput.setEditingState")
    {
        if (args && args->k == kind::object)
        {
            const value* t = args->get("text");
            if (t && t->k == kind::string_)
                s.text = t->str;
            const value* b = args->get("selectionBase");
            const value* e = args->get("selectionExtent");
            const value* cb = args->get("composingBase");
            const value* ce = args->get("composingExtent");
            if (b && b->k == kind::number)
                s.selection_base = static_cast<std::int64_t>(b->num);
            if (e && e->k == kind::number)
                s.selection_extent = static_cast<std::int64_t>(e->num);
            if (cb && cb->k == kind::number)
                s.composing_base = static_cast<std::int64_t>(cb->num);
            if (ce && ce->k == kind::number)
                s.composing_extent = static_cast<std::int64_t>(ce->num);
        }
    }
    else if (m == "TextInput.clearClient" || m == "TextInput.closeConnection")
    {
        s.client = -1;
        s.connected = false;
        s.text.clear();
        s.selection_base = s.selection_extent = 0;
        s.composing_base = s.composing_extent = -1;
        s.action_pending = false;
    }
    // show / hide / others: ack only.
    s.dirty = false;
    return ack();
}

// Apply a printable run (what a key-down for a printable produced) at the caret.
inline auto insert_characters(state& s, std::string_view run) -> void
{
    if (!s.connected)
        return;
    std::size_t at = s.selection_base < 0 ? s.text.size()
                                          : static_cast<std::size_t>(s.selection_base);
    if (at > s.text.size())
        at = s.text.size();
    // Replace any selection with the typed run.
    std::size_t extent = s.selection_extent >= 0
                             ? static_cast<std::size_t>(std::max<std::int64_t>(s.selection_extent, 0))
                             : at;
    if (extent > s.text.size())
        extent = s.text.size();
    std::size_t lo = std::min(at, extent);
    std::size_t hi = std::max(at, extent);
    s.text.replace(lo, hi - lo, run);
    std::size_t caret = lo + run.size();
    s.selection_base = static_cast<std::int64_t>(caret);
    s.selection_extent = static_cast<std::int64_t>(caret);
    s.dirty = true;
}

inline auto backspace(state& s) -> void
{
    if (!s.connected)
        return;
    std::size_t caret = s.selection_base < 0 ? s.text.size()
                                             : static_cast<std::size_t>(s.selection_base);
    std::size_t extent = s.selection_extent >= 0
                             ? static_cast<std::size_t>(std::max<std::int64_t>(s.selection_extent, 0))
                             : caret;
    if (caret > s.text.size())
        caret = s.text.size();
    if (extent > s.text.size())
        extent = s.text.size();
    std::size_t lo = std::min(caret, extent);
    std::size_t hi = std::max(caret, extent);

    if (hi > lo)
    {
        // A selection is active: backspace deletes the whole selected run.
        s.text.erase(lo, hi - lo);
        caret = lo;
    }
    else if (lo > 0)
    {
        // Collapsed caret: delete the single character before it.
        s.text.erase(lo - 1, 1);
        caret = lo - 1;
    }
    else
    {
        return;
    }
    s.selection_base = s.selection_extent = static_cast<std::int64_t>(caret);
    s.dirty = true;
}

// A control key (Enter, Tab) when the field is single-line: record that the
// engine should ask Flutter to perform the client's action (done/go/next...).
inline auto request_action(state& s) -> void
{
    if (!s.connected)
        return;
    s.action_pending = true;
}

// Move the caret without changing the text (framework key routing is not
// available in this embedder, so the IME owns navigation).
inline auto move_caret(state& s, int delta, bool to_home) -> void
{
    if (!s.connected)
        return;
    std::size_t at = s.selection_base < 0 ? s.text.size()
                                          : static_cast<std::size_t>(s.selection_base);
    if (at > s.text.size())
        at = s.text.size();
    std::size_t caret = at;
    if (to_home)
    {
        caret = delta <= -2 ? s.text.size() : 0; // delta<=-2 signals END (go to end)
    }
    else if (delta < 0)
        caret = at == 0 ? 0 : at - 1;
    else if (delta > 0)
        caret = at >= s.text.size() ? s.text.size() : at + 1;
    s.selection_base = s.selection_extent = static_cast<std::int64_t>(caret);
    s.dirty = true;
}

// Build the JSON payload for TextInputClient.performAction.
inline auto perform_action_json(const state& s) -> std::string
{
    if (s.client < 0)
        return {};
    std::string j;
    j = "{\"method\":\"TextInputClient.performAction\",\"args\":[";
    j += std::to_string(s.client);
    j += ",";
    j += quote(s.text_input_action.empty() ? "done" : s.text_input_action);
    j += "]}";
    return j;
}

inline auto forward_delete(state& s) -> void
{
    if (!s.connected)
        return;
    std::size_t caret = s.selection_base < 0 ? s.text.size()
                                             : static_cast<std::size_t>(s.selection_base);
    std::size_t extent = s.selection_extent >= 0
                             ? static_cast<std::size_t>(std::max<std::int64_t>(s.selection_extent, 0))
                             : caret;
    if (caret > s.text.size())
        caret = s.text.size();
    if (extent > s.text.size())
        extent = s.text.size();
    std::size_t lo = std::min(caret, extent);
    std::size_t hi = std::max(caret, extent);

    if (hi > lo)
    {
        s.text.erase(lo, hi - lo); // remove the selection
        caret = lo;
    }
    else if (lo < s.text.size())
    {
        s.text.erase(lo, 1); // remove the character after the caret
    }
    else
    {
        return;
    }
    s.selection_base = s.selection_extent = static_cast<std::int64_t>(caret);
    s.dirty = true;
}

} // namespace shell::textinput
