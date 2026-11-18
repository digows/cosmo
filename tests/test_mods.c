#include "cosmo/mods.h"
#include "cosmo/group.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#condition); \
        failures++; \
    } \
} while (0)

static bool write_text(const char *path, const char *text)
{
    return SDL_SaveFile(path,text,SDL_strlen(text));
}

static void remove_path(const char *root,const char *relative)
{
    char path[1024];
    SDL_snprintf(path,sizeof path,"%s/%s",root,relative);
    (void)SDL_RemovePath(path);
}

static void cleanup_fixture(const char *root)
{
    static const char *const folders[] = {
        "alpha","beta","broken","duplicate","missing","future","crt-shader"
    };
    char relative[256];
    remove_path(root,"COSMO1.STN");
    remove_path(root,"Mods/alpha/graphics/episode1/PRETITLE.MNI");
    remove_path(root,"Mods/alpha/mod.json");
    for (size_t i=1;i<SDL_arraysize(folders);++i) {
        SDL_snprintf(relative,sizeof relative,"Mods/%s/mod.json",folders[i]);
        remove_path(root,relative);
    }
    remove_path(root,"Settings/mods.json");
    remove_path(root,"Mods/alpha/graphics/episode1");
    remove_path(root,"Mods/alpha/graphics");
    for (size_t i=0;i<SDL_arraysize(folders);++i) {
        SDL_snprintf(relative,sizeof relative,"Mods/%s",folders[i]);
        remove_path(root,relative);
    }
    remove_path(root,"Mods");
    remove_path(root,"Settings");
    remove_path(root,"user");
    (void)SDL_RemovePath(root);
}

static void create_mod(const char *root,const char *folder,const char *manifest)
{
    char path[1024];
    SDL_snprintf(path,sizeof path,"%s/Mods/%s",root,folder);
    CHECK(SDL_CreateDirectory(path));
    SDL_snprintf(path,sizeof path,"%s/Mods/%s/mod.json",root,folder);
    CHECK(write_text(path,manifest));
}

int main(void)
{
    char root[1024], settings[1024], path[1024];
    char payload[32] = {0};
    char *cwd = SDL_GetCurrentDirectory();
    CHECK(SDL_Init(0));
    SDL_snprintf(root,sizeof root,"%s/cosmo-mod-test-%llu-%llu",cwd ? cwd : ".",
                 (unsigned long long)SDL_GetTicksNS(),
                 (unsigned long long)SDL_GetTicksNS());
    SDL_free(cwd);
    SDL_snprintf(settings,sizeof settings,"%s/user",root);
    CHECK(SDL_CreateDirectory(root));
    CHECK(SDL_CreateDirectory(settings));

    /* Missing and empty Mods directories are harmless. */
    CHECK(mods_init(root,settings));
    CHECK(mods_count()==0);

    create_mod(root,"alpha","{\"id\":\"alpha\",\"name\":\"Alpha\",\"version\":\"1.0\",\"author\":\"A\",\"description\":\"First\",\"enabled\":true}");
    create_mod(root,"beta","{\"id\":\"beta\",\"name\":\"Beta\",\"version\":\"2.0\"}");
    create_mod(root,"broken","{ definitely not json");
    create_mod(root,"duplicate","{\"id\":\"alpha\",\"name\":\"Duplicate\",\"version\":\"9\"}");
    create_mod(root,"missing","{\"id\":\"missing\",\"name\":\"Missing version\"}");
    create_mod(root,"future","{\"id\":\"future\",\"name\":\"Future\",\"version\":\"1\",\"manifestVersion\":2}");
    create_mod(root,"crt-shader","{\"id\":\"crt-shader\",\"name\":\"CRT Shader\",\"version\":\"1.0\",\"enabled\":true,\"crtShader\":true}");

    /* A tiny original STN entry lets the test verify same-size overrides and
     * the normal game-data fallback path. */
    {
        unsigned char group[966] = {0};
        FILE *fp;
        memcpy(group,"PRETITLE.MNI",12);
        group[12]=0xC0; group[13]=0x03; /* offset 960 */
        group[16]=6;
        memcpy(group+960,"ORIG!!",6);
        SDL_snprintf(path,sizeof path,"%s/COSMO1.STN",root);
        fp=fopen(path,"wb");
        CHECK(fp != NULL);
        if (fp) { CHECK(fwrite(group,1,sizeof group,fp)==sizeof group); fclose(fp); }
    }

    /* Stable folder order makes alpha the first provider and duplicate wins
     * are rejected. Preferences start disabled until explicitly enabled. */
    CHECK(mods_init(root,settings));
    CHECK(mods_count()==3);
    CHECK(mods_get(0)->enabled && !mods_get(1)->enabled);
    CHECK(mods_get(2)->crt_shader && mods_crt_shader_enabled());
    /* The first config file is created automatically. */
    SDL_snprintf(path,sizeof path,"%s/Settings/mods.json",settings);
    CHECK(SDL_GetPathInfo(path,NULL));
    CHECK(mods_read_override(root,1,"PRETITLE.MNI",payload,sizeof payload)==0);

    SDL_snprintf(path,sizeof path,"%s/Mods/alpha/graphics/episode1",root);
    CHECK(SDL_CreateDirectory(path));
    SDL_snprintf(path,sizeof path,"%s/Mods/alpha/graphics/episode1/PRETITLE.MNI",root);
    CHECK(SDL_SaveFile(path,"MODDED",6));
    CHECK(mods_read_override(root,1,"PRETITLE.MNI",payload,sizeof payload)==6);
    CHECK(!memcmp(payload,"MODDED",6));
    {
        uint32_t length = 0;
        FILE *fp = mods_open_override(root,1,"PRETITLE.MNI",&length);
        CHECK(fp != NULL && length == 6);
        if (fp) {
            memset(payload,0,sizeof payload);
            CHECK(fread(payload,1,6,fp)==6);
            CHECK(!memcmp(payload,"MODDED",6));
            fclose(fp);
        }
    }
    CHECK(mods_read_override(root,1,"MISSING.MNI",payload,sizeof payload)==0);
    CHECK(mods_read_override(root,1,"../escape",payload,sizeof payload)==0);

    CHECK(mods_get(0)->enabled);
    CHECK(mods_init(root,settings));
    CHECK(mods_get(0)->enabled);
    CHECK(mods_set_enabled(0,false));
    CHECK(mods_init(root,settings));
    CHECK(!mods_get(0)->enabled);
    CHECK(mods_crt_shader_enabled());
    CHECK(mods_set_enabled(2,false));
    CHECK(!mods_crt_shader_enabled());
    CHECK(mods_set_enabled(2,true));

    /* With no enabled replacement, the normal STN reader still supplies the
     * original asset. */
    CHECK(group_read(root,1,"PRETITLE.MNI",payload,sizeof payload)==6);
    CHECK(!memcmp(payload,"ORIG!!",6));

    SDL_snprintf(path,sizeof path,"%s/Settings/mods.json",settings);
    CHECK(write_text(path,"not json"));
    CHECK(mods_init(root,settings));
    CHECK(mods_get(0)->enabled);
    CHECK(mods_crt_shader_enabled());

    if (failures) return 1;
    cleanup_fixture(root);
    SDL_Quit();
    puts("mod tests passed");
    return 0;
}
