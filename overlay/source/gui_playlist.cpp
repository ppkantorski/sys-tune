#include "gui_playlist.hpp"
#include "gui_main.hpp"

#include "elm_overlayframe.hpp"
#include "config/config.hpp"
#include "play_context.hpp"
#include "symbol.hpp"
#include "tune.h"

#include <memory>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// =============================================================================
// Lightweight tag reader — title + artist only, no art.
// Self-contained copy in this TU's anonymous namespace.
// =============================================================================

    static u32 ta_be32(const u8 *p) {
        return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|(u32)p[3];
    }
    static u32 ta_le32(const u8 *p) {
        return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
    }
    static u32 ta_syncsafe(const u8 *p) {
        return ((u32)(p[0]&0x7F)<<21)|((u32)(p[1]&0x7F)<<14)|
               ((u32)(p[2]&0x7F)<<7) |(u32)(p[3]&0x7F);
    }
    static bool ta_keycmp(const char *a, const char *b) {
        for (; *a && *b; ++a, ++b)
            if ((*a | 0x20) != (*b | 0x20)) return false;
        return *a == '\0' && *b == '\0';
    }

    struct TitleArtist { std::string title; std::string artist; };

    static std::string ta_decodeString(const u8 *payload, size_t size) {
        if (size < 1) return {};
        u8 enc = payload[0];
        const u8 *s = payload + 1;
        size_t    n = size   - 1;

        if (enc == 0 || enc == 3) {
            size_t len = 0;
            while (len < n && s[len]) len++;
            return std::string(reinterpret_cast<const char*>(s), len);
        }

        bool be = (enc == 2);
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if      (s[0]==0xFE && s[1]==0xFF) { be=true;  i=2; }
            else if (s[0]==0xFF && s[1]==0xFE) { be=false; i=2; }
        }
        std::string r;
        r.reserve(n / 2);
        for (; i+1 < n; i+=2) {
            u16 cp = be ? (u16)((s[i]<<8)|s[i+1]) : (u16)(s[i]|(s[i+1]<<8));
            if (!cp) break;
            if      (cp < 0x80)  { r += (char)cp; }
            else if (cp < 0x800) { r += (char)(0xC0|(cp>>6));
                                   r += (char)(0x80|(cp&0x3F)); }
            else                 { r += (char)(0xE0|(cp>>12));
                                   r += (char)(0x80|((cp>>6)&0x3F));
                                   r += (char)(0x80|(cp&0x3F)); }
        }
        return r;
    }

    // ---- MP3 / ID3 ----------------------------------------------------------

    static TitleArtist ta_readID3(FILE *f) {
        fseek(f, 0, SEEK_SET);
        u8 hdr[10];
        if (fread(hdr,1,10,f)!=10 || memcmp(hdr,"ID3",3)!=0) return {};
        u8  ver     = hdr[3];
        u8  flags   = hdr[5];
        u32 tagSize = ta_syncsafe(hdr+6);
        if (tagSize > 16*1024*1024) return {};
        // Cap at 64 KB — title/artist frames are always near the front;
        // only embedded album art pushes tags beyond this, and we don't need it.
        constexpr u32 kMaxTagRead = 64 * 1024;
        if (tagSize > kMaxTagRead) tagSize = kMaxTagRead;

        size_t total = 10 + tagSize;
        auto buf = std::make_unique<u8[]>(total);
        memcpy(buf.get(), hdr, 10);
        fseek(f, 10, SEEK_SET);
        if (fread(buf.get()+10, 1, tagSize, f) != tagSize) return {};

        TitleArtist ta;
        const u8 *data = buf.get();
        size_t pos = 10;
        if ((flags & 0x40) && ver >= 3) {
            if (pos+4 > total) return ta;
            u32 exSz = (ver==4) ? ta_syncsafe(data+pos) : ta_be32(data+pos);
            pos += exSz;
        }
        size_t end = std::min((size_t)(10+tagSize), total);

        while (pos+6 < end) {
            if (ver == 2) {
                if (pos+6 > end) break;
                const u8 *fh = data+pos; pos+=6;
                if (!fh[0]) break;
                u32 sz = ((u32)fh[3]<<16)|((u32)fh[4]<<8)|(u32)fh[5];
                if (pos+sz > total) break;
                if (ta.title.empty()  && memcmp(fh,"TT2",3)==0)
                    ta.title  = ta_decodeString(data+pos, sz);
                if (ta.artist.empty() && memcmp(fh,"TP1",3)==0)
                    ta.artist = ta_decodeString(data+pos, sz);
                pos += sz;
            } else {
                if (pos+10 > end) break;
                const u8 *fh = data+pos; pos+=10;
                if (!fh[0]) break;
                u32 sz = (ver==4) ? ta_syncsafe(fh+4) : ta_be32(fh+4);
                if (!sz || pos+sz > total) { pos+=sz; continue; }
                if (ta.title.empty()  && memcmp(fh,"TIT2",4)==0)
                    ta.title  = ta_decodeString(data+pos, sz);
                if (ta.artist.empty() && memcmp(fh,"TPE1",4)==0)
                    ta.artist = ta_decodeString(data+pos, sz);
                pos += sz;
            }
            if (!ta.title.empty() && !ta.artist.empty()) break;
        }
        return ta;
    }

    // ---- FLAC ---------------------------------------------------------------

    static void ta_readVorbisComment(FILE *f, u32 blkLen,
                                     std::string &title, std::string &artist) {
        u8 tmp[4];
        if (fread(tmp,1,4,f)!=4) return;
        u32 vlen = ta_le32(tmp);
        if (vlen > blkLen) return;
        fseek(f, (long)vlen, SEEK_CUR);

        if (fread(tmp,1,4,f)!=4) return;
        u32 count = ta_le32(tmp);
        if (count > 2000) return;

        char buf[512];
        for (u32 i = 0; i < count; i++) {
            if (fread(tmp,1,4,f)!=4) return;
            u32 clen = ta_le32(tmp);
            if (clen == 0) continue;
            if (clen > 65536) { fseek(f, (long)clen, SEEK_CUR); continue; }

            size_t toRead = std::min((size_t)clen, sizeof(buf)-1);
            if (fread(buf, 1, toRead, f) != toRead) return;
            buf[toRead] = '\0';
            if (clen > (u32)toRead) fseek(f, (long)(clen-toRead), SEEK_CUR);

            char *eq = strchr(buf, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *val = eq + 1;

            if      (title.empty()  && ta_keycmp(buf, "TITLE"))  title  = val;
            else if (artist.empty() && ta_keycmp(buf, "ARTIST")) artist = val;

            if (!title.empty() && !artist.empty()) return;
        }
    }

    static TitleArtist ta_readFLAC(FILE *f) {
        TitleArtist ta;
        fseek(f, 4, SEEK_SET);
        u8 bh[4];
        while (fread(bh,1,4,f)==4) {
            bool last = (bh[0]&0x80)!=0;
            u8   type = bh[0]&0x7F;
            u32  len  = ((u32)bh[1]<<16)|((u32)bh[2]<<8)|(u32)bh[3];
            if (len > 16*1024*1024) break;
            long blockStart = ftell(f);

            if (type == 4 && (ta.title.empty() || ta.artist.empty()))
                ta_readVorbisComment(f, len, ta.title, ta.artist);

            if (last) break;
            if (!ta.title.empty() && !ta.artist.empty()) break;
            fseek(f, blockStart + (long)len, SEEK_SET);
        }
        return ta;
    }

    // ---- WAV ----------------------------------------------------------------

    static TitleArtist ta_readWAV(FILE *f) {
        fseek(f, 12, SEEK_SET);
        u8 ch[8];
        while (fread(ch,1,8,f)==8) {
            u32 sz = ta_le32(ch+4);
            if (sz > 16*1024*1024) break;

            if (memcmp(ch,"id3 ",4)==0 || memcmp(ch,"ID3 ",4)==0) {
                auto buf = std::make_unique<u8[]>(sz);
                if (fread(buf.get(),1,sz,f)!=sz) return {};
                if (sz < 10 || memcmp(buf.get(),"ID3",3)!=0) return {};

                u8  ver     = buf[3];
                u8  flags   = buf[5];
                u32 tagSize = ta_syncsafe(buf.get()+6);
                if (tagSize+10 > sz) return {};

                TitleArtist ta;
                const u8 *data = buf.get();
                size_t pos = 10;
                if ((flags & 0x40) && ver >= 3) {
                    if (pos+4 > sz) return ta;
                    u32 exSz = (ver==4) ? ta_syncsafe(data+pos) : ta_be32(data+pos);
                    pos += exSz;
                }
                size_t end = std::min((size_t)(10+tagSize), (size_t)sz);
                while (pos+6 < end) {
                    if (ver == 2) {
                        if (pos+6 > end) break;
                        const u8 *fh=data+pos; pos+=6;
                        if (!fh[0]) break;
                        u32 fsz=((u32)fh[3]<<16)|((u32)fh[4]<<8)|(u32)fh[5];
                        if (pos+fsz > sz) break;
                        if (ta.title.empty()  && memcmp(fh,"TT2",3)==0)
                            ta.title  = ta_decodeString(data+pos, fsz);
                        if (ta.artist.empty() && memcmp(fh,"TP1",3)==0)
                            ta.artist = ta_decodeString(data+pos, fsz);
                        pos += fsz;
                    } else {
                        if (pos+10 > end) break;
                        const u8 *fh=data+pos; pos+=10;
                        if (!fh[0]) break;
                        u32 fsz=(ver==4)?ta_syncsafe(fh+4):ta_be32(fh+4);
                        if (!fsz || pos+fsz > sz) { pos+=fsz; continue; }
                        if (ta.title.empty()  && memcmp(fh,"TIT2",4)==0)
                            ta.title  = ta_decodeString(data+pos, fsz);
                        if (ta.artist.empty() && memcmp(fh,"TPE1",4)==0)
                            ta.artist = ta_decodeString(data+pos, fsz);
                        pos += fsz;
                    }
                    if (!ta.title.empty() && !ta.artist.empty()) break;
                }
                return ta;
            }
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
        return {};
    }

    // ---- Public entry point -------------------------------------------------

    static TitleArtist readTitleArtist(const char *path) {
        TitleArtist ta;
        FILE *f = fopen(path, "rb");
        if (!f) return ta;

        u8 magic[4] = {};
        fread(magic, 1, 4, f);

        if      (memcmp(magic, "ID3",  3) == 0) ta = ta_readID3(f);
        else if (memcmp(magic, "fLaC", 4) == 0) ta = ta_readFLAC(f);
        else if (memcmp(magic, "RIFF", 4) == 0) ta = ta_readWAV(f);

        fclose(f);

        if (ta.title.empty()) {
            const char *slash = std::strrchr(path, '/');
            const char *name  = slash ? slash+1 : path;
            ta.title = name;
            size_t dot = ta.title.rfind('.');
            if (dot != std::string::npos && dot > 0)
                ta.title.resize(dot);
        }
        if (ta.artist.empty())
            ta.artist = "Unknown Artist";

        return ta;
    }

// =============================================================================
// UI helpers
// =============================================================================

    constexpr float kIndicatorScale = 0.65f;
    constexpr s32   kSymMargin      = 19;

    /* Deferred focus item — set by click handlers, resolved in update(). */
    class ButtonListItem;
    ButtonListItem *g_focus_item = nullptr;

    /* Fire the count callback with the saved-playlist size. */
    void notifyCountChanged(const std::function<void(u32)> &cb) {
        if (!cb) return;
        cb(play_ctx::savedPlaylistSize());
    }

// =============================================================================
// ButtonListItem
//
// Label format: "<entry_num>  ⊘  <song> by <artist>"
//
// The play/pause indicator is shown ONLY when:
//   • play_ctx::source() == Playlist  (IPC queue == saved[])
//   • this item's full_path matches play_ctx::currentPath()
//
// When source == Folder the user is playing from a folder, so no indicator
// appears in the playlist view.
// =============================================================================

    class ButtonListItem final : public tsl::elm::ListItem {
        std::string m_full_path;
        std::string m_song_name;
        std::string m_artist_name;
        u32         m_entry_num;
        bool        m_was_current = false;

        static constexpr const char *kPlaceholder = "\uE098";

    public:
        static std::string buildLabel(u32 num,
                                      const std::string &song,
                                      const std::string &artist) {
            return std::to_string(num) + ult::DIVIDER_SYMBOL +
                   song + " by " + artist;
        }

        ButtonListItem(u32 entry_num,
                       std::string song_name,
                       std::string artist_name,
                       std::string full_path)
            : ListItem(buildLabel(entry_num, song_name, artist_name),
                       /*value=*/"", /*isMini=*/true)
            , m_full_path  (std::move(full_path))
            , m_song_name  (std::move(song_name))
            , m_artist_name(std::move(artist_name))
            , m_entry_num  (entry_num) {}

        const std::string &getFullPath() const { return m_full_path; }

        std::string getLabel() const {
            return buildLabel(m_entry_num, m_song_name, m_artist_name);
        }

        void setEntryNumber(u32 num) {
            if (m_entry_num == num) return;
            m_entry_num  = num;
            const std::string newLabel = buildLabel(num, m_song_name, m_artist_name);
            m_text       = newLabel;
            m_text_clean = newLabel;
            m_maxWidth          = 0;
            m_textWidth         = 0;
            m_scrollText.clear();
            m_ellipsisText.clear();
            m_flags.m_truncated = false;
        }

        /* True only when Playlist context is active AND this path is current. */
        bool isCurrent() const {
            if (play_ctx::source() != play_ctx::Source::Playlist) return false;
            const char *cp = play_ctx::currentPath();
            return !m_full_path.empty() && cp[0] != '\0' &&
                   strcasecmp(m_full_path.c_str(), cp) == 0;
        }

        void draw(tsl::gfx::Renderer *renderer) override {
            const bool cur = isCurrent();

            if (cur != m_was_current) {
                m_was_current = cur;
                setValue(cur ? kPlaceholder : "");
                m_maxWidth = 0;   // force layout recalc on state change
            }

            if (!cur) {
                ListItem::draw(renderer);
                return;
            }

            // ---- Indicator path --------------------------------------------
            const s32 scaledW = static_cast<s32>(26.0f * kIndicatorScale);

            if (!m_maxWidth) {
                const u16 textMaxW = static_cast<u16>(
                    static_cast<s32>(getWidth()) - kSymMargin - scaledW - kSymMargin - 19);
                m_maxWidth = static_cast<u16>(
                    static_cast<s32>(getWidth()) - kSymMargin - scaledW - kSymMargin - 55);
                const u16 textW = static_cast<u16>(
                    renderer->getTextDimensions(m_text_clean, false, 23).first);
                m_flags.m_truncated = (textW > textMaxW);
                if (m_flags.m_truncated) {
                    m_scrollText.clear();
                    m_scrollText.reserve(m_text_clean.size() * 2 + 8);
                    m_scrollText.append(m_text_clean).append("        ");
                    m_textWidth = static_cast<u16>(
                        renderer->getTextDimensions(m_scrollText, false, 23).first);
                    m_scrollText.append(m_text_clean);
                    m_ellipsisText = renderer->limitStringLength(
                        m_text_clean, false, 23, textMaxW);
                } else {
                    m_textWidth = static_cast<u16>(textW);
                }
            }

            {
                const std::string saved_value = m_value;
                m_value = "";
                ListItem::draw(renderer);
                m_value = saved_value;
            }

            const s32 cx = getX() + static_cast<s32>(getWidth()) - kSymMargin - scaledW / 2;
            const s32 cy = getY() + static_cast<s32>(m_listItemHeight) / 2;

            const bool useClickTextColor =
                m_touched &&
                Element::getInputMode() == tsl::InputMode::Touch &&
                ult::touchInBounds;

            const tsl::Color symColor = (m_focused && ult::useSelectionValue)
                ? (useClickTextColor ? tsl::clickTextColor : tsl::selectedValueTextColor)
                : tsl::onTextColor;

            const auto &sym = play_ctx::isPlaying()
                ? symbol::pause::symbol
                : symbol::play::symbol;

            sym.draw(play_ctx::isPlaying() ? cx - 1 : cx, cy, renderer, symColor, kIndicatorScale);
        }

        bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY,
                     s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
            if (event == tsl::elm::TouchEvent::Touch)
                this->m_touched = this->inBounds(currX, currY);

            if (event == tsl::elm::TouchEvent::Release && this->m_touched) {
                this->m_touched = false;
                if (Element::getInputMode() == tsl::InputMode::Touch) {
                    m_clickAnimationProgress = 0;
                    const bool handled = onClick(HidNpadButton_A);
                    if (handled) tsl::shiftItemFocus(this);
                    return handled;
                }
            }
            return false;
        }
    };

} // namespace

// =============================================================================
// PlaylistGui constructor
//
// Builds the UI from play_ctx::savedPlaylist() — the user's persisted list —
// regardless of whether we are currently in Playlist or Folder context.
// =============================================================================

PlaylistGui::PlaylistGui(std::function<void(u32)> on_count_changed)
    : m_on_count_changed(std::move(on_count_changed)) {

    m_list = new tsl::elm::List();

    // Re-snapshot the IPC queue order into g_saved so the list always reflects
    // the actual playback order (including any shuffle the service has applied).
    // This also calls poll() internally to refresh currentPath().
    play_ctx::resyncFromIPC();

    const auto &saved = play_ctx::savedPlaylist();
    const u32   count = static_cast<u32>(saved.size());

    if (count == 0) {
        m_list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            const size_t ix = x + (w - r->getTextDimensions("\uE150", false, 90).first) / 2;
            r->drawString("\uE150", false, ix, y + (h / 2) - 10+30, 90, tsl::defaultTextColor);
            const size_t tx = x + (w - r->getTextDimensions("Playlist is empty!", false, 25).first) / 2;
            r->drawString("Playlist is empty!", false, tx, y + (h / 2) + 90+30, 25, tsl::defaultTextColor);
        }), 380);
        return;
    }

    m_list->addItem(new tsl::elm::CategoryHeader(
        "Playlist " + ult::DIVIDER_SYMBOL +
        " \uE0E3 Remove "   + ult::DIVIDER_SYMBOL +
        " \uE0E2 Remove All " + ult::DIVIDER_SYMBOL +
        " \uE0B6 Set As Startup", true));

    m_items.reserve(count);

    // Jump target: only meaningful when source == Playlist (indicator would
    // be shown) — otherwise we leave the list at the top.
    const bool   inPlaylistCtx = (play_ctx::source() == play_ctx::Source::Playlist);
    const char  *cp            = play_ctx::currentPath();
    std::string  current_label;

    for (u32 i = 0; i < count; ++i) {
        const std::string &full_path = saved[i];

        TitleArtist ta = readTitleArtist(full_path.c_str());

        auto *item = new ButtonListItem(i + 1,
                                        std::move(ta.title),
                                        std::move(ta.artist),
                                        full_path);

        // Only record a jump target when the playlist is the active context.
        if (inPlaylistCtx && cp[0] != '\0' &&
            !strcasecmp(full_path.c_str(), cp) &&
            current_label.empty())
        {
            current_label = item->getLabel();
        }

        item->setClickListener([this, item](u64 keys) -> bool {
            const auto index      = this->m_list->getIndexInList(item);
            const auto tune_index = index - 1;  // subtract 1 for the header

            // ---- A: play / toggle / context-switch -------------------------
            if (keys & HidNpadButton_A) {

                if (play_ctx::source() == play_ctx::Source::Playlist) {
                    // Already in playlist context — toggle or select.
                    const bool isCurrentTrack =
                        play_ctx::currentPath()[0] != '\0' &&
                        strcasecmp(item->getFullPath().c_str(),
                                   play_ctx::currentPath()) == 0;

                    if (isCurrentTrack) {
                        if (play_ctx::isPlaying()) { tunePause(); }
                        else                       { tunePlay();  }
                    } else {
                        tuneSelect(tune_index);
                    }
                    play_ctx::poll();

                } else {
                    // Folder context — g_saved is in original sorted order and
                    // tune_index is the sorted position.  If shuffle is on,
                    // tuneSelect(tune_index) would play m_shuffle_playlist[tune_index]
                    // which is a random song, not the one clicked.  Disable shuffle
                    // first so the sorted index maps correctly, then switch context.
                    {
                        TuneShuffleMode shuffleMode = TuneShuffleMode_Off;
                        tuneGetShuffleMode(&shuffleMode);
                        if (shuffleMode != TuneShuffleMode_Off) {
                            tuneSetShuffleMode(TuneShuffleMode_Off);
                            config::set_shuffle(TuneShuffleMode_Off);
                        }
                    }
                    play_ctx::switchToPlaylist(static_cast<u32>(tune_index));
                    play_ctx::poll();
                }
                return true;

            // ---- Y: remove this item from saved playlist -------------------
            } else if (keys & KEY_Y) {
                bool ok = false;
                if (play_ctx::source() == play_ctx::Source::Playlist) {
                    // IPC and saved[] are in sync — remove from both.
                    ok = play_ctx::playlistTuneRemove(static_cast<u32>(tune_index));
                } else {
                    // Folder context — IPC has folder songs; only remove from saved[].
                    play_ctx::savedRemove(static_cast<u32>(tune_index));
                    ok = true;
                }

                if (ok) {
                    const s32 nextIndex = (index + 1 < this->m_list->getLastIndex())
                        ? index + 1 : index - 1;
                    this->removeFocus();
                    this->m_list->removeIndex(index);
                    this->m_list->setFocusedIndex(nextIndex);

                    if ((size_t)tune_index < this->m_items.size())
                        this->m_items.erase(this->m_items.begin() + tune_index);

                    for (size_t k = (size_t)tune_index; k < this->m_items.size(); ++k)
                        static_cast<ButtonListItem*>(this->m_items[k])->setEntryNumber(
                            static_cast<u32>(k + 1));

                    notifyCountChanged(m_on_count_changed);
                    triggerFeedbackImpl(triggerRumbleClick, triggerMoveSound);
                }
                return true;

            // ---- X: clear entire playlist ----------------------------------
            } else if (keys & KEY_X) {
                bool ok = false;
                if (play_ctx::source() == play_ctx::Source::Playlist) {
                    ok = play_ctx::playlistTuneClearQueue();
                } else {
                    play_ctx::savedClear();
                    ok = true;
                }

                if (ok) {
                    this->removeFocus();
                    this->m_list->clear();
                    this->m_items.clear();
                    m_list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
                        const size_t ix = x + (w - r->getTextDimensions("\uE150", false, 90).first) / 2;
                        r->drawString("\uE150", false, ix, y + (h / 2) - 10+30, 90, tsl::defaultTextColor);
                        const size_t tx = x + (w - r->getTextDimensions("Playlist is empty!", false, 25).first) / 2;
                        r->drawString("Playlist is empty!", false, tx, y + (h / 2) + 90+30, 25, tsl::defaultTextColor);
                    }), 380);
                    if (m_on_count_changed) {
                        triggerFeedbackImpl(triggerRumbleClick, triggerMoveSound);
                        m_on_count_changed(0);
                    }
                }
                return true;

            // ---- Minus: set as startup path --------------------------------
            } else if (keys & KEY_MINUS) {
                // Use saved[] path directly — it's the authoritative source.
                const auto &sp = play_ctx::savedPlaylist();
                if ((size_t)tune_index < sp.size())
                    config::set_load_path(sp[tune_index].c_str());
                return true;
            }
            return false;
        });

        m_list->addItem(item);
        m_items.push_back(item);
    }

    // Jump to currently playing item (only when in Playlist context and
    // the track is actually in this list).
    if (!current_label.empty())
        m_list->jumpToItem(current_label);
}

// =============================================================================

tsl::elm::Element *PlaylistGui::createUI() {
    auto *rootFrame = new SysTuneOverlayFrame(/*pageLeft=*/"Player", /*pageRight=*/"");
    rootFrame->setContent(this->m_list);
    return rootFrame;
}

// =============================================================================

void PlaylistGui::update() {
    // Resolve any deferred focus change set during a click handler.
    if (g_focus_item) {
        const auto index = m_list->getIndexInList(g_focus_item);
        if (index >= 0) {
            this->removeFocus();
            this->requestFocus(g_focus_item, tsl::FocusDirection::Down);
            m_list->setFocusedIndex(index);
            g_focus_item = nullptr;
        }
    }

    static u8 tick = 0;
    if ((++tick % 15) == 0)
        play_ctx::poll();
}

// ---------------------------------------------------------------------------
bool PlaylistGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                              HidAnalogStickState joyStickPosLeft,
                              HidAnalogStickState joyStickPosRight) {
    /* Left footer tap OR KEY_LEFT — swap back to player.
       Stack before: [SettingsGui, PlaylistGui]
       SwapDepth{2}: pop×2, push MainGui → [MainGui] → B exits cleanly. */
    const bool goLeft = ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel)
                     || ((keysDown & KEY_LEFT) && !(keysHeld & ~KEY_LEFT & ~KEY_R & ALL_KEYS_MASK));
    if (goLeft) {
        setPlayerRightDest(PlayerRightDest::Playlist);
        triggerNavigationFeedback();
        tsl::swapTo<MainGui>(SwapDepth{2});
        return true;
    }
    return SysTuneGui::handleInput(keysDown, keysHeld, touchPos,
                                   joyStickPosLeft, joyStickPosRight);
}