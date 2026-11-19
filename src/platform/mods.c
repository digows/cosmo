#include "cosmo/mods.h"
#include "cosmo/group.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define PATH_SIZE 1024
#define JSON_SIZE (64u * 1024u)
#define OVERRIDE_MAX_SIZE (16u * 1024u * 1024u)

static cosmo_mod mods[COSMO_MOD_LIMIT];
static size_t mod_count;
static char settings_file[PATH_SIZE];
static bool gamepad_rumble = true;
static char player_colours_file[PATH_SIZE];
static int player_colours[2] = {-1, -1};

static const struct {
    const char *name;
    uint8_t shadow, light;
} player_colour_options[COSMO_PLAYER_COLOUR_COUNT] = {
    {"Green", 2, 10}, {"Red", 4, 12}, {"Blue", 1, 9}, {"Cyan", 3, 11},
    {"Purple", 1, 13}, {"Gold", 6, 14}, {"Silver", 8, 15}
};

static const uint8_t player_shadows[2] = {2, 4};
static const uint8_t player_lights[2] = {10, 12};

static bool valid_player_colour(int colour)
{
    return colour >= 0 && colour < COSMO_PLAYER_COLOUR_COUNT;
}

static const struct {
    const char *key;
    enum cosmo_mod_feature flag;
} features[] = {
    {"auroraPalette", MOD_AURORA_PALETTE},
    {"lowGravity", MOD_LOW_GRAVITY},
    {"shieldMode", MOD_SHIELD_MODE},
    {"turboMode", MOD_TURBO_MODE},
    {"widescreen", MOD_WIDESCREEN},
};

static const char *const sound_asset_names[65] = {
    "big_prize", "player_jump", "player_land", "player_cling",
    "player_hit_head", "player_pounce", "player_death", "door_unlock",
    "spikes_move", "explosion", "win_level", "barrel_destroy_1", "prize",
    "player_hurt", "foot_switch_move", "foot_switch_on", "roamer_gift",
    "destroy_satellite", "player_footstep", "push_player", "drip",
    "pipe_corner_hit", "transporter_on", "scooter_putt", "destroy_solid",
    "projectile_launch", "big_object_hit", "no_bombs", "place_bomb",
    "hint_dialog_alert", "red_jumper_jump", "red_jumper_land",
    "bghost_egg_crack", "bghost_egg_hatch", "saw_blade_move",
    "fireball_launch", "object_hit", "exit_monster_open",
    "exit_monster_ingest", "bear_trap_close", "pause_game", "weeeeeeee",
    "jump_pad_robot", "text_typewriter", "bonus_stage", "shard_bounce",
    "tulip_launch", "new_game", "rocket_burn", "smash",
    "high_score_display", "high_score_set", "ivy_plant_rise", "flame_pulse",
    "boss_damage", "boss_move", "speech_bubble", "baby_ghost_jump",
    "baby_ghost_land", "thunder", "barrel_destroy_2", "tulip_ingest",
    "plant_mouth_open", "entering_level_num", "boss_launch"
};

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static bool json_string(const char **cursor, char *out, size_t cap)
{
    const char *p = ws(*cursor);
    size_t n = 0;
    if (*p++ != '"') return false;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c < 0x20) return false;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (c == '"' || c == '\\' || c == '/') { }
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == 'u') {
                /* IDs and display text are UTF-8 in manifests. Keep escaped
                 * ASCII codepoints; replace other escapes safely. */
                unsigned value = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = *p++;
                    value <<= 4;
                    if (h >= '0' && h <= '9') value |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') value |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') value |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                c = value < 0x80 ? (unsigned char)value : '?';
            } else return false;
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    if (*p++ != '"') return false;
    if (cap) out[n] = '\0';
    *cursor = p;
    return true;
}

static bool skip_value(const char **cursor, int depth)
{
    const char *p = ws(*cursor);
    if (depth > 32 || !*p) return false;
    if (*p == '"') {
        char scratch[2];
        if (!json_string(&p, scratch, sizeof scratch)) return false;
    } else if (*p == '{' || *p == '[') {
        char close = *p++ == '{' ? '}' : ']';
        p = ws(p);
        if (*p != close) for (;;) {
            if (close == '}' && !json_string(&p, (char[2]){0}, 2)) return false;
            p = ws(p);
            if (close == '}' && *p++ != ':') return false;
            if (!skip_value(&p, depth + 1)) return false;
            p = ws(p);
            if (*p == close) break;
            if (*p++ != ',') return false;
        }
        if (*p++ != close) return false;
    } else {
        const char *start = p;
        if (!strncmp(p,"true",4)) p += 4;
        else if (!strncmp(p,"null",4)) p += 4;
        else if (!strncmp(p,"false",5)) p += 5;
        else {
            if (*p == '-') ++p;
            if (*p == '0') ++p;
            else {
                if (*p < '1' || *p > '9') return false;
                while (*p >= '0' && *p <= '9') ++p;
            }
            if (*p == '.') {
                ++p;
                if (*p < '0' || *p > '9') return false;
                while (*p >= '0' && *p <= '9') ++p;
            }
            if (*p == 'e' || *p == 'E') {
                ++p;
                if (*p == '+' || *p == '-') ++p;
                if (*p < '0' || *p > '9') return false;
                while (*p >= '0' && *p <= '9') ++p;
            }
        }
        if (p == start) return false;
    }
    *cursor = p;
    return true;
}

static bool parse_player_palette(const char **cursor, uint8_t palette[16])
{
    const char *p = *cursor;
    if (*p++ != '[') return false;
    for (size_t i = 0; i < 16; ++i) {
        p = ws(p);
        if (*p < '0' || *p > '9') return false;
        unsigned value = (unsigned)(*p++ - '0');
        if (value && *p >= '0' && *p <= '9')
            value = value * 10 + (unsigned)(*p++ - '0');
        if (value > 15) return false;
        palette[i] = (uint8_t)value;
        p = ws(p);
        if (*p++ != (i == 15 ? ']' : ',')) return false;
    }
    *cursor = p;
    return true;
}

static bool parse_manifest(const char *text, cosmo_mod *mod)
{
    const char *p = ws(text);
    bool have_id = false, have_name = false, have_version = false;
    memset(mod, 0, sizeof *mod);
    if (*p++ != '{') return false;
    p = ws(p);
    while (*p && *p != '}') {
        char key[64], value[512];
        if (!json_string(&p, key, sizeof key)) return false;
        p = ws(p);
        if (*p++ != ':') return false;
        p = ws(p);
        if (!strcmp(key,"manifestVersion")) {
            if (*p == '"') {
                if (!json_string(&p,value,sizeof value) || strcmp(value,"1")) return false;
            } else {
                if (strncmp(p,"1",1) || (p[1] >= '0' && p[1] <= '9')) return false;
                ++p;
            }
        } else if (!strcmp(key,"id") || !strcmp(key,"name") || !strcmp(key,"version") ||
            !strcmp(key,"author") || !strcmp(key,"description")) {
            if (!json_string(&p, value, sizeof value)) return false;
            if (!strcmp(key,"id")) { SDL_strlcpy(mod->id,value,sizeof mod->id); have_id = true; }
            if (!strcmp(key,"name")) { SDL_strlcpy(mod->name,value,sizeof mod->name); have_name = true; }
            if (!strcmp(key,"version")) { SDL_strlcpy(mod->version,value,sizeof mod->version); have_version = true; }
            if (!strcmp(key,"author")) SDL_strlcpy(mod->author,value,sizeof mod->author);
            if (!strcmp(key,"description")) SDL_strlcpy(mod->description,value,sizeof mod->description);
        } else if (!strcmp(key,"enabled")) {
            if (!strncmp(p,"true",4)) { mod->enabled = true; p += 4; }
            else if (!strncmp(p,"false",5)) { mod->enabled = false; p += 5; }
            else return false;
        } else if (!strcmp(key,"playerPalette")) {
            if (!parse_player_palette(&p, mod->player_palette)) return false;
            mod->has_player_palette = true;
        } else if (!strcmp(key,"crtShader")) {
            if (!strncmp(p,"true",4)) { mod->crt_shader = true; p += 4; }
            else if (!strncmp(p,"false",5)) { mod->crt_shader = false; p += 5; }
            else return false;
        } else if (!strcmp(key,"crtScanlines")) {
            if (!strncmp(p,"true",4)) { mod->crt_scanlines = true; p += 4; }
            else if (!strncmp(p,"false",5)) { mod->crt_scanlines = false; p += 5; }
            else return false;
        } else {
            unsigned flag = 0;
            for (size_t i = 0; i < SDL_arraysize(features); ++i) {
                if (!strcmp(key, features[i].key)) {
                    flag = features[i].flag;
                    break;
                }
            }
            if (flag) {
                if (!strncmp(p, "true", 4)) { mod->features |= flag; p += 4; }
                else if (!strncmp(p, "false", 5)) { mod->features &= ~flag; p += 5; }
                else return false;
            } else if (!skip_value(&p, 0)) return false;
        }
        p = ws(p);
        if (*p == '}') break;
        if (*p++ != ',') return false;
        p = ws(p);
    }
    if (*p++ != '}') return false;
    return *ws(p) == '\0' && have_id && have_name && have_version &&
           mod->id[0] && mod->name[0] && mod->version[0];
}

static bool valid_id(const char *id)
{
    if (!strcmp(id,".") || !strcmp(id,"..")) return false;
    for (; *id; ++id) if (!( (*id >= 'a' && *id <= 'z') ||
                            (*id >= 'A' && *id <= 'Z') ||
                            (*id >= '0' && *id <= '9') ||
                            *id == '-' || *id == '_' || *id == '.')) return false;
    return true;
}

static int path_compare(const void *a, const void *b)
{
    int order = SDL_strcasecmp(*(char *const *)a, *(char *const *)b);
    return order ? order : SDL_strcmp(*(char *const *)a, *(char *const *)b);
}

static bool any_enabled_mod(void)
{
    for (size_t i = 0; i < mod_count; ++i) if (mods[i].enabled) return true;
    return false;
}

static bool read_mod_setting(const char *id, bool default_value)
{
    size_t size = 0;
    char *text = (char *)SDL_LoadFile(settings_file, &size);
    const char *p, *end;
    bool enabled = false, known = false, has_known_list = false;
    bool rumble = true;
    if (!text) return default_value;
    if (size > JSON_SIZE) { SDL_free(text); return default_value; }
    p = ws(text);
    if (*p++ != '{') goto malformed;
    p = ws(p);
    while (*p && *p != '}') {
        char key[64];
        if (!json_string(&p,key,sizeof key) || *(p=ws(p)) != ':') goto malformed;
        ++p; p = ws(p);
        if (!strcmp(key,"enabledMods") || !strcmp(key,"knownMods")) {
            bool is_enabled = !strcmp(key,"enabledMods");
            if (!is_enabled) has_known_list = true;
            if (*p++ != '[') goto malformed;
            p = ws(p);
            while (*p && *p != ']') {
                char value[128];
                if (!json_string(&p,value,sizeof value)) goto malformed;
                if (id && !strcmp(value,id)) {
                    if (is_enabled) enabled = true;
                    else known = true;
                }
                p = ws(p);
                if (*p == ']') break;
                if (*p++ != ',') goto malformed;
                p = ws(p);
            }
            if (*p++ != ']') goto malformed;
        } else if (!strcmp(key, "gamepadRumble")) {
            if (!strncmp(p, "true", 4)) { rumble = true; p += 4; }
            else if (!strncmp(p, "false", 5)) { rumble = false; p += 5; }
            else goto malformed;
        } else if (!skip_value(&p,0)) goto malformed;
        p = ws(p);
        if (*p == '}') break;
        if (*p++ != ',') goto malformed;
        p = ws(p);
    }
    if (*p++ != '}') goto malformed;
    end = ws(p);
    if (*end) goto malformed;
    SDL_free(text);
    if (!id) return rumble;
    return has_known_list && !known ? default_value : enabled;
malformed:
    SDL_Log("Cosmo mods: malformed settings; using defaults");
    SDL_free(text);
    return default_value;
}

bool mods_save(void)
{
    char json[JSON_SIZE];
    size_t used = 0;
    int n = SDL_snprintf(json,sizeof json,"{\n  \"enabledMods\": [");
    if (n < 0 || (size_t)n >= sizeof json) return false;
    used = (size_t)n;
    for (size_t i = 0; i < mod_count; ++i) if (mods[i].enabled) {
        n = SDL_snprintf(json + used,sizeof json - used,"%s\n    \"%s\"",
                         used > 22 ? "," : "", mods[i].id);
        if (n < 0 || (size_t)n >= sizeof json - used) return false;
        used += (size_t)n;
    }
    n = SDL_snprintf(json + used,sizeof json - used,"\n  ],\n  \"knownMods\": [");
    if (n < 0 || (size_t)n >= sizeof json - used) return false;
    used += (size_t)n;
    for (size_t i = 0; i < mod_count; ++i) {
        n = SDL_snprintf(json + used,sizeof json - used,"%s\n    \"%s\"",
                         i ? "," : "",mods[i].id);
        if (n < 0 || (size_t)n >= sizeof json - used) return false;
        used += (size_t)n;
    }
    n = SDL_snprintf(json + used, sizeof json - used,
                     "\n  ],\n  \"gamepadRumble\": %s\n}\n",
                     gamepad_rumble ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof json - used) return false;
    used += (size_t)n;
    if (!SDL_SaveFile(settings_file,json,used)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"Cosmo mods: cannot save %s: %s",settings_file,SDL_GetError());
        return false;
    }
    return true;
}

bool mods_init(const char *program_dir, const char *settings_root)
{
    char mods_dir[PATH_SIZE], settings_dir[PATH_SIZE];
    int count = 0;
    char **names;
    mod_count = 0;
    gamepad_rumble = true;
    player_colours[0] = player_colours[1] = -1;
    player_colours_file[0] = '\0';
    if (!program_dir || !settings_root ||
        SDL_snprintf(mods_dir,sizeof mods_dir,"%s/Mods",program_dir) >= (int)sizeof mods_dir ||
        SDL_snprintf(settings_dir,sizeof settings_dir,"%s/Settings",settings_root) >= (int)sizeof settings_dir ||
        SDL_snprintf(settings_file,sizeof settings_file,"%s/mods.json",settings_dir) >= (int)sizeof settings_file) return false;
    if (!SDL_CreateDirectory(mods_dir))
        SDL_Log("Cosmo mods: cannot create %s: %s",mods_dir,SDL_GetError());
    if (!SDL_CreateDirectory(settings_dir))
        SDL_Log("Cosmo mods: cannot create %s: %s",settings_dir,SDL_GetError());
    gamepad_rumble = read_mod_setting(NULL, true);
    int path_length = SDL_snprintf(player_colours_file, sizeof player_colours_file,
                                   "%s/player-colours", settings_dir);
    if (path_length > 0 && (size_t)path_length < sizeof player_colours_file) {
        size_t size = 0;
        char *text = SDL_LoadFile(player_colours_file, &size);
        if (text && size == 2 && valid_player_colour(text[0] - '0') &&
            valid_player_colour(text[1] - '0')) {
            player_colours[0] = text[0] - '0';
            player_colours[1] = text[1] - '0';
        }
        SDL_free(text);
    } else {
        player_colours_file[0] = '\0';
    }
    SDL_ClearError();
    names = SDL_GlobDirectory(mods_dir,"*",SDL_GLOB_CASEINSENSITIVE,&count);
    if (!names && SDL_GetError()[0]) SDL_Log("Cosmo mods: cannot scan %s: %s",mods_dir,SDL_GetError());
    if (names) SDL_qsort(names,(size_t)count,sizeof *names,path_compare);
    for (int i = 0; names && i < count && mod_count < COSMO_MOD_LIMIT; ++i) {
        char dir[PATH_SIZE], manifest[PATH_SIZE];
        SDL_PathInfo info;
        size_t size = 0;
        char *contents;
        cosmo_mod candidate;
        if (SDL_snprintf(dir,sizeof dir,"%s/%s",mods_dir,names[i]) >= (int)sizeof dir ||
            !SDL_GetPathInfo(dir,&info) || info.type != SDL_PATHTYPE_DIRECTORY ||
            SDL_snprintf(manifest,sizeof manifest,"%s/mod.json",dir) >= (int)sizeof manifest) continue;
        contents = (char *)SDL_LoadFile(manifest,&size);
        if (!contents || size > JSON_SIZE || !parse_manifest(contents,&candidate) || !valid_id(candidate.id)) {
            SDL_Log("Cosmo mods: skipping invalid mod manifest %s",manifest);
            SDL_free(contents);
            continue;
        }
        SDL_free(contents);
        bool duplicate = false;
        for (size_t j = 0; j < mod_count; ++j) if (!SDL_strcasecmp(mods[j].id,candidate.id)) duplicate = true;
        if (duplicate) { SDL_Log("Cosmo mods: duplicate id '%s' skipped",candidate.id); continue; }
        SDL_strlcpy(candidate.directory,dir,sizeof candidate.directory);
#ifdef COSMO_DEFAULT_WIDESCREEN
        if (candidate.features & MOD_WIDESCREEN) candidate.enabled = true;
#endif
        candidate.enabled = read_mod_setting(candidate.id,candidate.enabled);
        mods[mod_count++] = candidate;
    }
    SDL_free(names);
    SDL_PathInfo config_info;
    if (!SDL_GetPathInfo(settings_file,&config_info) && !mods_save())
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"Cosmo mods: could not create default settings file %s",settings_file);
    return true;
}

size_t mods_count(void) { return mod_count; }
const cosmo_mod *mods_get(size_t index) { return index < mod_count ? &mods[index] : NULL; }

bool mods_gamepad_rumble_enabled(void) { return gamepad_rumble; }

bool mods_set_gamepad_rumble(bool enabled)
{
    bool old = gamepad_rumble;
    gamepad_rumble = enabled;
    if (mods_save()) return true;
    gamepad_rumble = old;
    return false;
}

bool mods_crt_shader_enabled(void)
{
    for (size_t i = 0; i < mod_count; ++i)
        if (mods[i].enabled && mods[i].crt_shader) return true;
    return false;
}

bool mods_crt_scanlines_enabled(void)
{
    for (size_t i = 0; i < mod_count; ++i)
        if (mods[i].enabled && mods[i].crt_scanlines) return true;
    return false;
}

bool mods_feature_enabled(enum cosmo_mod_feature feature)
{
    for (size_t i = 0; i < mod_count; ++i)
        if (mods[i].enabled && (mods[i].features & feature)) return true;
    return false;
}

static const uint8_t *active_skin_palette(void)
{
    for (size_t i = 0; i < mod_count; ++i)
        if (mods[i].enabled && mods[i].has_player_palette) return mods[i].player_palette;
    return NULL;
}

static void recolor_player(uint8_t *tiles, size_t size, const uint8_t *palette)
{
    if (!palette || !tiles || size % 40 != 0) return;

    /* Each 8x8 tile has eight rows of one mask byte and four colour planes. */
    for (size_t row = 0; row < size; row += 5) {
        for (unsigned pixel = 0; pixel < 8; ++pixel) {
            uint8_t bit = (uint8_t)(1u << pixel);
            if (tiles[row] & bit) continue;
            unsigned index = 0;
            for (unsigned plane = 0; plane < 4; ++plane)
                if (tiles[row + 1 + plane] & bit) index |= 1u << plane;

            unsigned replacement = palette[index];
            if (index == replacement) continue;
            for (unsigned plane = 0; plane < 4; ++plane) {
                uint8_t *channel = &tiles[row + 1 + plane];
                if (replacement & (1u << plane)) *channel |= bit;
                else *channel &= (uint8_t)~bit;
            }
        }
    }
}

const char *mods_player_colour_name(int colour)
{
    return valid_player_colour(colour) ? player_colour_options[colour].name : "";
}

void mods_get_player_colours(int colours[2])
{
    const uint8_t *skin = active_skin_palette();
    for (size_t part = 0; part < 2; ++part) {
        colours[part] = player_colours[part] >= 0 ? player_colours[part] : (int)part;
        if (player_colours[part] >= 0 || !skin) continue;
        for (int colour = 0; colour < COSMO_PLAYER_COLOUR_COUNT; ++colour) {
            if (skin[player_shadows[part]] == player_colour_options[colour].shadow &&
                skin[player_lights[part]] == player_colour_options[colour].light) {
                colours[part] = colour;
                break;
            }
        }
    }
}

void mods_preview_player_colours(uint8_t *tiles, size_t size, const int colours[2])
{
    if (!colours) {
        recolor_player(tiles, size, active_skin_palette());
        return;
    }
    if (!valid_player_colour(colours[0]) || !valid_player_colour(colours[1])) return;
    uint8_t palette[16];
    for (unsigned i = 0; i < 16; ++i) palette[i] = (uint8_t)i;
    for (size_t part = 0; part < 2; ++part) {
        palette[player_shadows[part]] = player_colour_options[colours[part]].shadow;
        palette[player_lights[part]] = player_colour_options[colours[part]].light;
    }
    recolor_player(tiles, size, palette);
}

void mods_recolor_player(uint8_t *tiles, size_t size)
{
    mods_preview_player_colours(tiles, size, player_colours[0] >= 0 ? player_colours : NULL);
}

bool mods_save_player_colours(const int colours[2])
{
    char temporary[PATH_SIZE + 32];
    char text[2] = {'-', '-'};
    if (!player_colours_file[0]) return false;
    if (colours) {
        if (!valid_player_colour(colours[0]) || !valid_player_colour(colours[1])) return false;
        for (size_t i = 0; i < 2; ++i) text[i] = (char)('0' + colours[i]);
    }
    SDL_snprintf(temporary, sizeof temporary, "%s.%llu.tmp", player_colours_file,
                 (unsigned long long)SDL_GetPerformanceCounter());
    if (!SDL_SaveFile(temporary, text, sizeof text) ||
        !SDL_RenamePath(temporary, player_colours_file)) {
        SDL_RemovePath(temporary);
        return false;
    }
    for (size_t i = 0; i < 2; ++i) player_colours[i] = colours ? colours[i] : -1;
    return true;
}

bool mods_music_override(const char *asset_name, char *path, size_t path_size)
{
    char filename[128];
    SDL_PathInfo info;
    const char *extension;

    if (!asset_name || !path || !path_size || strchr(asset_name,'/') ||
        strchr(asset_name,'\\')) return false;
    extension = strrchr(asset_name,'.');
    if (!extension || SDL_strcasecmp(extension,".mni")) return false;
    size_t stem = (size_t)(extension - asset_name);
    if (!stem || stem + 5 >= sizeof filename) return false;
    SDL_memcpy(filename,asset_name,stem);
    SDL_memcpy(filename + stem,".mp3",5);

    for (size_t i = 0; i < mod_count; ++i) if (mods[i].enabled) {
        int n = SDL_snprintf(path,path_size,"%s/music/%s",mods[i].directory,filename);
        if (n < 0 || (size_t)n >= path_size) continue;
        if (SDL_GetPathInfo(path,&info) && info.type == SDL_PATHTYPE_FILE && info.size > 0)
            return true;
    }
    path[0] = '\0';
    return false;
}

bool mods_sound_override(unsigned sound_number, char *path, size_t path_size)
{
    SDL_PathInfo info;
    if (!path || !path_size || sound_number < 1 ||
        sound_number > SDL_arraysize(sound_asset_names)) return false;
    for (size_t i = 0; i < mod_count; ++i) if (mods[i].enabled) {
        const char *extensions[] = {"wav", "mp3"};
        for (size_t ext = 0; ext < SDL_arraysize(extensions); ++ext) {
            int n = SDL_snprintf(path,path_size,"%s/sounds/%s.%s",mods[i].directory,
                                 sound_asset_names[sound_number - 1],extensions[ext]);
            if (n < 0 || (size_t)n >= path_size) continue;
            if (SDL_GetPathInfo(path,&info) && info.type == SDL_PATHTYPE_FILE && info.size > 0)
                return true;
        }
    }
    path[0] = '\0';
    return false;
}

bool mods_set_enabled(size_t index, bool enabled)
{
    if (index >= mod_count) return false;
    bool old = mods[index].enabled;
    mods[index].enabled = enabled;
    if (!mods_save()) { mods[index].enabled = old; return false; }
    return true;
}

static bool set_filter_pair(size_t crt, size_t scanlines, int value)
{
    bool old_crt = mods[crt].enabled;
    bool old_scanlines = mods[scanlines].enabled;
    mods[crt].enabled = (value & 1) != 0;
    mods[scanlines].enabled = (value & 2) != 0;
    if (mods_save()) return true;
    mods[crt].enabled = old_crt;
    mods[scanlines].enabled = old_scanlines;
    return false;
}

static int filter_settings(size_t *crt, size_t *scanlines)
{
    int value = 0;
    *crt = SIZE_MAX;
    *scanlines = SIZE_MAX;
    for (size_t i = 0; i < mod_count; ++i) {
        if (!SDL_strcasecmp(mods[i].id, COSMO_CRT_MOD_ID)) {
            *crt = i;
            if (mods[i].enabled) value |= 1;
        } else if (!SDL_strcasecmp(mods[i].id, COSMO_SCANLINES_MOD_ID)) {
            *scanlines = i;
            if (mods[i].enabled) value |= 2;
        }
    }
    return value;
}

const char *mods_display_filter_name(void)
{
    static const char *const names[] = {"None", "CRT", "Scanlines", "Both"};
    size_t crt, scanlines;
    return names[filter_settings(&crt, &scanlines)];
}

bool mods_step_display_filter(int direction)
{
    size_t crt, scanlines;
    int value = filter_settings(&crt, &scanlines);
    int available = (crt != SIZE_MAX ? 1 : 0) | (scanlines != SIZE_MAX ? 2 : 0);
    if (!available) return false;

    do {
        value = (value + (direction < 0 ? 3 : 1)) % 4;
    } while (value & ~available);

    if (available == 3) return set_filter_pair(crt, scanlines, value);
    return mods_set_enabled(available == 1 ? crt : scanlines, value != 0);
}

static bool override_path(const cosmo_mod *mod, int episode,
                          const char *asset_name, size_t expected,
                          char path[PATH_SIZE])
{
    SDL_PathInfo info;
    SDL_snprintf(path,PATH_SIZE,"%s/graphics/episode%d/%s",mod->directory,episode,asset_name);
    if (SDL_GetPathInfo(path,&info) && info.type == SDL_PATHTYPE_FILE &&
        info.size == expected && info.size <= OVERRIDE_MAX_SIZE) return true;
    SDL_snprintf(path,PATH_SIZE,"%s/graphics/%s",mod->directory,asset_name);
    return SDL_GetPathInfo(path,&info) && info.type == SDL_PATHTYPE_FILE &&
           info.size == expected && info.size <= OVERRIDE_MAX_SIZE;
}

size_t mods_read_override(const char *datadir, int episode, const char *asset_name,
                          void *dest, size_t max)
{
    if (!asset_name || strchr(asset_name,'/') || strchr(asset_name,'\\')) return 0;
    if (!any_enabled_mod()) return 0;
    size_t expected = group_entry_size(datadir,episode,asset_name);
    if (!expected) return 0;
    for (size_t i = 0; i < mod_count; ++i) if (mods[i].enabled) {
        char path[PATH_SIZE];
        size_t size = 0;
        void *data;
        if (!override_path(&mods[i],episode,asset_name,expected,path)) continue;
        data = SDL_LoadFile(path,&size);
        if (data && size == expected && size <= OVERRIDE_MAX_SIZE) {
            size_t copied = size < max ? size : max;
            if (copied) SDL_memcpy(dest,data,copied);
            SDL_free(data);
            if (size > max) SDL_Log("Cosmo mods: replacement %s exceeds expected size",path);
            return copied;
        }
        SDL_free(data);
    }
    return 0;
}

FILE *mods_open_override(const char *datadir, int episode, const char *asset_name,
                         uint32_t *length)
{
    if (!asset_name || strchr(asset_name,'/') || strchr(asset_name,'\\')) return NULL;
    if (!any_enabled_mod()) return NULL;
    size_t expected = group_entry_size(datadir,episode,asset_name);
    if (!expected) return NULL;
    for (size_t i = 0; i < mod_count; ++i) if (mods[i].enabled) {
        char path[PATH_SIZE];
        size_t size = 0;
        void *data;
        FILE *fp;
        if (!override_path(&mods[i],episode,asset_name,expected,path)) continue;
        data = SDL_LoadFile(path,&size);
        if (!data || size != expected || size > OVERRIDE_MAX_SIZE || size > UINT32_MAX) {
            SDL_free(data);
            SDL_Log("Cosmo mods: invalid or unreadable replacement %s",path);
            continue;
        }
        fp = tmpfile();
        if (!fp || fwrite(data,1,size,fp) != size) {
            if (fp) fclose(fp);
            SDL_free(data);
            SDL_Log("Cosmo mods: could not stage replacement %s",path);
            continue;
        }
        SDL_free(data);
        rewind(fp);
        if (length) *length = (uint32_t)size;
        return fp;
    }
    return NULL;
}
