/* stb_image — drop stb_image.h anywhere in your include path.
   https://github.com/nothings/stb
   Define STB_IMAGE_IMPLEMENTATION exactly once in your project.
   If another TU already defines it, remove the line below. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "elm_status_bar.hpp"
#include "symbol.hpp"
#include "config/config.hpp"

#include <cstdio>
#include <cstring>
#include <memory>       /* std::make_unique */
#include <algorithm>    /* std::min        */

// =============================================================================
// Tag / art extraction helpers
// =============================================================================
namespace {

    char path_buffer[FS_MAX_PATH] = "";
    char current_buffer[0x20] = "";
    char total_buffer[0x20] = "";

    void NullLastDot(char *str) {
        char *end = str + strlen(str) - 1;
        while (str != end) {
            if (*end == '.') { *end = '\0'; return; }
            end--;
        }
    }

    static u32 be32(const u8 *p) {
        return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|(u32)p[3];
    }
    static u32 le32(const u8 *p) {
        return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
    }
    static u32 syncsafe(const u8 *p) {
        return ((u32)(p[0]&0x7F)<<21)|((u32)(p[1]&0x7F)<<14)|
               ((u32)(p[2]&0x7F)<<7) |(u32)(p[3]&0x7F);
    }

    static bool keycmp(const char *a, const char *b) {
        for (; *a && *b; ++a, ++b)
            if ((*a | 0x20) != (*b | 0x20)) return false;
        return *a == '\0' && *b == '\0';
    }

    struct BufSpan  { size_t offset = 0; size_t size = 0; };
    struct ArtRegion {
        long   offset = -1;
        size_t size   = 0;
        bool valid() const { return offset >= 0 && size > 0; }
    };

    struct TrackTags {
        ArtRegion   art;
        std::string title;
        std::string artist;
    };

    static std::string extractID3String(const u8 *payload, size_t size) {
        if (size < 1) return {};
        u8 enc = payload[0];
        const u8 *s = payload + 1;
        size_t    n = size - 1;

        if (enc == 0 || enc == 3) {
            size_t len = 0;
            while (len < n && s[len]) len++;
            return std::string(reinterpret_cast<const char *>(s), len);
        }

        bool be = (enc == 2);
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if      (s[0] == 0xFE && s[1] == 0xFF) { be = true;  i = 2; }
            else if (s[0] == 0xFF && s[1] == 0xFE) { be = false; i = 2; }
        }
        std::string result;
        result.reserve(n / 2);
        for (; i + 1 < n; i += 2) {
            u16 cp = be ? (u16)((s[i] << 8) | s[i+1])
                        : (u16)(s[i] | (s[i+1] << 8));
            if (!cp) break;
            if      (cp < 0x80)  { result += (char)cp; }
            else if (cp < 0x800) { result += (char)(0xC0 | (cp >> 6));
                                   result += (char)(0x80 | (cp & 0x3F)); }
            else                 { result += (char)(0xE0 | (cp >> 12));
                                   result += (char)(0x80 | ((cp >> 6) & 0x3F));
                                   result += (char)(0x80 | (cp & 0x3F)); }
        }
        return result;
    }

    static void scanID3Buffer(const u8 *data, size_t total,
                               BufSpan &art, std::string &title, std::string &artist) {
        if (total < 10 || memcmp(data, "ID3", 3) != 0) return;
        u8  ver     = data[3];
        u8  flags   = data[5];
        u32 tagSize = syncsafe(data + 6);
        size_t pos  = 10;

        if ((flags & 0x40) && ver >= 3) {
            if (pos + 4 > total) return;
            u32 exSz = (ver == 4) ? syncsafe(data+pos) : be32(data+pos);
            pos += exSz;
        }
        size_t end = std::min((size_t)(10 + tagSize), total);

        while (pos + 6 < end) {
            if (ver == 2) {
                if (pos + 6 > end) break;
                const u8 *fh = data + pos; pos += 6;
                if (!fh[0]) break;
                u32 sz = ((u32)fh[3]<<16)|((u32)fh[4]<<8)|(u32)fh[5];
                if (pos + sz > total) break;

                if (!art.size && memcmp(fh, "PIC", 3) == 0 && sz > 6) {
                    size_t p = pos + 1 + 3 + 1;
                    while (p < pos+sz && data[p]) { p++; } p++;
                    if (p < pos+sz) art = {p, pos+sz-p};
                }
                else if (title.empty()  && memcmp(fh, "TT2", 3) == 0)
                    title  = extractID3String(data+pos, sz);
                else if (artist.empty() && memcmp(fh, "TP1", 3) == 0)
                    artist = extractID3String(data+pos, sz);

                pos += sz;
            } else {
                if (pos + 10 > end) break;
                const u8 *fh = data + pos; pos += 10;
                if (!fh[0]) break;
                u32 sz = (ver == 4) ? syncsafe(fh+4) : be32(fh+4);
                if (!sz || pos + sz > total) { pos += sz; continue; }

                if (!art.size && memcmp(fh, "APIC", 4) == 0) {
                    u8 enc = data[pos];
                    size_t p = pos + 1;
                    while (p < pos+sz && data[p]) { p++; } p++;  /* skip MIME + null */
                    if (p >= pos+sz) { pos += sz; continue; }
                    p++;                                           /* picture type */
                    if (enc == 1 || enc == 2) {
                        while (p+1 < pos+sz && (data[p]||data[p+1])) { p+=2; } p+=2;
                    } else {
                        while (p < pos+sz && data[p]) { p++; } p++;
                    }
                    if (p < pos+sz) art = {p, pos+sz-p};
                }
                else if (title.empty()  && memcmp(fh, "TIT2", 4) == 0)
                    title  = extractID3String(data+pos, sz);
                else if (artist.empty() && memcmp(fh, "TPE1", 4) == 0)
                    artist = extractID3String(data+pos, sz);

                pos += sz;
            }

            if (art.size && !title.empty() && !artist.empty()) break;
        }
    }

    static TrackTags readTagsMP3(FILE *f) {
        fseek(f, 0, SEEK_SET);
        u8 hdr[10];
        if (fread(hdr,1,10,f)!=10 || memcmp(hdr,"ID3",3)!=0) return {};
        u32 tagSize = syncsafe(hdr+6);
        if (tagSize > 16*1024*1024) return {};

        size_t total = 10 + tagSize;
        auto buf = std::make_unique<u8[]>(total);
        memcpy(buf.get(), hdr, 10);
        fseek(f, 10, SEEK_SET);
        if (fread(buf.get()+10, 1, tagSize, f) != tagSize) return {};

        TrackTags tags;
        BufSpan apic;
        scanID3Buffer(buf.get(), total, apic, tags.title, tags.artist);

        if (apic.size)
            tags.art = {(long)apic.offset, apic.size};
        return tags;
    }

    static void readFLACVorbisComment(FILE *f, u32 blkLen,
                                      std::string &title, std::string &artist) {
        u8 tmp[4];
        if (fread(tmp,1,4,f)!=4) return;
        u32 vlen = le32(tmp);
        if (vlen > blkLen) return;
        fseek(f, (long)vlen, SEEK_CUR);

        if (fread(tmp,1,4,f)!=4) return;
        u32 count = le32(tmp);
        if (count > 2000) return;

        char buf[512];
        for (u32 i = 0; i < count; i++) {
            if (fread(tmp,1,4,f)!=4) return;
            u32 clen = le32(tmp);
            if (clen == 0) continue;
            if (clen > 65536) { fseek(f, (long)clen, SEEK_CUR); continue; }

            size_t toRead = std::min((size_t)clen, sizeof(buf)-1);
            if (fread(buf, 1, toRead, f) != toRead) return;
            buf[toRead] = '\0';
            if (clen > (u32)toRead) fseek(f, (long)(clen - toRead), SEEK_CUR);

            char *eq = strchr(buf, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *val = eq + 1;

            if      (title.empty()  && keycmp(buf, "TITLE"))  title  = val;
            else if (artist.empty() && keycmp(buf, "ARTIST")) artist = val;

            if (!title.empty() && !artist.empty()) return;
        }
    }

    static TrackTags readTagsFLAC(FILE *f) {
        TrackTags tags;
        fseek(f, 4, SEEK_SET);
        u8 bh[4];
        while (fread(bh,1,4,f)==4) {
            bool last = (bh[0]&0x80)!=0;
            u8   type = bh[0]&0x7F;
            u32  len  = ((u32)bh[1]<<16)|((u32)bh[2]<<8)|(u32)bh[3];
            if (len > 16*1024*1024) break;
            long blockDataStart = ftell(f);

            if (type == 6 && !tags.art.valid()) {
                u8 tmp[4];
                if (fread(tmp,1,4,f)==4 && fread(tmp,1,4,f)==4) {
                    u32 mlen = be32(tmp);
                    if (mlen <= len) {
                        fseek(f, (long)mlen, SEEK_CUR);
                        if (fread(tmp,1,4,f)==4) {
                            u32 dlen = be32(tmp);
                            if (dlen <= len) {
                                fseek(f, (long)dlen, SEEK_CUR);
                                fseek(f, 16, SEEK_CUR);
                                if (fread(tmp,1,4,f)==4) {
                                    u32 ilen = be32(tmp);
                                    if (ilen > 0) tags.art = {ftell(f), ilen};
                                }
                            }
                        }
                    }
                }
            } else if (type == 4 && (tags.title.empty() || tags.artist.empty())) {
                readFLACVorbisComment(f, len, tags.title, tags.artist);
            }

            if (last) break;
            if (tags.art.valid() && !tags.title.empty() && !tags.artist.empty()) break;
            fseek(f, blockDataStart + (long)len, SEEK_SET);
        }
        return tags;
    }

    static TrackTags readTagsWAV(FILE *f) {
        fseek(f, 12, SEEK_SET);
        u8 ch[8];
        while (fread(ch,1,8,f)==8) {
            u32 sz = le32(ch+4);
            if (sz > 16*1024*1024) break;
            if (memcmp(ch,"id3 ",4)==0 || memcmp(ch,"ID3 ",4)==0) {
                long chunkStart = ftell(f);
                auto buf = std::make_unique<u8[]>(sz);
                if (fread(buf.get(),1,sz,f)!=sz) return {};

                TrackTags tags;
                BufSpan apic;
                scanID3Buffer(buf.get(), sz, apic, tags.title, tags.artist);

                if (apic.size)
                    tags.art = {chunkStart + (long)apic.offset, apic.size};
                return tags;
            }
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
        return {};
    }

    struct FileRegion { FILE *f; long end; };

    static int  fr_read(void *u, char *buf, int n) {
        auto *r = static_cast<FileRegion*>(u);
        long avail = r->end - ftell(r->f);
        if (avail <= 0) return 0;
        if (n > (int)avail) n = (int)avail;
        return (int)fread(buf, 1, (size_t)n, r->f);
    }
    static void fr_skip(void *u, int n) { fseek(static_cast<FileRegion*>(u)->f, n, SEEK_CUR); }
    static int  fr_eof(void *u) {
        auto *r = static_cast<FileRegion*>(u);
        return ftell(r->f) >= r->end ? 1 : 0;
    }
    static constexpr stbi_io_callbacks k_stbi_cbs = { fr_read, fr_skip, fr_eof };

    static void toRGBA4444_from_RGB(const u8 *src, int sw, int sh, int d, u8 *dst) {
        const float x_scale = (float)(sw - 1) / (float)(d - 1);
        const float y_scale = (float)(sh - 1) / (float)(d - 1);
    
        for (int y = 0; y < d; y++) {
            const float fy = y * y_scale;
            const int   y0 = (int)fy;
            const int   y1 = y0 + 1 < sh ? y0 + 1 : y0;
            const float yw = fy - y0;
    
            const u8 *row0 = src + y0 * sw * 3;
            const u8 *row1 = src + y1 * sw * 3;
            u8 *out = dst + y * d * 2;
    
            for (int x = 0; x < d; x++) {
                const float fx = x * x_scale;
                const int   x0 = (int)fx;
                const int   x1 = x0 + 1 < sw ? x0 + 1 : x0;
                const float xw = fx - x0;
    
                // 4-sample bilinear blend per channel
                const u8 *p00 = row0 + x0 * 3;
                const u8 *p10 = row0 + x1 * 3;
                const u8 *p01 = row1 + x0 * 3;
                const u8 *p11 = row1 + x1 * 3;
    
                const float w00 = (1.0f - xw) * (1.0f - yw);
                const float w10 = xw           * (1.0f - yw);
                const float w01 = (1.0f - xw) * yw;
                const float w11 = xw           * yw;
    
                const u8 r = (u8)(p00[0]*w00 + p10[0]*w10 + p01[0]*w01 + p11[0]*w11 + 0.5f);
                const u8 g = (u8)(p00[1]*w00 + p10[1]*w10 + p01[1]*w01 + p11[1]*w11 + 0.5f);
                const u8 b = (u8)(p00[2]*w00 + p10[2]*w10 + p01[2]*w01 + p11[2]*w11 + 0.5f);
    
                out[x*2  ] = ((r >> 4) << 4) | (g >> 4);
                out[x*2+1] = ((b >> 4) << 4) | 0xF;
            }
        }
    }

} // namespace

// =============================================================================
// StatusBar
// =============================================================================

StatusBar::StatusBar() {
    if (R_FAILED(tuneGetRepeatMode(&this->m_repeat)))
        this->m_repeat = TuneRepeatMode_Off;
    if (R_FAILED(tuneGetShuffleMode(&this->m_shuffle)))
        this->m_shuffle = TuneShuffleMode_Off;

    if (R_SUCCEEDED(tuneGetCurrentQueueItem(path_buffer, FS_MAX_PATH, &this->m_stats))) {
        m_last_full_path = path_buffer;
        NullLastDot(path_buffer);
        const char *slash = std::strrchr(path_buffer, '/');
        m_song_title_str = slash ? (slash + 1) : path_buffer;
        m_artist_str     = "Unknown Artist";
    } else {
        path_buffer[0]   = '\0';
        this->m_stats    = {};
        m_song_title_str = "";   /* ⋯ */
        m_artist_str     = "";
    }

    m_isItem = false;
}

// ---------------------------------------------------------------------------

tsl::elm::Element *StatusBar::requestFocus(tsl::elm::Element *oldFocus, tsl::FocusDirection direction) {
    /* Tesla calls requestFocus(nullptr, None) to re-confirm focus on the
       active element after onClick returns true — oldFocus is NOT 'this'.
       Guard on m_focused + None direction instead: if we're already the
       focused element and no directional navigation is incoming, preserve
       m_active_btn exactly as-is so Up/Down zone transitions survive. */
    if (this->m_focused && direction == tsl::FocusDirection::None)
        return this;

    if (direction == tsl::FocusDirection::Right)
        m_active_btn = 0;
    else if (direction == tsl::FocusDirection::Left)
        m_active_btn = 4;
    else
        m_active_btn = 2;
    return this;
}

// ---------------------------------------------------------------------------
bool StatusBar::onClick(u64 keys) {
    if (keys & HidNpadButton_A) {
        /* KEY_R held = navigation mode: block OK so accidental presses
           don't activate buttons while the user is reaching for a page nav. */
        if (m_r_held) return true;
        if (m_active_btn != 5) {
            /* Determine sound BEFORE the action mutates state. */
            std::atomic<bool>& sound = [&]() -> std::atomic<bool>& {
                switch (m_active_btn) {
                    case 0: return (m_shuffle == TuneShuffleMode_Off) ? triggerOnSound : triggerOffSound;
                    case 1: return triggerOffSound;
                    case 2: return m_playing ? triggerOffSound : triggerOnSound;
                    case 3: return triggerOnSound;
                    case 4: return (static_cast<TuneRepeatMode>((m_repeat + 1) % TuneRepeatMode_Count) == TuneRepeatMode_Off) ? triggerOffSound : triggerOnSound;
                    default: return triggerOnSound;
                }
            }();
            ActivateButton(m_active_btn);
            triggerClickAnimation();
            triggerFeedbackImpl(triggerRumbleClick, sound);
        }
        return true;
    }
    if (keys & KEY_UP) {
        if (m_r_held) return true;  /* R held = nav mode, ignore vertical movement */
        /* From the button row → move focus up to the seek bar. */
        if (m_active_btn != 5) {
            m_active_btn = 5;
            m_hold_start_ns  = 0;
            m_last_repeat_ns = 0;
            triggerNavigationFeedback();
        }
        return true;  /* always consume — prevents List wrap-around */
    }
    if (keys & KEY_DOWN) {
        if (m_r_held) return true;  /* R held = nav mode, ignore vertical movement */
        if (m_active_btn == 5) {
            /* From seek bar → drop back down to the play button.
               Cancel any in-progress controller scrub without committing. */
            m_active_btn = 2;
            m_hold_start_ns  = 0;
            m_last_repeat_ns = 0;
            if (m_ctrl_scrubbing) {
                m_ctrl_scrubbing = false;
                m_seeking        = false;
            }
            triggerNavigationFeedback();
            return true;
        }
        /* On the button row — consume with no effect, no wrap-around. */
        return true;
    }
    if (keys & KEY_RIGHT) {
        if (m_active_btn == 5) { NudgeSeek(+1, 5); return true; }
        if (m_active_btn < 4) { m_active_btn++; triggerNavigationFeedback(); }
        else if (m_on_page_right) m_on_page_right();
        return true;
    }
    if (keys & KEY_LEFT) {
        if (m_r_held) return true;  /* R held = nav mode, ignore */
        if (m_active_btn == 5) { NudgeSeek(-1, 5); return true; }
        if (m_active_btn > 0) { m_active_btn--; triggerNavigationFeedback(); return true; }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// draw
// ---------------------------------------------------------------------------
void StatusBar::draw(tsl::gfx::Renderer *renderer) {
    const s32 art_sz  = ArtSize();
    const s32 art_off = ArtOffset();
    const s32 avail_w = this->getWidth() - 30;

    /* --- Album art --- */
    if (art_sz > 0) {
        ensureArtScaled(art_sz);
        if (!m_art_rgba4444.empty()) {
            const s32 art_x = this->getX() + 15 + (avail_w - art_sz) / 2;

            if (m_art_valid) {
                renderer->drawBitmapRGBA4444(art_x, this->getY() + 6,
                                             art_sz, art_sz, m_art_rgba4444.data(),
                                             tsl::gfx::Renderer::s_opacity, true);
            }

            /* Overlay a music note when no real art was found. */
            else {
                /* 4-px white border drawn inside the art rectangle. */
                constexpr s32 BORDER = 4;
                const s32 art_y = this->getY() + 6;

                // black fill FIRST (so border stays visible on top)
                renderer->drawRectAdaptive(art_x, art_y, art_sz, art_sz, a(0xA000));

                renderer->drawRect(art_x,                   art_y,                   art_sz, BORDER, a(0xffff)); /* top    */
                renderer->drawRect(art_x,                   art_y + art_sz - BORDER, art_sz, BORDER, a(0xffff)); /* bottom */
                renderer->drawRect(art_x,                   art_y,                   BORDER, art_sz, a(0xffff)); /* left   */
                renderer->drawRect(art_x + art_sz - BORDER, art_y,                   BORDER, art_sz, a(0xffff)); /* right  */

                constexpr u32         NOTE_FONT = 120;
                constexpr const char *NOTE      = "\u266B";
                const auto [note_w, note_h] = renderer->drawString(NOTE, false,
                                                  0, 0, NOTE_FONT,
                                                  tsl::style::color::ColorTransparent);
                const s32 note_x = art_x + (art_sz - (s32)note_w) / 2 -2;
                const s32 note_y = art_y  + (art_sz + (s32)note_h) / 2 -12;
                renderer->drawString(NOTE, false, note_x, note_y,
                                     NOTE_FONT, tsl::style::color::ColorText);
            }
        }
    }

    /* --- Song title (centered; scrolls when wider than available width) --- */
    if (this->m_text_width == 0) {
        const u32 titleW = renderer->drawString(m_song_title_str.c_str(), false,
                                                 0, 0, 23,
                                                 tsl::style::color::ColorTransparent).first;
        this->m_truncated = (s32)titleW > avail_w;
        if (this->m_truncated) {
            this->m_scroll_text = m_song_title_str + "       ";
            this->m_text_width  = renderer->drawString(this->m_scroll_text.c_str(), false,
                                                        0, 0, 23,
                                                        tsl::style::color::ColorTransparent).first;
            this->m_scroll_text += m_song_title_str;
        } else {
            this->m_text_width = titleW;
        }
    }

    const s32 title_y = this->getY() + art_sz + 4 + 20 + 10 + 6;

    if (this->m_truncated) {
        renderer->enableScissoring(this->getX() + 15, title_y - 22, avail_w, 28);
        renderer->drawString(this->m_scroll_text.c_str(), false,
            this->getX() + 15 - this->m_scroll_offset, title_y, 23,
            tsl::style::color::ColorText);
        renderer->disableScissoring();

        if (this->m_counter == 120) {
            if (this->m_scroll_offset >= this->m_text_width) {
                this->m_scroll_offset = 0;
                this->m_counter       = 0;
            } else {
                this->m_scroll_offset++;
            }
        } else {
            this->m_counter++;
        }
    } else {
        const s32 cx = this->getX() + 15 + (avail_w - (s32)this->m_text_width) / 2;
        renderer->drawString(m_song_title_str.c_str(), false,
            cx, title_y, 23, tsl::style::color::ColorText);
    }

    /* --- Artist (centered, font 18; scrolls when wider than available width) --- */
    if (this->m_artist_width == 0) {
        const u32 aw = renderer->drawString(m_artist_str.c_str(), false,
                                             0, 0, 18,
                                             tsl::style::color::ColorTransparent).first;
        this->m_artist_truncated = (s32)aw > avail_w;
        if (this->m_artist_truncated) {
            this->m_artist_scroll_text = m_artist_str + "       ";
            this->m_artist_width       = renderer->drawString(this->m_artist_scroll_text.c_str(), false,
                                                               0, 0, 18,
                                                               tsl::style::color::ColorTransparent).first;
            this->m_artist_scroll_text += m_artist_str;
        } else {
            this->m_artist_width = aw ? aw : 1u;
        }
    }

    const s32 artist_y = title_y + 24;

    if (this->m_artist_truncated) {
        renderer->enableScissoring(this->getX() + 15, artist_y - 18, avail_w, 22);
        renderer->drawString(this->m_artist_scroll_text.c_str(), false,
            this->getX() + 15 - (s32)this->m_artist_scroll_offset, artist_y, 18,
            tsl::style::color::ColorText);
        renderer->disableScissoring();

        if (this->m_artist_counter == 120) {
            if (this->m_artist_scroll_offset >= this->m_artist_width) {
                this->m_artist_scroll_offset = 0;
                this->m_artist_counter       = 0;
            } else {
                this->m_artist_scroll_offset++;
            }
        } else {
            this->m_artist_counter++;
        }
    } else {
        const s32 ax = this->getX() + 15 + (avail_w - (s32)this->m_artist_width) / 2;
        renderer->drawString(m_artist_str.c_str(), false,
            ax, artist_y, 18, tsl::style::color::ColorText);
    }

    /* --- Seek bar --- */
    const s32 bar_off = art_off + 10;
    const s32 bar_x   = this->getX() + 15;
    const s32 bar_y   = this->getY() + bar_off + tsl::style::ListItemDefaultHeight + 2;
    const s32 bar_len = this->getWidth() - 30;

    renderer->drawRect(bar_x, bar_y, bar_len, 3, a(0xffff));

    const float thumb_pct = this->m_seeking ? this->m_seek_percentage : this->m_percentage;
    if (thumb_pct > 0.0f)
        renderer->drawRect(bar_x, bar_y - 2,
                           static_cast<s32>(bar_len * thumb_pct), 7, a(0xf00f));

    const s32 thumb_x = bar_x + static_cast<s32>(bar_len * thumb_pct);

    /* --- Seek-bar focus highlight (drawn behind thumb) --- */
    if (this->m_focused && m_active_btn == 5) {
        const u64    now_ns = ult::nowNs();
        const double t      = static_cast<double>(now_ns) / 1000000000.0;
        const auto   prog   = ((ult::cos(2.0 * ult::_M_PI * std::fmod(t, 1.0) - ult::_M_PI / 2) + 1.0) / 2.0);
        const auto   hcol   = m_r_held
            ? lerpColor(tsl::highlightColor3, tsl::highlightColor4, prog)
            : lerpColor(tsl::highlightColor1, tsl::highlightColor2, prog);
        /* Radius = thumb radius (7) + 4 px border. No black fill — highlight
           peeks out 4 px behind the white thumb circle. */
        renderer->drawCircle(thumb_x, bar_y + 2, 7 + 4, true, a(hcol));
    }

    renderer->drawCircle(thumb_x, bar_y + 2, 7, true, a(0xffff));

    /* --- Time labels --- */
    /* While controller-scrubbing, derive the preview time from m_seek_percentage
       so the left timestamp tracks the thumb in real time. */
    const s32 label_y = this->getY() + bar_off + CenterOfLine(1) + 9;

    if (m_ctrl_scrubbing && m_stats.total_frames > 0 && m_stats.sample_rate > 0) {
        const u32 preview_frame = static_cast<u32>(m_seek_percentage * float(m_stats.total_frames));
        const u32 preview_sec   = preview_frame / m_stats.sample_rate;
        char preview_buf[0x20];
        std::snprintf(preview_buf, sizeof(preview_buf), "%d:%02d", preview_sec / 60, preview_sec % 60);
        renderer->drawString(preview_buf, false,
            bar_x, label_y, 20, 0xffff);
    } else {
        renderer->drawString(current_buffer, false,
            bar_x, label_y, 20, 0xffff);
    }

    // Right-align the total duration to the bar's right edge.
    // Measure the actual text width so any duration string fits precisely.
    {
        const s32 bar_right = bar_x + bar_len;
        const s32 total_w   = static_cast<s32>(
            renderer->getTextDimensions(total_buffer, false, 20).first);
        renderer->drawString(total_buffer, false,
            bar_right - total_w, label_y, 20, 0xffff);
    }

    /* --- Focus highlight (button row) --- */
    if (this->m_focused && m_active_btn != 5) {
        const u64    now_ns = ult::nowNs();
        const double t      = static_cast<double>(now_ns) / 1000000000.0;
        const auto   progress = ((ult::cos(2.0 * ult::_M_PI * std::fmod(t, 1.0) - ult::_M_PI / 2) + 1.0) / 2.0);

        tsl::Color highlightColor;
        if (this->m_clickAnimationProgress > 0) {
            /* Replicate drawClickAnimation's color cycle exactly:
               highlightColor1 → clickColor → highlightColor2 */
            const double cp = (ult::cos(2.0 * ult::_M_PI * std::fmod(now_ns / 1000000000.0 - 0.25, 1.0)) + 1.0) / 2.0;
            tsl::Color c1 = tsl::highlightColor1;
            tsl::Color c2 = tsl::clickColor;
            if (cp >= 0.5) { c1 = tsl::clickColor; c2 = tsl::highlightColor2; }
            highlightColor = lerpColor(c1, c2, cp);
        } else if (m_r_held) {
            highlightColor = lerpColor(tsl::highlightColor3, tsl::highlightColor4, progress);
        } else {
            highlightColor = lerpColor(tsl::highlightColor1, tsl::highlightColor2, progress);
        }

        auto [cx, cy] = ButtonCenter(m_active_btn);
        const s32 btn_r = (m_active_btn == 2) ? 24
                        : (m_active_btn == 0 || m_active_btn == 4) ? 18 : 20;

        // Compute saturation color for inner circle
        const u64 btnClickElapsed = (m_btn_click_idx == m_active_btn)
            ? (now_ns - m_btn_click_start_ns) : 0;
        float btnClickProgress = (m_btn_click_idx == m_active_btn)
            ? tsl::style::ListItemHighlightLength
              * (1.0f - (static_cast<float>(btnClickElapsed) / 500000000.0f))
            : 0.0f;
        if (btnClickProgress < 0.0f) btnClickProgress = 0.0f;

        tsl::Color innerColor = tsl::style::color::ColorClickAnimation;
        if (btnClickProgress > 0.0f) {
            const u8 sat = tsl::style::ListItemHighlightSaturation
                * (btnClickProgress / static_cast<float>(tsl::style::ListItemHighlightLength));
            tsl::Color animColor = {0xF, 0xF, 0xF, 0xF};
            if (tsl::invertBGClickColor) {
                animColor.r = 15 - sat;
                animColor.g = 15 - sat;
                animColor.b = 15 - sat;
            } else {
                animColor.r = sat;
                animColor.g = sat;
                animColor.b = sat;
            }
            animColor.a = innerColor.a;
            innerColor = animColor;
        }

        renderer->drawCircle(cx, cy, btn_r + 4, true, a(highlightColor));
        renderer->drawCircle(cx, cy, btn_r,     true, a(innerColor));
    }

    /* --- Touch-activated button saturation (unfocused or different from active_btn) --- */
    {
        const u64 now_ns = ult::nowNs();
        if (m_btn_click_idx >= 0) {
            const u64 btnClickElapsed = now_ns - m_btn_click_start_ns;
            float btnClickProgress = tsl::style::ListItemHighlightLength
                * (1.0f - (static_cast<float>(btnClickElapsed) / 500000000.0f));
            if (btnClickProgress < 0.0f) {
                btnClickProgress  = 0.0f;
                m_btn_click_idx   = -1;
            }
            if (btnClickProgress > 0.0f
                && (!this->m_focused || m_btn_click_idx != m_active_btn)) {
                auto [acx, acy] = ButtonCenter(m_btn_click_idx);
                const s32 abtn_r = (m_btn_click_idx == 2) ? 24
                                 : (m_btn_click_idx == 0 || m_btn_click_idx == 4) ? 18 : 20;
                const u8 sat = tsl::style::ListItemHighlightSaturation
                    * (btnClickProgress / static_cast<float>(tsl::style::ListItemHighlightLength));
                if (sat > 0) {
                    tsl::Color animColor = {0xF, 0xF, 0xF, sat};
                    renderer->drawCircle(acx, acy, abtn_r, true, a(animColor));
                }
            }
        }
    }

    /* --- Control symbols --- */
    const auto repeat_color = this->m_repeat ? tsl::onTextColor
                                              : tsl::offTextColor;
    if (this->m_repeat == TuneRepeatMode_One)
        symbol::repeat::one::symbol.draw(GetRepeatX(), GetRepeatY(), renderer, repeat_color);
    else
        symbol::repeat::all::symbol.draw(GetRepeatX(), GetRepeatY(), renderer, repeat_color);

    const auto shuffle_color = this->m_shuffle ? tsl::onTextColor
                                               : tsl::offTextColor;
    symbol::shuffle::symbol.draw(GetShuffleX(), GetShuffleY(), renderer, shuffle_color);
    symbol::prev::symbol.draw  (GetPrevX(),     GetPrevY(),    renderer, tsl::style::color::ColorText);
    this->GetPlaybackSymbol().draw(GetPlayStateX() + (this->m_playing ? 0 : 2),
                                   GetPlayStateY(), renderer, tsl::style::color::ColorText);
    symbol::next::symbol.draw  (GetNextX(),     GetNextY(),    renderer, tsl::style::color::ColorText);
}

// ---------------------------------------------------------------------------

void StatusBar::layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) {
    s32 new_w = this->getWidth() + 9;
    s32 art   = (new_w - 30) * 9 / 10;
    s32 new_h = art + 14 + tsl::style::ListItemDefaultHeight * 3;
    this->setBoundaries(this->getX() + 3, this->getY(), new_w, new_h);

    if (m_art_scaled_size == 0 && !m_last_full_path.empty())
        loadArt(m_last_full_path.c_str());
}

// ---------------------------------------------------------------------------

#define TOUCHED(button) \
    (currX > (Get##button##X() - 30) && currX < (Get##button##X() + 30) && \
     currY > (Get##button##Y() - 30) && currY < (Get##button##Y() + 30))

bool StatusBar::onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY,
                         s32 prevX, s32 prevY, s32 initialX, s32 initialY) {
    s32 barX      = this->getX() + 15;
    s32 barY      = this->getY() + ArtOffset() + tsl::style::ListItemDefaultHeight + 1;
    s32 barLength = static_cast<s32>(this->getWidth() - 30);

    const float thumb_pct = this->m_seeking ? this->m_seek_percentage : this->m_percentage;
    const s32   thumbX    = barX + static_cast<s32>(barLength * thumb_pct);
    const bool  nearThumb = (std::abs(currX - thumbX) <= 20 && std::abs(currY - barY) <= 20);

    if (event == tsl::elm::TouchEvent::Touch && nearThumb)
        this->m_seeking = true;

    if (this->m_seeking) {
        s32 clampedX = std::max(barX, std::min(currX, barX + barLength));
        this->m_seek_percentage = float(clampedX - barX) / float(barLength);
        if (event == tsl::elm::TouchEvent::Release) {
            tuneSeek(static_cast<u32>(this->m_seek_percentage * float(this->m_stats.total_frames)));
            this->m_percentage = this->m_seek_percentage;
            this->m_seeking    = false;
        }
        return true;
    }

    if (event == tsl::elm::TouchEvent::Touch)
        this->m_touched = this->inBounds(currX, currY);

    if (event == tsl::elm::TouchEvent::Release && this->m_touched) {
        this->m_touched = false;
        if (Element::getInputMode() == tsl::InputMode::Touch) {
            u16 handled = 0;
            std::atomic<bool> *touchSound = &triggerOnSound;
            if (TOUCHED(Repeat))    { touchSound = &((m_repeat + 1) % TuneRepeatMode_Count == TuneRepeatMode_Off ? triggerOffSound : triggerOnSound); ActivateButton(4); handled++; }
            if (TOUCHED(Shuffle))   { touchSound = &(m_shuffle == TuneShuffleMode_Off ? triggerOnSound : triggerOffSound); ActivateButton(0); handled++; }
            if (TOUCHED(PlayState)) { touchSound = &(m_playing ? triggerOffSound : triggerOnSound); ActivateButton(2); handled++; }
            if (TOUCHED(Prev))      { touchSound = &triggerOffSound; ActivateButton(1); handled++; }
            if (TOUCHED(Next))      { touchSound = &triggerOnSound;  ActivateButton(3); handled++; }
            if (handled > 0) {
                this->m_clickAnimationProgress = 0;
                ult::shortTouchAndRelease.store(true, std::memory_order_release);
                triggerFeedbackImpl(triggerRumbleClick, *touchSound);
                return true;
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------
void StatusBar::update() {
    /* Drain any queued seek first — tuneSeek must run on the main thread
       (the IPC session is not thread-safe).  NudgeSeek and scrub-commit
       store the target here so the UI never blocks on the IPC call. */
    const u32 pending = m_pending_seek_frame.exchange(UINT32_MAX, std::memory_order_acq_rel);
    if (pending != UINT32_MAX)
        tuneSeek(pending);

    if (R_FAILED(tuneGetStatus(&this->m_playing)))
        this->m_playing = false;

    if (R_SUCCEEDED(tuneGetCurrentQueueItem(path_buffer, FS_MAX_PATH, &this->m_stats))) {
        const size_t length = std::strlen(path_buffer);
        char fullPath[FS_MAX_PATH];
        memcpy(fullPath, path_buffer, length + 1);
        NullLastDot(path_buffer);

        if (m_last_full_path != fullPath) {
            const char *slash    = std::strrchr(path_buffer, '/');
            const char *filename = slash ? (slash + 1) : path_buffer;
            m_song_title_str      = filename;
            m_artist_str          = "Unknown Artist";
            m_text_width          = 0;
            m_artist_width        = 0;
            m_scroll_offset       = 0;
            m_counter             = 0;
            m_artist_scroll_offset = 0;
            m_artist_counter      = 0;
            m_artist_truncated    = false;

            if (loadArt(fullPath))
                m_last_full_path = fullPath;
        }
    } else {
        if (!m_last_full_path.empty()) {
            m_last_full_path.clear();
            m_art_path.clear();
            m_art_valid       = false;
            m_art_scaled_size = 0;
            m_art_rgba4444.clear();
            m_art_rgba4444.shrink_to_fit();
        }
        m_song_title_str      = "";   /* ⋯ */
        m_artist_str          = "";
        m_text_width          = 0;
        m_artist_width        = 0;
        m_scroll_offset       = 0;
        m_counter             = 0;
        m_artist_scroll_offset = 0;
        m_artist_counter      = 0;
        m_artist_truncated    = false;
        this->m_stats    = {};
    }

    const u32 current = this->m_stats.current_frame / this->m_stats.sample_rate;
    const u32 total   = this->m_stats.total_frames   / this->m_stats.sample_rate;
    this->m_percentage = std::clamp(
        float(this->m_stats.current_frame) / float(this->m_stats.total_frames),
        0.0f, 1.0f);

    std::snprintf(current_buffer, sizeof(current_buffer), "%d:%02d", current / 60, current % 60);
    std::snprintf(total_buffer,   sizeof(total_buffer),   "%d:%02d", total   / 60, total   % 60);
}

// ---------------------------------------------------------------------------
// loadArt
// ---------------------------------------------------------------------------
bool StatusBar::loadArt(const char *fullPath) {
    const s32 sz = ArtSize();
    if (sz <= 0) return false;

    m_art_path        = fullPath;
    m_art_valid       = false;
    m_art_scaled_size = 0;

    const size_t needed = (size_t)sz * sz * 2;
    m_art_rgba4444.resize(needed);

    /* Black opaque in RGBA4444: byte0=0x00 (R4|G4), byte1=0x0F (B4|A4).
       Not a uniform byte value so we fill in pairs. */
    auto fill_placeholder = [&]() {
        u8 *p = m_art_rgba4444.data(), *pend = p + needed;
        while (p < pend) { *p++ = 0x00; *p++ = 0x0F; }
        m_art_scaled_size = sz;
    };

    FILE *f = fopen(fullPath, "rb");
    if (!f) { fill_placeholder(); return true; }

    u8 magic[4] = {};
    fread(magic, 1, 4, f);

    TrackTags tags;
    if      (memcmp(magic, "ID3",  3) == 0) tags = readTagsMP3(f);
    else if (memcmp(magic, "fLaC", 4) == 0) tags = readTagsFLAC(f);
    else if (memcmp(magic, "RIFF", 4) == 0) tags = readTagsWAV(f);

    if (!tags.title.empty()) {
        m_song_title_str = std::move(tags.title);
        m_text_width     = 0;
    }
    m_artist_str          = tags.artist.empty() ? "Unknown Artist" : std::move(tags.artist);
    m_artist_width        = 0;
    m_artist_scroll_offset = 0;
    m_artist_counter      = 0;
    m_artist_truncated    = false;

    if (!tags.art.valid()) {
        fclose(f);
        fill_placeholder();
        return true;
    }

    fseek(f, tags.art.offset, SEEK_SET);
    FileRegion fr{ f, tags.art.offset + (long)tags.art.size };
    int w, h, ch_unused;
    u8 *px = stbi_load_from_callbacks(&k_stbi_cbs, &fr, &w, &h, &ch_unused, 3);
    fclose(f);

    if (!px || w > 2048 || h > 2048) {
        if (px) stbi_image_free(px);
        fill_placeholder();
        return true;
    }

    toRGBA4444_from_RGB(px, w, h, sz, m_art_rgba4444.data());
    stbi_image_free(px);

    m_art_scaled_size = sz;
    m_art_valid       = true;
    return true;
}

// ---------------------------------------------------------------------------
// ensureArtScaled
// ---------------------------------------------------------------------------
void StatusBar::ensureArtScaled(s32 size) {
    if (size <= 0 || size == m_art_scaled_size) return;
    const size_t needed = (size_t)size * size * 2;
    m_art_rgba4444.resize(needed);
    u8 *p = m_art_rgba4444.data(), *pend = p + needed;
    while (p < pend) { *p++ = 0x00; *p++ = 0x0F; }
    m_art_scaled_size = size;
}

// ---------------------------------------------------------------------------

void StatusBar::ActivateButton(int i) {
    m_btn_click_idx      = i;
    m_btn_click_start_ns = ult::nowNs();
    switch (i) {
        case 0: CycleShuffle(); break;
        case 1: Prev();         break;
        case 2: CyclePlay();    break;
        case 3: Next();         break;
        case 4: CycleRepeat();  break;
    }
}

void StatusBar::CycleRepeat() {
    this->m_repeat = static_cast<TuneRepeatMode>((this->m_repeat + 1) % TuneRepeatMode_Count);
    config::set_repeat(this->m_repeat);
    tuneSetRepeatMode(this->m_repeat);
}

void StatusBar::CycleShuffle() {
    this->m_shuffle = static_cast<TuneShuffleMode>((this->m_shuffle + 1) % TuneShuffleMode_Count);
    config::set_shuffle(this->m_shuffle);
    tuneSetShuffleMode(this->m_shuffle);
}

void StatusBar::CyclePlay() {
    if (this->m_playing) tunePause();
    else                 tunePlay();
}

void StatusBar::Prev() {
    /* Spotify-style back behaviour:
     *   - If more than 3 seconds into the track → restart from the beginning.
     *   - If within the first 3 seconds (or no timing info available) → go to
     *     the actual previous track.
     *
     * The seek is queued via m_pending_seek_frame so it runs on the main thread
     * (same pattern as NudgeSeek), keeping libnx IPC thread-safe. */
    if (m_stats.total_frames > 0 && m_stats.sample_rate > 0
        && m_stats.current_frame > m_stats.sample_rate * 3u) {

        /* Seek to frame 0 — restart the current track. */
        m_pending_seek_frame.store(0u, std::memory_order_release);

        /* Update visuals immediately so the scrub thumb and timestamp snap
           to 0:00 on the very next frame without waiting for update(). */
        m_stats.current_frame = 0;
        m_percentage          = 0.0f;
        std::snprintf(current_buffer, sizeof(current_buffer), "0:00");
    } else {
        tunePrev();
    }
}
void StatusBar::Next() { tuneNext(); }

void StatusBar::NudgeSeek(int direction, u32 seconds) {
    if (this->m_stats.total_frames == 0 || this->m_stats.sample_rate == 0) return;

    const u32 step    = this->m_stats.sample_rate * seconds;
    const u32 current = this->m_stats.current_frame;
    const u32 target  = (direction > 0)
        ? std::min(current + step, this->m_stats.total_frames)
        : (current > step ? current - step : 0u);

    /* Queue for the main thread — libnx IPC is not thread-safe.
       update() drains this every tick. */
    m_pending_seek_frame.store(target, std::memory_order_release);

    /* Update visuals immediately so thumb and timestamp move on next frame. */
    this->m_stats.current_frame = target;
    this->m_percentage = float(target) / float(this->m_stats.total_frames);
    if (this->m_stats.sample_rate > 0) {
        const u32 sec = target / this->m_stats.sample_rate;
        std::snprintf(current_buffer, sizeof(current_buffer),
                      "%d:%02d", sec / 60, sec % 60);
    }

    triggerNavigationFeedback();
}

const AlphaSymbol &StatusBar::GetPlaybackSymbol() {
    return this->m_playing ? symbol::pause::symbol : symbol::play::symbol;
}

// ---------------------------------------------------------------------------
// onHeld — called every frame from MainGui::handleInput while a key is held.
// Implements hold-to-repeat for Left/Right on both the button row and seekbar.
// ---------------------------------------------------------------------------
void StatusBar::onHeld(u64 keysHeld) {
    m_r_held = (keysHeld & KEY_R) != 0;
    g_player_r_held.store(m_r_held, std::memory_order_release);

    if (m_r_held) {
        m_hold_start_ns  = 0;
        m_last_repeat_ns = 0;
        if (m_ctrl_scrubbing) {
            m_ctrl_scrubbing = false;
            m_seeking        = false;
        }
        return;
    }

    const bool holdLeft  = (keysHeld & KEY_LEFT)  != 0;
    const bool holdRight = (keysHeld & KEY_RIGHT) != 0;

    if (!holdLeft && !holdRight) {
        /* Key released — commit any in-progress controller scrub. */
        if (m_ctrl_scrubbing) {
            const u32 committed = static_cast<u32>(m_seek_percentage * float(m_stats.total_frames));
            m_pending_seek_frame.store(committed, std::memory_order_release);
            m_percentage              = m_seek_percentage;
            m_stats.current_frame     = committed;
            m_ctrl_scrubbing          = false;
            m_seeking                 = false;

            /* Write current_buffer immediately so draw() never briefly
               shows the pre-scrub time before the next update() tick. */
            if (m_stats.sample_rate > 0) {
                const u32 sec = committed / m_stats.sample_rate;
                std::snprintf(current_buffer, sizeof(current_buffer),
                              "%d:%02d", sec / 60, sec % 60);
            }
        }
        m_hold_start_ns  = 0;
        m_last_repeat_ns = 0;
        return;
    }

    const u64 now = ult::nowNs();

    if (m_hold_start_ns == 0) {
        m_hold_start_ns  = now;
        m_last_repeat_ns = now;
        return;
    }

    /* A tap on a physical button typically lasts 50-80 ms (3-5 frames at 60 fps).
       400 ms is clearly "intentional hold" and will never fire on a quick press. */
    constexpr u64 HOLD_DELAY_NS  = 400'000'000ULL;  /* 400 ms initial delay       */
    constexpr u64 BTN_REPEAT_NS  = 100'000'000ULL;  /* 100 ms between button steps */
    constexpr u64 SEEK_REPEAT_NS =  80'000'000ULL;  /* ~12 ticks/s seek scrub rate */

    if (now - m_hold_start_ns < HOLD_DELAY_NS) return;

    const u64 interval = (m_active_btn == 5) ? SEEK_REPEAT_NS : BTN_REPEAT_NS;
    if (now - m_last_repeat_ns < interval) return;

    m_last_repeat_ns = now;

    if (m_active_btn == 5) {
        /* --- Seekbar hold: preview-only, commit on release --- */
        if (!m_ctrl_scrubbing) {
            /* First repeat tick — initialise the preview from current position. */
            m_seek_percentage = m_percentage;
            m_ctrl_scrubbing  = true;
            m_seeking         = true;
        }

        if (m_stats.total_frames > 0 && m_stats.sample_rate > 0) {
            /* Accelerating step: starts at 1 s/tick, ramps to 10 s/tick over
               ~4.5 s of active holding.  hold_active_s is measured from the
               moment the initial delay expired, not from the first keypress. */
            const float hold_active_s = float(now - (m_hold_start_ns + HOLD_DELAY_NS)) / 1e9f;
            const float seconds_per_tick = std::min(1.0f + hold_active_s * 2.0f, 10.0f);
            const float step = (float(m_stats.sample_rate) * seconds_per_tick)
                               / float(m_stats.total_frames);
            m_seek_percentage = holdRight
                ? std::min(m_seek_percentage + step, 1.0f)
                : std::max(m_seek_percentage - step, 0.0f);
        }
        triggerNavigationFeedback();
    } else {
        /* --- Button row hold: cycle through buttons --- */
        if (holdRight && m_active_btn < 4) {
            m_active_btn++;
            triggerNavigationFeedback();
        } else if (holdLeft && m_active_btn > 0) {
            m_active_btn--;
            triggerNavigationFeedback();
        }
        /* Don't fire page-right during hold. */
    }
}