/*
 * q2red_bot_brain.h - the contract between the q2red server and a bot brain.
 *
 * DIVERGENCE(A54). Not retail. Retail's engine carries its own bot brain (the
 * Kex bot manager behind `addbot`); q2red does not reconstruct it. Instead the
 * server can load a bot brain from a shared library and drive it:
 *
 *   - the SERVER owns the bots as clients. It admits each one into a real
 *     client slot with the bot endpoint and no netchan, exactly as retail's
 *     add-bot path does, and calls the game module's ClientChooseSlot /
 *     ClientConnect / ClientBegin with is_bot = true and social id
 *     "me_a_bot". So any rerelease game module (id's, MuffMode, MMREX) sees
 *     the same bots it would see on retail, with no change to the module.
 *   - the BRAIN only decides. Once per server frame, for every bot, it fills
 *     in a usercmd; the server hands that to the game's ClientThink. It reads
 *     the world through the host table below and never touches game memory.
 *   - movement is the game's: `pmove` runs the game module's own Pmove
 *     export, so a brain that predicts movement predicts the real thing.
 *
 * The game module ABI (game.h) is not touched, and nothing here reaches the
 * wire. With no brain loaded (the default) the server behaves as before.
 *
 * Plain C99 on purpose: brains are C, and the brain side keeps a pinned copy
 * of this file. Bump Q2RED_BOT_BRAIN_API_VERSION on any incompatible change;
 * append-only additions to the two tables keep the version and grow `size`.
 *
 * Userinfo strings use this server's (retail's) form, key\value pairs with no
 * leading backslash; add_bot also accepts a legacy "\name\x" string.
 *
 * Loading: `sv_bot_brain <name>` (empty by default). The server looks for
 * <name>_x64.so (Linux) / <name>_x64.dll (Windows) in the game directory,
 * then in the base directory, resolves Q2RED_BOT_BRAIN_ENTRY and calls it.
 */
#ifndef Q2RED_BOT_BRAIN_H
#define Q2RED_BOT_BRAIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Q2RED_BOT_BRAIN_API_VERSION 1
#define Q2RED_BOT_BRAIN_ENTRY "Q2RED_GetBotBrainAPI"

/* Sizes the host guarantees. */
#define Q2RED_BOT_MAX_ITEMS 256 /* inventory slots, rerelease MAX_ITEMS */
#define Q2RED_BOT_MAX_STATS 64  /* player_state_t stats, rerelease MAX_STATS */

/* Rerelease button bits (game.h button_t). */
#define Q2RED_BOT_BUTTON_ATTACK  0x01
#define Q2RED_BOT_BUTTON_USE     0x02
#define Q2RED_BOT_BUTTON_HOLSTER 0x04
#define Q2RED_BOT_BUTTON_JUMP    0x08
#define Q2RED_BOT_BUTTON_CROUCH  0x10
#define Q2RED_BOT_BUTTON_ANY     0x80

/* Same layout as the rerelease usercmd_t. angles are absolute view angles in
 * degrees MINUS the player's delta_angles, as a real client sends them;
 * moves are in units per second (+-400 is a full key press). */
typedef struct q2red_bot_usercmd_s {
    uint8_t  msec;    /* filled by the server: the tick length */
    uint8_t  buttons; /* Q2RED_BOT_BUTTON_* */
    uint16_t padding;
    float    angles[3];
    float    forwardmove;
    float    sidemove;
    uint32_t server_frame; /* filled by the server */
} q2red_bot_usercmd_t;

/* A console variable mirror. The host owns it and refreshes `string`,
 * `value`, `integer` and `modified_count` before every call into the brain,
 * so a brain can read cvars as fields without calling back. */
typedef struct q2red_bot_cvar_s {
    const char *name;
    const char *string;
    float       value;
    int32_t     integer;
    int32_t     modified_count;
} q2red_bot_cvar_t;

typedef struct q2red_bot_trace_s {
    int32_t allsolid;
    int32_t startsolid;
    float   fraction;
    float   endpos[3];
    float   plane_normal[3];
    float   plane_dist;
    int32_t contents;      /* contents of what was hit */
    int32_t surface_flags; /* SURF_* of the surface hit, 0 if none */
    int32_t ent;           /* entity number hit; 0 = world, -1 = nothing */
} q2red_bot_trace_t;

/* What the server can see of one entity: the networked state, the shared
 * edict fields, and the read-only bot block (sv_entity_t) the game module
 * publishes for an engine-side bot every frame. Strings are borrowed: valid
 * until the next call into the brain. */
typedef struct q2red_bot_entity_s {
    int32_t number;
    int32_t inuse;
    int32_t is_client;   /* number 1..maxclients with a client */
    int32_t is_bot;      /* a client the server admitted as a bot */
    int32_t linked;

    /* entity_state_t */
    float   origin[3];
    float   old_origin[3];
    float   angles[3];
    int32_t modelindex;
    int32_t effects;
    int32_t renderfx;
    int32_t sound;
    int32_t event;

    /* edict_shared_t */
    uint32_t svflags;
    int32_t  solid;
    uint32_t clipmask;
    int32_t  owner;     /* entity number, -1 if none */
    float    mins[3];
    float    maxs[3];
    float    absmin[3];
    float    absmax[3];

    /* sv_entity_t, as the game module last published it (it fills the block
     * during its own frame, so it lags by one frame). */
    int32_t     sv_init;
    uint64_t    sv_flags;          /* SVFL_* */
    int32_t     buttons;
    uint32_t    spawnflags;
    int32_t     item_id;
    int32_t     armor_type;
    int32_t     armor_value;
    int32_t     health;            /* -1 for a dead player or monster */
    int32_t     max_health;
    int32_t     starting_health;
    int32_t     weapon;            /* item id of the held weapon */
    int32_t     team;
    int32_t     respawntime;       /* items: ms until respawn; players: respawn time */
    int32_t     viewheight;
    int32_t     last_attackertime; /* ms */
    int32_t     waterlevel;        /* 0..3 */
    float       viewangles[3];
    float       viewforward[3];
    float       velocity[3];
    float       start_origin[3];
    float       end_origin[3];
    int32_t     enemy;             /* entity number, -1 if none */
    int32_t     ground_entity;     /* entity number, -1 if none */
    const char *classname;         /* "" if unknown */
    const char *targetname;        /* "" if unknown */
    const char *netname;           /* "" if unknown */
    const int32_t *inventory;      /* Q2RED_BOT_MAX_ITEMS entries; NULL if not a client */

    /* player_state_t, clients only */
    int32_t pm_type;
    float   pm_origin[3];
    float   pm_velocity[3];
    int32_t pm_flags;
    int32_t pm_time;               /* ms */
    int32_t gravity;
    float   delta_angles[3];
    int32_t pm_viewheight;
    float   ps_viewangles[3];
    const int16_t *stats;          /* Q2RED_BOT_MAX_STATS entries; NULL if not a client */
} q2red_bot_entity_t;

/* One player-movement step, run by the game module's own Pmove. The host
 * fills the bounding box, traces and contents itself. */
typedef struct q2red_bot_pmove_s {
    /* state, in and out (pmove_state_t) */
    int32_t pm_type;
    float   origin[3];
    float   velocity[3];
    int32_t pm_flags;
    int32_t pm_time;        /* ms */
    int32_t gravity;
    float   delta_angles[3];
    int32_t viewheight;

    /* in */
    q2red_bot_usercmd_t cmd; /* msec must be set */
    int32_t passent;         /* the player entity, ignored by the traces */
    int32_t snapinitial;

    /* out */
    float   viewangles[3];
    float   mins[3];
    float   maxs[3];
    int32_t groundentity;    /* -1 if airborne */
    int32_t watertype;
    int32_t waterlevel;
    int32_t jump_sound;
    float   impact_delta;
} q2red_bot_pmove_t;

/* Print levels for bprint / cprint (game.h print_type_t). */
#define Q2RED_BOT_PRINT_LOW    0
#define Q2RED_BOT_PRINT_MEDIUM 1
#define Q2RED_BOT_PRINT_HIGH   2
#define Q2RED_BOT_PRINT_CHAT   3

/* add_bot failures. */
#define Q2RED_BOT_ADD_UNAVAILABLE    (-1)
#define Q2RED_BOT_ADD_SERVER_FULL    (-2)
#define Q2RED_BOT_ADD_NO_EDICT       (-3)
#define Q2RED_BOT_ADD_GAME_REJECTED  (-4)

typedef void (*q2red_bot_command_fn)(void);

/* Services the server provides. Every call is valid from inside any brain
 * callback; none may be kept for use from another thread. */
typedef struct q2red_bot_host_s {
    uint32_t api_version;
    uint32_t size;

    /* console */
    void (*print)(const char *text);            /* Com_Printf */
    void (*dprint)(const char *text);           /* Com_DPrintf */
    void (*bprint)(int level, const char *text);
    void (*cprint)(int entnum, int level, const char *text);
    q2red_bot_cvar_t *(*cvar)(const char *name, const char *default_value,
                              int flags);       /* flags: CVAR_* */
    void (*cvar_set)(const char *name, const char *value);
    void (*cvar_forceset)(const char *name, const char *value);
    void (*add_command)(const char *name, q2red_bot_command_fn fn);
    void (*remove_command)(const char *name);
    int  (*argc)(void);
    const char *(*argv)(int index);
    const char *(*args)(void);
    void (*add_command_string)(const char *text); /* console buffer */

    /* time and place */
    uint32_t (*tick_rate)(void);       /* server frames per second */
    uint32_t (*frame_time_ms)(void);
    uint64_t (*level_time_ms)(void);   /* ms since the map started */
    int32_t  (*frame_number)(void);
    const char *(*map_name)(void);
    const char *(*game_dir)(void);     /* writable game directory path */
    const char *(*base_dir)(void);
    const char *(*entity_string)(void); /* the map's entity lump */
    const char *(*configstring)(int index);

    /* world */
    int32_t (*max_clients)(void);
    int32_t (*num_edicts)(void);
    int32_t (*get_entity)(int32_t number, q2red_bot_entity_t *out); /* 0 = no such */
    void    (*trace)(const float start[3], const float mins[3],
                     const float maxs[3], const float end[3], int32_t passent,
                     int32_t contentmask, q2red_bot_trace_t *out);
    int32_t (*pointcontents)(const float point[3]);
    int32_t (*in_pvs)(const float p1[3], const float p2[3]);
    int32_t (*in_phs)(const float p1[3], const float p2[3]);
    void    (*pmove)(q2red_bot_pmove_t *pm);

    /* bots */
    int32_t (*add_bot)(const char *userinfo); /* entity number, or Q2RED_BOT_ADD_* */
    void    (*kick_bot)(int32_t entnum);
    int32_t (*is_bot)(int32_t entnum);
    void    (*client_command)(int32_t entnum, const char *text); /* as if typed */
    void    (*set_weapon)(int32_t entnum, int32_t item_id, int32_t instant);
    void    (*use_item)(int32_t entnum, int32_t item_id);
    int32_t (*item_id)(const char *classname); /* game's Bot_GetItemID; 0 if unknown */

    /* debug drawing (clients with the debug draw cvar see it); may be no-ops */
    void (*draw_line)(const float start[3], const float end[3],
                      uint32_t rgba, float seconds);
} q2red_bot_host_t;

/* What a brain provides. Any callback may be NULL. */
typedef struct q2red_bot_brain_s {
    uint32_t    api_version;
    uint32_t    size;
    const char *name;
    const char *version;

    int  (*init)(void);      /* after load; 0 = refuse, the host unloads */
    void (*shutdown)(void);  /* before unload */

    void (*map_begin)(const char *mapname); /* entities spawned */
    void (*map_end)(void);                  /* before the next map or shutdown */

    /* every client, human or bot, entering and leaving the game */
    void (*client_begin)(int32_t entnum, int32_t is_bot);
    void (*client_disconnect)(int32_t entnum, int32_t is_bot);

    /* once per server frame before the game runs: think for each bot. */
    void (*frame_begin)(void);
    void (*think)(int32_t entnum, q2red_bot_usercmd_t *cmd);
    void (*frame_end)(void); /* after the game frame */

    /* the operator's addbot [name] / kickbot. NULL = the host's defaults. */
    void (*cmd_addbot)(const char *name);
    void (*cmd_kickbot)(void);

    /* a sound the game started (gi.sound / gi.positioned_sound). entnum is
     * the emitting entity or 0; origin is where it was heard from. */
    void (*sound)(int32_t entnum, const float origin[3], int32_t channel,
                  int32_t soundindex, float volume, float attenuation);

    /* the game asked the engine bot to go somewhere (gi.Bot_MoveToPoint /
     * gi.Bot_FollowActor). Returns a GoalReturnCode: 0 Error, 1 Started,
     * 2 InProgress, 3 Finished. NULL = always Error, as with no brain. */
    int32_t (*move_to_point)(int32_t bot, const float point[3], float tolerance);
    int32_t (*follow_actor)(int32_t bot, int32_t actor);
} q2red_bot_brain_t;

typedef const q2red_bot_brain_t *(*q2red_bot_brain_entry_fn)(
    uint32_t api_version, const q2red_bot_host_t *host);

#ifdef __cplusplus
}
#endif

#endif /* Q2RED_BOT_BRAIN_H */
