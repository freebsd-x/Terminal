// Terminal.xapp — tmux-style terminal multiplexer for FreeBSD-X
//
// Layout:
//   Tab bar (32px): [session-1 ×] [session-2 ×] ... [+ New] | [⊟ H] [⊞ V] [✕]
//   Pane area (flex): one or two TerminalWidget panes (H or V split)
//
// Features:
//   • Each visible pane is attached to a TerminalService session (PTY in compositor)
//   • Sessions survive Terminal.xapp closing: DetachSession keeps PTY alive
//   • On restart: ListSessions → reattach to existing sessions
//   • Tab bar: one tab per known session; click to focus primary pane
//   • Horizontal split (⊟): primary left, secondary right
//   • Vertical split (⊞): primary top, secondary bottom
//   • Ctrl+Shift+C/V: clipboard copy/paste (preserved from original)
//   • Ctrl+Shift+N: new session
//   • Ctrl+Shift+W: close / detach current pane session
//   • Session rename: double-click tab (TextInputWidget overlay)

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/render_node.h"
#include "xkit/xbus_client.h"
#include "xkit/clipboard.h"
#include "shared/protocol.h"
#include "shared/logger.h"
#include <iostream>
#include <vector>
#include <string>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <csignal>

using namespace fbx::xkit;
using namespace fbx::shared;

// ── Session model ─────────────────────────────────────────────────────────────

struct SessionInfo {
    uint32_t    id{0};
    std::string name;
    bool        attached{false};   // currently attached in a pane
};

// ── Global state ──────────────────────────────────────────────────────────────

static XBusClient*            g_xbus          = nullptr;
static std::vector<SessionInfo> g_sessions;
static uint32_t               g_primary_id    = 0;    // session in pane 0
static uint32_t               g_secondary_id  = 0;    // session in pane 1 (or 0 = no split)

// Rebuild hooks (set after widget creation in main)
static std::function<void()>  g_rebuild_tabs;
static std::function<void()>  g_rebuild_panes;

enum class SplitMode { Single, Horizontal, Vertical };
static SplitMode g_split = SplitMode::Single;

// Split position (fraction given to pane 0) and live references to the two panes
// so a splitter drag can adjust their flex-grow in place — without rebuilding the
// pane tree, which would detach the splitter mid-drag and drop its capture.
static float g_split_ratio = 0.5f;
static std::shared_ptr<fbx::xkit::Widget> g_pane0_w, g_pane1_w;

// ── XBus helpers ──────────────────────────────────────────────────────────────

[[maybe_unused]] static void xbus_call(uint32_t opcode, const void* payload, size_t size,
                       std::function<void(const XBusMessage&)> cb = nullptr) {
    if (!g_xbus) return;
    XBusMessage msg{};
    msg.magic_ = XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.Terminal",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = opcode;
    if (payload && size)
        std::memcpy(msg.payload_, payload, std::min(size, sizeof(msg.payload_)));
    g_xbus->Call("com.freebsdx.Terminal", opcode,
                 {reinterpret_cast<const uint8_t*>(payload ? payload : msg.payload_),
                  payload ? size : 0},
                 cb ? [cb](const XBusMessage& r){ cb(r); } : XBusClient::ResponseCallback{});
}

// Create a new session via TerminalService, then call done_cb(id, name).
static void createSession(const std::string& name, uint16_t cols, uint16_t rows,
                           std::function<void(uint32_t, std::string)> done_cb) {
    if (!g_xbus) { done_cb(0, ""); return; }
    TerminalCreateRequest req{};
    std::strncpy(req.name_, name.c_str(), sizeof(req.name_) - 1);
    req.cols_ = cols ? cols : 80;
    req.rows_ = rows ? rows : 24;
    g_xbus->Call("com.freebsdx.Terminal",
                 static_cast<uint32_t>(TerminalOpcode::CreateSession),
                 {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                 [done_cb](const XBusMessage& resp) {
                     TerminalSessionReady ready{};
                     std::memcpy(&ready, resp.payload_, sizeof(ready));
                     ready.name_[sizeof(ready.name_) - 1] = '\0';
                     done_cb(ready.id_, ready.name_);
                 });
}

static void attachSession(uint32_t id) {
    if (!g_xbus || !id) return;
    TerminalSessionIdRequest req{};
    req.session_id_ = id;
    g_xbus->Call("com.freebsdx.Terminal",
                 static_cast<uint32_t>(TerminalOpcode::AttachSession),
                 {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                 nullptr);
}

static void detachSession(uint32_t id) {
    if (!g_xbus || !id) return;
    TerminalSessionIdRequest req{};
    req.session_id_ = id;
    g_xbus->Call("com.freebsdx.Terminal",
                 static_cast<uint32_t>(TerminalOpcode::DetachSession),
                 {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                 nullptr);
}

[[maybe_unused]] static void destroySession(uint32_t id) {
    if (!g_xbus || !id) return;
    TerminalSessionIdRequest req{};
    req.session_id_ = id;
    g_xbus->Call("com.freebsdx.Terminal",
                 static_cast<uint32_t>(TerminalOpcode::DestroySession),
                 {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                 nullptr);
}

static void sendInput(uint32_t session_id, const char* data, size_t n) {
    if (!g_xbus || !session_id || n == 0) return;
    // Break into ≤248-byte chunks
    while (n > 0) {
        size_t chunk_sz = std::min(n, sizeof(TerminalDataChunk::data_));
        TerminalDataChunk chunk{};
        chunk.session_id_ = session_id;
        chunk.size_       = static_cast<uint32_t>(chunk_sz);
        std::memcpy(chunk.data_, data, chunk_sz);
        g_xbus->Call("com.freebsdx.Terminal",
                     static_cast<uint32_t>(TerminalOpcode::SendInput),
                     {reinterpret_cast<const uint8_t*>(&chunk), sizeof(chunk)},
                     nullptr);
        data += chunk_sz;
        n    -= chunk_sz;
    }
}

static void renameSession(uint32_t id, const std::string& name) {
    if (!g_xbus || !id) return;
    TerminalRenameRequest req{};
    req.session_id_ = id;
    std::strncpy(req.name_, name.c_str(), sizeof(req.name_) - 1);
    g_xbus->Call("com.freebsdx.Terminal",
                 static_cast<uint32_t>(TerminalOpcode::RenameSession),
                 {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                 nullptr);
}

// ── TerminalWidget ────────────────────────────────────────────────────────────
// Dumb display: accumulates text from OutputChunk broadcasts, sends
// key input as SendInput XBus calls.  No embedded PTY.

class TerminalWidget : public Widget {
public:
    static constexpr uint16_t kCols    = 80;
    static constexpr uint16_t kRows    = 24;
    static constexpr size_t   kMaxLines = 500;

    explicit TerminalWidget(uint32_t session_id) noexcept
        : session_id_(session_id) {}

    uint32_t sessionId() const noexcept { return session_id_; }

    void appendOutput(const uint8_t* data, size_t n) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < n; ++i) {
            char c = static_cast<char>(data[i]);
            if (c == '\n') {
                lines_.push_back(std::move(current_line_));
                current_line_.clear();
                if (lines_.size() > kMaxLines)
                    lines_.pop_front();
            } else {
                current_line_ += c;
            }
        }
    }

    std::string getOutput() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string raw;
        raw.reserve(lines_.size() * 42);
        for (const auto& line : lines_) { raw += line; raw += '\n'; }
        if (!current_line_.empty()) raw += current_line_;
        return stripAnsi(raw);
    }

    void sendKey(char c) noexcept {
        sendInput(session_id_, &c, 1);
    }
    void sendSeq(const char* s, size_t n) noexcept {
        sendInput(session_id_, s, n);
    }

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

private:
    static std::string stripAnsi(const std::string& s) noexcept {
        std::string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '\x1b') {
                ++i;
                if (i < s.size() && s[i] == '[') {
                    ++i;
                    while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
                    if (i < s.size()) ++i;
                } else if (i < s.size() && s[i] == ']') {
                    ++i;
                    while (i < s.size() && s[i] != '\x07' && s[i] != '\x1b') ++i;
                    if (i < s.size()) ++i;
                } else {
                    if (i < s.size()) ++i;
                }
                continue;
            }
            char c = s[i++];
            if (c == '\n' || c == '\r' || (c >= 32 && c != 127))
                out += c;
        }
        return out;
    }

    uint32_t session_id_;
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    std::string current_line_;
};

// The visible terminal text: the newest lines that fit the pane, one RenderText
// per line, top to bottom. (A single RenderText drew the whole scrollback on
// one line — it doesn't break at '\n'.)
class RenderTermLines : public RenderNode {
public:
    static constexpr float kFontSize = 13.0f;
    static constexpr float kLineH    = 17.0f;
    static constexpr float kPad      = 6.0f;

    void setLines(std::vector<std::string> lines) noexcept {
        if (lines != lines_) { lines_ = std::move(lines); rebuild_ = true; }
    }

    [[nodiscard]] Size performLayout(const Constraints& c) noexcept override {
        const float w = c.max_width_;
        const float h = c.max_height_ < 50000.0f ? c.max_height_
                                                 : kPad * 2 + kLineH * static_cast<float>(lines_.size());
        const size_t fit = std::max<size_t>(1, static_cast<size_t>((h - kPad * 2) / kLineH));
        const size_t first = lines_.size() > fit ? lines_.size() - fit : 0;
        if (rebuild_ || first != first_) {
            clearChildren();
            for (size_t i = first; i < lines_.size(); ++i)
                addChild(std::make_shared<RenderText>(lines_[i], kFontSize));
            first_ = first;
            rebuild_ = false;
        }
        float y = kPad;
        for (const auto& line : children()) {
            (void)line->layout({0.0f, std::max(0.0f, w - kPad * 2), 0.0f, kLineH});
            line->setOffset({kPad, y});
            y += kLineH;
        }
        return c.clamp({w, h});
    }
    void paintSelf(fbx::xrender::DisplayList&, Offset) noexcept override {}

private:
    std::vector<std::string> lines_;
    size_t first_ = 0;
    bool   rebuild_ = true;
};

// The screen lines of terminal output: split at '\n'; within a line a '\r'
// returns to the start, so only the text after the last one with content
// shows (the shell redraws its prompt that way).
static std::vector<std::string> screenLines(const std::string& out) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= out.size()) {
        size_t nl = out.find('\n', start);
        if (nl == std::string::npos) nl = out.size();
        std::string line = out.substr(start, nl - start);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (auto cr = line.rfind('\r'); cr != std::string::npos) line = line.substr(cr + 1);
        lines.push_back(std::move(line));
        start = nl + 1;
    }
    return lines;
}

class TerminalElement : public Element {
public:
    explicit TerminalElement(std::shared_ptr<TerminalWidget> widget)
    {
        widget_ = std::move(widget);
        lines_node_ = std::make_shared<RenderTermLines>();
        // The terminal grid is an editable text surface — show the I-beam.
        lines_node_->setCursor(CursorShape::Text);
        renderNode_ = std::static_pointer_cast<RenderNode>(lines_node_);
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = newWidget;
    }
    void unmount() noexcept override { parent_.reset(); }

    void syncState() noexcept override {
        auto term = std::static_pointer_cast<TerminalWidget>(widget_);
        lines_node_->setLines(screenLines(term->getOutput()));
    }

    [[nodiscard]] bool handleEvent(const InputMessage& ev) noexcept override {
        auto term = std::static_pointer_cast<TerminalWidget>(widget_);

        // Mouse-button state is in char_value_ (value_b_ holds the cursor position).
        if (ev.type_ == InputEventType::MouseButton && ev.char_value_ != 0) {
            XApplication::requestFocus(shared_from_this());
            return true;
        }
        if (ev.type_ != InputEventType::KeyPress || ev.value_b_ == 0)
            return false;

        const int32_t key   = ev.value_a_;
        const bool    ctrl  = (ev.modifiers_ & 2) != 0;
        const bool    shift = (ev.modifiers_ & 1) != 0;

        // Ctrl+Shift+C — copy
        if (ctrl && shift && key == 46) {
            if (auto* bus = XApplication::getBus())
                Clipboard::setString(*bus, term->getOutput());
            return true;
        }
        // Ctrl+Shift+V — paste
        if (ctrl && shift && key == 47) {
            if (auto* bus = XApplication::getBus()) {
                uint32_t sid = term->sessionId();
                Clipboard::getString(*bus, [sid](const std::string& text) {
                    sendInput(sid, text.c_str(), text.size());
                });
            }
            return true;
        }
        // Ctrl+Shift+N — new session
        if (ctrl && shift && key == 49) {
            createSession("", 80, 24, [](uint32_t id, const std::string& name) {
                if (!id) return;
                g_sessions.push_back({id, name, true});
                g_primary_id = id;
                if (g_rebuild_tabs)  g_rebuild_tabs();
                if (g_rebuild_panes) g_rebuild_panes();
            });
            return true;
        }
        // Ctrl+Shift+W — detach/close current pane
        if (ctrl && shift && key == 17) {
            uint32_t sid = term->sessionId();
            if (sid) {
                detachSession(sid);
                // If it was primary, switch to next available
                if (sid == g_primary_id) {
                    g_primary_id = 0;
                    for (auto& s : g_sessions) {
                        if (s.id != sid) { g_primary_id = s.id; break; }
                    }
                }
                if (sid == g_secondary_id) {
                    g_secondary_id = 0;
                    g_split = SplitMode::Single;
                }
                if (g_rebuild_tabs)  g_rebuild_tabs();
                if (g_rebuild_panes) g_rebuild_panes();
            }
            return true;
        }

        // Arrow / special keys
        if      (key == 1)   { term->sendKey('\x1b'); }
        else if (key == 14)  { term->sendKey('\x7f'); }
        else if (key == 15)  { term->sendKey('\t'); }
        else if (key == 28)  { term->sendKey('\r'); }
        else if (key == 103) { term->sendSeq("\x1b[A", 3); }
        else if (key == 108) { term->sendSeq("\x1b[B", 3); }
        else if (key == 106) { term->sendSeq("\x1b[C", 3); }
        else if (key == 105) { term->sendSeq("\x1b[D", 3); }
        else if (key == 102) { term->sendSeq("\x1b[H", 3); }
        else if (key == 107) { term->sendSeq("\x1b[F", 3); }
        else if (key == 104) { term->sendSeq("\x1b[5~", 4); }
        else if (key == 109) { term->sendSeq("\x1b[6~", 4); }
        else if (key == 111) { term->sendSeq("\x1b[3~", 4); }
        else if (ev.char_value_ != 0) {
            if (ctrl && ev.char_value_ >= 'a' && ev.char_value_ <= 'z') {
                char ctrl_c = static_cast<char>(ev.char_value_ & 0x1F);
                term->sendKey(ctrl_c);
            } else {
                term->sendKey(static_cast<char>(ev.char_value_));
            }
        }
        return true;
    }

    SemanticNode getSemanticTree() const noexcept override {
        return Element::getSemanticTree();
    }

private:
    std::shared_ptr<RenderTermLines> lines_node_;
};

std::shared_ptr<Element> TerminalWidget::createElement() noexcept {
    return std::make_shared<TerminalElement>(
        std::static_pointer_cast<TerminalWidget>(shared_from_this()));
}

// ── Pane state ────────────────────────────────────────────────────────────────

// Live TerminalWidget instances indexed by session_id.
static std::unordered_map<uint32_t, std::shared_ptr<TerminalWidget>> g_pane_widgets;

static std::shared_ptr<TerminalWidget> getOrMakePaneWidget(uint32_t session_id) {
    auto it = g_pane_widgets.find(session_id);
    if (it != g_pane_widgets.end()) return it->second;
    auto w = std::make_shared<TerminalWidget>(session_id);
    g_pane_widgets[session_id] = w;
    return w;
}

// ── Session name helper ───────────────────────────────────────────────────────

static std::string sessionName(uint32_t id) {
    for (const auto& s : g_sessions)
        if (s.id == id) return s.name;
    return "session-" + std::to_string(id);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    Logger::init("terminal");

    // --open <script> (a shell script opened from Files / search): run it with
    // sh in a new tab instead of reattaching to the last session.
    std::string script;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--open") { script = argv[i + 1]; break; }
    LOG_INFO("Starting Terminal (multiplexer mode)…");

    // ── Tab bar container ─────────────────────────────────────────────────────
    auto tab_row = std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{});
    tab_row->setFixedSize(0.0f, 32.0f);
    tab_row->setGap(2.0f);
    tab_row->setPadding(4.0f);
    tab_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
    tab_row->setBackgroundColor(Color{0.10f, 0.10f, 0.12f, 1.0f});

    // ── Pane container (rebuilt on split change) ──────────────────────────────
    auto pane_container = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{});
    pane_container->setFlexGrow(1.0f);
    pane_container->setGap(0.0f);
    pane_container->setBackgroundColor(Color{0.0f, 0.0f, 0.0f, 1.0f});

    // ── Rename overlay (shared, shown in tab_row) ─────────────────────────────
    auto rename_input = std::make_shared<TextInputWidget>("Session name…");
    rename_input->setFixedSize(160.0f, 24.0f);
    rename_input->setVisibility(false);
    auto rename_session_id = std::make_shared<uint32_t>(0);

    // ── Rebuild: tab bar ──────────────────────────────────────────────────────
    g_rebuild_tabs = [=]() {
        tab_row->clearChildren();

        for (const auto& sess : g_sessions) {
            bool active = (sess.id == g_primary_id);
            auto tab_bg  = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{});
            tab_bg->setGap(4.0f);
            tab_bg->setPadding(4.0f);
            tab_bg->setCrossAxisAlignment(CrossAxisAlignment::Center);
            tab_bg->setBackgroundColor(active
                ? Color{0.22f, 0.40f, 0.70f, 1.0f}
                : Color{0.15f, 0.15f, 0.18f, 1.0f});

            auto name_lbl = std::make_shared<TextWidget>(sess.name, 11.0f);
            name_lbl->setColor(active ? Color{1.0f,1.0f,1.0f,1.0f}
                                      : Color{0.72f,0.72f,0.72f,1.0f});

            uint32_t sid = sess.id;

            // Close (×) button — detaches the session
            auto close_btn = std::make_shared<ButtonWidget>(
                std::make_shared<TextWidget>("×", 10.0f),
                [=]() {
                    detachSession(sid);
                    g_sessions.erase(
                        std::remove_if(g_sessions.begin(), g_sessions.end(),
                            [sid](const SessionInfo& s){ return s.id == sid; }),
                        g_sessions.end());
                    g_pane_widgets.erase(sid);
                    if (g_primary_id == sid) {
                        g_primary_id = g_sessions.empty() ? 0 : g_sessions[0].id;
                        if (g_primary_id) attachSession(g_primary_id);
                    }
                    if (g_secondary_id == sid) {
                        g_secondary_id = 0;
                        g_split = SplitMode::Single;
                    }
                    if (g_rebuild_tabs)  g_rebuild_tabs();
                    if (g_rebuild_panes) g_rebuild_panes();
                });
            close_btn->setFixedSize(18.0f, 18.0f);
            close_btn->setBackgroundColor(Color{0.0f, 0.0f, 0.0f, 0.0f});

            tab_bg->addChild(name_lbl);
            tab_bg->addChild(close_btn);
            tab_row->addChild(tab_bg);
        }

        // Rename input (hidden until double-click)
        rename_input->setOnChanged([](const std::string&) {
            // Update on Enter handled in key event
        });
        rename_input->setOnChanged([=](const std::string& v) {
            if (*rename_session_id && !v.empty()) {
                renameSession(*rename_session_id, v);
                for (auto& s : g_sessions)
                    if (s.id == *rename_session_id) { s.name = v; break; }
            }
            rename_input->setVisibility(false);
            *rename_session_id = 0;
            if (g_rebuild_tabs) g_rebuild_tabs();
        });
        tab_row->addChild(rename_input);

        // Spacer
        auto spacer = std::make_shared<FlexRow>(
            std::vector<std::shared_ptr<Widget>>{});
        spacer->setFlexGrow(1.0f);
        tab_row->addChild(spacer);

        // "+ New session" button
        auto new_btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("+ New", 11.0f),
            []() {
                createSession("", 80, 24, [](uint32_t id, const std::string& name) {
                    if (!id) return;
                    g_sessions.push_back({id, name, true});
                    detachSession(g_primary_id);
                    g_primary_id = id;
                    if (g_rebuild_tabs)  g_rebuild_tabs();
                    if (g_rebuild_panes) g_rebuild_panes();
                });
            });
        new_btn->setFixedSize(56.0f, 24.0f);
        new_btn->setBackgroundColor(Color{0.20f, 0.22f, 0.26f, 1.0f});
        tab_row->addChild(new_btn);

        // Separator
        tab_row->addChild(std::make_shared<Spacer>(8, 0));

        // ⊟ Horizontal split
        auto hsplit_btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("⊟ H", 11.0f),
            []() {
                if (g_sessions.size() < 2 || g_split != SplitMode::Single) return;
                // Find a session that is not primary
                for (const auto& s : g_sessions) {
                    if (s.id != g_primary_id) {
                        g_secondary_id = s.id;
                        break;
                    }
                }
                if (!g_secondary_id) {
                    // Create a new session for the split
                    createSession("", 80, 24, [](uint32_t id, const std::string& name) {
                        if (!id) return;
                        g_sessions.push_back({id, name, true});
                        g_secondary_id = id;
                        attachSession(id);
                        g_split = SplitMode::Horizontal;
                        if (g_rebuild_tabs)  g_rebuild_tabs();
                        if (g_rebuild_panes) g_rebuild_panes();
                    });
                    return;
                }
                attachSession(g_secondary_id);
                g_split = SplitMode::Horizontal;
                if (g_rebuild_tabs)  g_rebuild_tabs();
                if (g_rebuild_panes) g_rebuild_panes();
            });
        hsplit_btn->setFixedSize(42.0f, 24.0f);
        hsplit_btn->setBackgroundColor(Color{0.18f, 0.20f, 0.24f, 1.0f});

        // ⊞ Vertical split
        auto vsplit_btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("⊞ V", 11.0f),
            []() {
                if (g_sessions.size() < 2 || g_split != SplitMode::Single) return;
                for (const auto& s : g_sessions) {
                    if (s.id != g_primary_id) {
                        g_secondary_id = s.id;
                        break;
                    }
                }
                if (!g_secondary_id) {
                    createSession("", 80, 24, [](uint32_t id, const std::string& name) {
                        if (!id) return;
                        g_sessions.push_back({id, name, true});
                        g_secondary_id = id;
                        attachSession(id);
                        g_split = SplitMode::Vertical;
                        if (g_rebuild_tabs)  g_rebuild_tabs();
                        if (g_rebuild_panes) g_rebuild_panes();
                    });
                    return;
                }
                attachSession(g_secondary_id);
                g_split = SplitMode::Vertical;
                if (g_rebuild_tabs)  g_rebuild_tabs();
                if (g_rebuild_panes) g_rebuild_panes();
            });
        vsplit_btn->setFixedSize(42.0f, 24.0f);
        vsplit_btn->setBackgroundColor(Color{0.18f, 0.20f, 0.24f, 1.0f});

        // ✕ Remove split
        auto unsplit_btn = std::make_shared<ButtonWidget>(
            std::make_shared<TextWidget>("✕", 11.0f),
            []() {
                if (g_split == SplitMode::Single) return;
                detachSession(g_secondary_id);
                g_secondary_id = 0;
                g_split = SplitMode::Single;
                if (g_rebuild_tabs)  g_rebuild_tabs();
                if (g_rebuild_panes) g_rebuild_panes();
            });
        unsplit_btn->setFixedSize(24.0f, 24.0f);
        unsplit_btn->setBackgroundColor(Color{0.28f, 0.14f, 0.14f, 1.0f});

        tab_row->addChild(hsplit_btn);
        tab_row->addChild(vsplit_btn);
        tab_row->addChild(unsplit_btn);
    };

    // ── Rebuild: pane area ────────────────────────────────────────────────────
    g_rebuild_panes = [=]() {
        pane_container->clearChildren();

        auto make_pane = [&](uint32_t session_id, const std::string& name) {
            auto col = std::make_shared<FlexColumn>(
                std::vector<std::shared_ptr<Widget>>{});
            col->setFlexGrow(1.0f);
            col->setGap(0.0f);
            col->setBackgroundColor(Color{0.0f, 0.0f, 0.0f, 1.0f});

            // Thin pane label bar
            auto hdr = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{});
            hdr->setFixedSize(0.0f, 18.0f);
            hdr->setPadding(3.0f);
            hdr->setBackgroundColor(Color{0.08f, 0.08f, 0.10f, 1.0f});
            auto hdr_lbl = std::make_shared<TextWidget>(name, 10.0f);
            hdr_lbl->setColor(Color{0.55f, 0.55f, 0.55f, 1.0f});
            hdr->addChild(hdr_lbl);
            col->addChild(hdr);

            // Terminal content
            auto term_w = getOrMakePaneWidget(session_id);
            auto scroll = std::make_shared<ScrollViewWidget>(term_w);
            scroll->setFlexGrow(1.0f);
            col->addChild(scroll);
            return col;
        };

        if (g_split == SplitMode::Horizontal) {
            auto row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{});
            row->setFlexGrow(1.0f);
            row->setGap(0.0f);

            auto pane0 = make_pane(g_primary_id,   sessionName(g_primary_id));
            auto pane1 = make_pane(g_secondary_id, sessionName(g_secondary_id));
            pane0->setFlexGrow(g_split_ratio);
            pane1->setFlexGrow(1.0f - g_split_ratio);
            g_pane0_w = pane0;
            g_pane1_w = pane1;

            auto splitter = std::make_shared<SplitterWidget>(
                SplitterWidget::Orientation::Vertical);
            splitter->onDrag([](float r) {
                g_split_ratio = r;
                if (g_pane0_w) g_pane0_w->setFlexGrow(r);
                if (g_pane1_w) g_pane1_w->setFlexGrow(1.0f - r);
            });
            row->addChild(pane0);
            row->addChild(splitter);
            row->addChild(pane1);
            pane_container->addChild(row);

        } else if (g_split == SplitMode::Vertical) {
            auto pane0 = make_pane(g_primary_id,   sessionName(g_primary_id));
            auto pane1 = make_pane(g_secondary_id, sessionName(g_secondary_id));
            pane0->setFlexGrow(g_split_ratio);
            pane1->setFlexGrow(1.0f - g_split_ratio);
            g_pane0_w = pane0;
            g_pane1_w = pane1;

            auto splitter = std::make_shared<SplitterWidget>(
                SplitterWidget::Orientation::Horizontal);
            splitter->onDrag([](float r) {
                g_split_ratio = r;
                if (g_pane0_w) g_pane0_w->setFlexGrow(r);
                if (g_pane1_w) g_pane1_w->setFlexGrow(1.0f - r);
            });
            pane_container->addChild(pane0);
            pane_container->addChild(splitter);
            pane_container->addChild(pane1);

        } else {
            if (g_primary_id) {
                auto pane0 = make_pane(g_primary_id, sessionName(g_primary_id));
                pane_container->addChild(pane0);
            } else {
                // No session yet — placeholder
                auto lbl = std::make_shared<TextWidget>(
                    "No sessions.  Press Ctrl+Shift+N or click \"+ New\".", 13.0f);
                lbl->setColor(Color{0.35f, 0.35f, 0.35f, 1.0f});
                lbl->setPadding(30.0f);
                pane_container->addChild(lbl);
            }
        }
    };

    // ── Root layout ───────────────────────────────────────────────────────────
    auto root = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        tab_row,
        pane_container,
    });
    root->setGap(0.0f);
    root->setBackgroundColor(Color{0.0f, 0.0f, 0.0f, 1.0f});

    // Initial render with empty state
    g_rebuild_tabs();
    g_rebuild_panes();

    return XApplication::Run(root, "/tmp/freebsdx.sock", /*sandboxed=*/false,
                             /*is_shell=*/false,
        [=](XBusClient& xbus) {
            g_xbus = &xbus;
            XApplication::setTitle("Terminal");

            // ── List existing sessions from TerminalService ────────────────────
            // (handles Terminal.xapp restart → reattach to live sessions)
            auto load_sessions_page = std::make_shared<std::function<void(uint32_t)>>();
            *load_sessions_page = [=, &xbus, &load_sessions_page](uint32_t page) {
                TerminalSessionIdRequest req{};
                req.session_id_ = page;  // page index in session_id field
                xbus.Call("com.freebsdx.Terminal",
                          static_cast<uint32_t>(TerminalOpcode::ListSessions),
                          {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                          [=, &load_sessions_page](const XBusMessage& resp) {
                    TerminalListPage lp{};
                    std::memcpy(&lp, resp.payload_, sizeof(lp));

                    for (uint32_t i = 0; i < lp.count_; ++i) {
                        const auto& e = lp.entries_[i];
                        if (e.id_ == 0) continue;
                        char name[sizeof(e.name_) + 1]{};
                        std::strncpy(name, e.name_, sizeof(e.name_));
                        // Avoid duplicates
                        bool found = false;
                        for (auto& s : g_sessions)
                            if (s.id == e.id_) { found = true; break; }
                        if (!found)
                            g_sessions.push_back({e.id_, name, false});
                    }

                    if (lp.has_more_) {
                        if (*load_sessions_page)
                            (*load_sessions_page)(lp.page_ + 1);
                    } else {
                        // All sessions loaded — attach to the most recent one
                        if (!g_sessions.empty() && script.empty()) {
                            g_primary_id = g_sessions.back().id;
                            attachSession(g_primary_id);
                        } else {
                            // No existing sessions — create a fresh one
                            createSession("", 80, 24,
                                [script](uint32_t id, const std::string& name) {
                                if (!id) return;
                                g_sessions.push_back({id, name, true});
                                g_primary_id = id;
                                if (!script.empty()) {
                                    std::string q = "'";
                                    for (char c : script) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
                                    std::string cmd = "sh " + q + "'\n";
                                    sendInput(id, cmd.data(), cmd.size());
                                }
                                if (g_rebuild_tabs)  g_rebuild_tabs();
                                if (g_rebuild_panes) g_rebuild_panes();
                            });
                            return;
                        }
                        if (g_rebuild_tabs)  g_rebuild_tabs();
                        if (g_rebuild_panes) g_rebuild_panes();
                    }
                });
            };
            (*load_sessions_page)(0);

            // ── Broadcast handler ──────────────────────────────────────────────
            xbus.OnBroadcast([=, &xbus](const XBusMessage& bcast) {
                if (std::strcmp(bcast.service_name_, "com.freebsdx.Terminal") != 0)
                    return;

                // OutputChunk → route to the matching TerminalWidget
                if (bcast.opcode_ ==
                    static_cast<uint32_t>(TerminalOpcode::OutputChunk))
                {
                    TerminalDataChunk chunk{};
                    std::memcpy(&chunk, bcast.payload_, sizeof(chunk));
                    uint32_t sid = chunk.session_id_;
                    auto it = g_pane_widgets.find(sid);
                    if (it != g_pane_widgets.end() && chunk.size_ > 0 &&
                        chunk.size_ <= sizeof(chunk.data_)) {
                        try {
                            it->second->appendOutput(chunk.data_,
                                                     static_cast<size_t>(chunk.size_));
                        } catch (...) {}
                    }
                    return;
                }

                // SessionChanged → refresh session list
                if (bcast.opcode_ ==
                    static_cast<uint32_t>(TerminalOpcode::SessionChanged))
                {
                    // Re-enumerate and sync g_sessions (add new, keep existing)
                    TerminalSessionIdRequest req{};
                    req.session_id_ = 0;
                    xbus.Call("com.freebsdx.Terminal",
                              static_cast<uint32_t>(TerminalOpcode::ListSessions),
                              {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                              [](const XBusMessage& resp) {
                        TerminalListPage lp{};
                        std::memcpy(&lp, resp.payload_, sizeof(lp));
                        for (uint32_t i = 0; i < lp.count_; ++i) {
                            const auto& e = lp.entries_[i];
                            if (!e.id_) continue;
                            bool found = false;
                            for (auto& s : g_sessions)
                                if (s.id == e.id_) {
                                    char name[sizeof(e.name_)+1]{};
                                    std::strncpy(name, e.name_, sizeof(e.name_));
                                    s.name = name;
                                    found = true; break;
                                }
                            if (!found) {
                                char name[sizeof(e.name_)+1]{};
                                std::strncpy(name, e.name_, sizeof(e.name_));
                                g_sessions.push_back({e.id_, name, false});
                            }
                        }
                        try {
                            if (g_rebuild_tabs) g_rebuild_tabs();
                        } catch (...) {}
                    });
                }
            });
        });
}
