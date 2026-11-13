// Nexus wire protocol (server <-> client). Shared by server/nexus_mm.cpp and client/nexus_client.cpp.
// Server -> client: one user message named "Nexus" (variable size, <= 190 bytes) holding one or more ops.
// Client -> server: console commands  nexus_hello / nexus_key / nexus_click / nexus_data.
#pragma once
#define NEXUS_VERSION   "1.1.0"
#define NEXUS_PROTO     1
#define NEXUS_MSG_NAME  "Nexus"
#define NEXUS_CVAR      "nexus_version"      // client cvar the server queries to detect Nexus
#define NEXUS_MAX_MSG   190
#define NEXUS_MAX_TEXT  1024
#define NEXUS_MAX_ELEMS 512
#define NEXUS_MAX_NAME  63

enum { NX_OP_HELLO = 1,   // u16 token                          -> client answers: nexus_hello <proto> <version> <w> <h> <caps> <token>
       NX_OP_RESET = 2,   // -                                   remove everything, cursor off, no key subscriptions
       NX_OP_SET = 3,     // u16 id, u8 type, u16 mask, fields   create or update an element (only the masked fields change)
       NX_OP_DEL = 4,     // u16 id
       NX_OP_CLEAR = 5,   // u8 group (255 = every group)
       NX_OP_ANIM = 6,    // u16 id, u8 prop, target, u16 duration (1/100 s), u8 ease, u8 flags (1 = delete element when done)
       NX_OP_TEXT = 7,    // u16 id, u8 flags (1 = append), str  long texts are sent in pieces
       NX_OP_KEYS = 8,    // u8 n, n * (u8 keynum, u8 flags)     replaces the key subscription list (flags: 1 = swallow the key)
       NX_OP_CURSOR = 9,  // u8 on
       NX_OP_SOUND = 10,  // u8 volume (0-255), str name         plays sound/<name> (.wav) locally
       NX_OP_CAMERA = 11, // u8 mode, u16 lerp (1/100 s), u8 flags (1 = collide with the world), 6 * i16:
                          //    mode 0 off | 1 third person: distance, height, side, 0, 0, 0 | 2 fixed: x, y, z, pitch*10, yaw*10, roll*10
       NX_OP_MUSIC = 12,  // u8 mode (0 stop, 1 play once, 2 loop), str file (.mp3, relative to the game dir)
       NX_OP_DATA = 13,   // str channel, str bytes              delivered to client plugins; reply: nexus_data <channel> <hex>
       NX_OP_URL = 14 };  // u16 request, str url (https only), str label
                          //    the CLIENT asks the player (its own window, keys Y/N, cannot be answered by the server);
                          //    reply: nexus_url <request> <result>   result: 1 opened, 0 declined / timed out, 2 links disabled (nexus_urls 0), 3 busy / too soon, 4 bad url

enum { NX_T_TEXT = 1, NX_T_RECT = 2, NX_T_IMAGE = 3, NX_T_BAR = 4 };

// field mask of NX_OP_SET, fields follow in this order.   i16 fractions are 1/10000 of the screen width/height.
enum { NX_F_POS = 1,      // i16 x_frac, i16 y_frac, i16 x_px, i16 y_px
       NX_F_SIZE = 2,     // i16 w_frac, i16 h_frac, i16 w_px, i16 h_px      (0,0,0,0 = natural size)
       NX_F_COLOR = 4,    // r g b a
       NX_F_COLOR2 = 8,   // r g b a        text: background box, bar: track colour
       NX_F_STYLE = 16,   // u8 anchor (0-8), u8 flags, i8 layer, u8 group
       NX_F_SCALE = 32,   // u16 scale * 256
       NX_F_TEXT = 64,    // str            (u8 length + bytes, UTF-8)
       NX_F_IMAGE = 128,  // str            file relative to the game dir: .png .tga .bmp .jpg
       NX_F_VALUE = 256,  // u16 0..10000   bar fill
       NX_F_WORLD = 512,  // u16 entity (0 = fixed position), i16 x, i16 y, i16 z (offset from the entity, or world position), u16 max distance (0 = no limit)
       NX_F_LIFE = 1024 };// u16 lifetime (1/100 s, 0 = forever), u8 fade in (1/20 s), u8 fade out (1/20 s)

enum { NX_FL_SHADOW = 1, NX_FL_OUTLINE = 2, NX_FL_CLICK = 4, NX_FL_OCCLUDE = 8, NX_FL_ADDITIVE = 16, NX_FL_HIDDEN = 32, NX_FL_DISTSCALE = 64, NX_FL_MARKUP = 128 };   // MARKUP: ^0..^9 in a text switch colour (^^ = a literal ^)
enum { NX_A_POS = 1, NX_A_PX = 2, NX_A_ALPHA = 3, NX_A_SCALE = 4, NX_A_VALUE = 5, NX_A_COLOR = 6 };   // anim props: (i16,i16) (i16,i16) (u8) (u16) (u16) (rgba)
enum { NX_CAP_GL = 1, NX_CAP_CURSOR = 2, NX_CAP_WORLD = 4, NX_CAP_IMAGES = 8, NX_CAP_CAMERA = 16, NX_CAP_PLUGINS = 32, NX_CAP_URL = 64 };
#define NEXUS_MAX_DATA 96        // bytes per data message (both directions)
#define NEXUS_MAX_KEYS 48
