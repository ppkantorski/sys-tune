#include "tag_reader.hpp"

#include <cstring>
#include <algorithm>
#include <memory>
#include <switch.h>   // u8, u16, u32

namespace {

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

// ---- ID3v2 (MP3) ------------------------------------------------------------

static TitleArtist ta_readID3(FILE *f) {
    fseek(f, 0, SEEK_SET);
    u8 hdr[10];
    if (fread(hdr,1,10,f)!=10 || memcmp(hdr,"ID3",3)!=0) return {};
    u8  ver     = hdr[3];
    u8  flags   = hdr[5];
    u32 tagSize = ta_syncsafe(hdr+6);
    if (tagSize > 16*1024*1024) return {};
    // Cap at 64 KB — title/artist frames are always near the front.
    // Only embedded album art pushes tags beyond this, and we don't need it.
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

// ---- FLAC / Vorbis comment --------------------------------------------------

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

// ---- WAV / ID3-in-RIFF ------------------------------------------------------

static TitleArtist ta_readWAV(FILE *f) {
    fseek(f, 12, SEEK_SET);
    u8 ch[8];
    while (fread(ch,1,8,f)==8) {
        u32 sz = ta_le32(ch+4);
        if (sz > 16*1024*1024) break;
        if (memcmp(ch,"id3 ",4)==0 || memcmp(ch,"ID3 ",4)==0) {
            // Cap at 64 KB — same reasoning as ta_readID3.
            const u32 readSz = std::min(sz, (u32)(64 * 1024));
            auto buf = std::make_unique<u8[]>(readSz);
            if (fread(buf.get(),1,readSz,f)!=readSz) return {};
            if (readSz < 10 || memcmp(buf.get(),"ID3",3)!=0) return {};

            u8  ver     = buf[3];
            u8  flags   = buf[5];
            u32 tagSize = ta_syncsafe(buf.get()+6);
            if (tagSize+10 > readSz) return {};

            TitleArtist ta;
            const u8 *data = buf.get();
            size_t pos = 10;
            if ((flags & 0x40) && ver >= 3) {
                if (pos+4 > readSz) return ta;
                u32 exSz = (ver==4) ? ta_syncsafe(data+pos) : ta_be32(data+pos);
                pos += exSz;
            }
            size_t end = std::min((size_t)(10+tagSize), (size_t)readSz);
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

} // anonymous namespace

// ---- Public entry point -----------------------------------------------------

TitleArtist readTitleArtist(const char *path) {
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
        const size_t dot = ta.title.rfind('.');
        if (dot != std::string::npos && dot > 0)
            ta.title.resize(dot);
    }
    if (ta.artist.empty())
        ta.artist = "Unknown Artist";

    return ta;
}