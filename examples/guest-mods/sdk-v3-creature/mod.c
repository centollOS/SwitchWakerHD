/* SPDX-License-Identifier: MPL-2.0
 * Original bounded wandering creature and PCM triangle-tone demonstration. */
#include "wwhd_guest.h"
#include "wwhd/bindings.h"

typedef struct { u32 handle,hits,last_sequence; f32 pause; u32 walking; } Creature;
static wwhd_actor_state_v1 state; /* Service buffers must not be game-stack locals. */
static const wwhd_actor_vec3 route[2]={{800,1040,-201500},{940,1040,-201500}};
static u32 registered;
static s32 stream=-1;
static unsigned long long audio_epoch;
static u32 tone_active,tone_phase;
static short samples[1600];

static void audio_reset(void) {
    if(stream>0)wwhd_audio_close((u32)stream);
    stream=-1;tone_phase=0;
}
static void audio_step(void) {
    unsigned long long epoch=wwhd_audio_epoch();
    if(epoch!=audio_epoch){audio_epoch=epoch;stream=-1;tone_phase=0;}
    if(!tone_active)return;
    if(stream<0)stream=wwhd_audio_open(48000,1);
    if(stream<0)return;
    s32 available=wwhd_audio_available((u32)stream);
    if(available==WWHD_AUDIO_INVALID){stream=-1;return;}
    if(available<=0)return;
    u32 count=(u32)available;if(count>1600)count=1600;
    for(u32 i=0;i<count;++i){u32 p=(tone_phase+i)%240; s32 triangle=p<120?(s32)p:(s32)(240-p);samples[i]=(short)((triangle-60)*70);}
    s32 accepted=wwhd_audio_submit((u32)stream,samples,count,1);
    if(accepted>0)tone_phase=(tone_phase+(u32)accepted)%240;
    else if(accepted==WWHD_AUDIO_INVALID)stream=-1;
}
static u32 music(const char* cue,u32 phase,u32 fade,u32 reason) {
    (void)reason;if(fade)return 0;
    /* One authored cue. Restore returns to this example's prior silence; it
     * never changes the game's BGM. This cue has abrupt start/stop, fade=0. */
    if(cue[0]!='c'||cue[1]!='o'||cue[2]!='r'||cue[3]!='a'||cue[4]!='l'||cue[5])return 0;
    if(phase==WWHD_SCENE_MUSIC_START){tone_active=1;wwhd_log("coral music start");audio_step();return 1;}
    if(phase==WWHD_SCENE_MUSIC_STOP||phase==WWHD_SCENE_MUSIC_RESTORE){tone_active=0;audio_reset();wwhd_log("coral music stop/restore");return 1;}
    return 0;
}
static void create(wwhd_actor_handle handle,void* data,u32 reason) {
    Creature* c=(Creature*)data;c->handle=handle;
    if(reason==WWHD_ACTOR_NEW){c->hits=0;c->last_sequence=0;c->pause=0;c->walking=0;}
    wwhd_log(reason==WWHD_ACTOR_RESTORED?"coral creature restored":"coral creature new");
}
static void execute(wwhd_actor_handle handle,void* data,f32 dt) {
    Creature* c=(Creature*)data;audio_step();
    state.size=sizeof state;state.version=1;
    if(!wwhd_actor_get(handle,&state)||(state.flags&WWHD_ACTOR_SCENE_OWNED))return;
    if(!(dt>0&&dt<=.25f))return;
    if(c->pause>0){c->pause-=dt;if(c->pause>0)return;c->pause=0;c->walking=0;}
    if(!c->walking&&wwhd_actor_walk(handle,route,2,45,1)){c->walking=1;wwhd_actor_set_animation(handle,"walk",.2f,1);}
}
static void talk(wwhd_actor_handle handle,void* data,u32 phase) {
    (void)data;(void)handle;
    if(phase==WWHD_ACTOR_TALK_BEGIN)wwhd_log("coral talk begin");
    else if(phase==WWHD_ACTOR_TALK_END)wwhd_log("coral talk end");
    else wwhd_log("coral talk cancel");
}
static void on_hit(wwhd_actor_handle handle,void* data,const wwhd_actor_hit_v1* hit) {
    Creature* c=(Creature*)data;
    if(hit->size!=64||hit->version!=1||hit->sequence==c->last_sequence)return;
    c->last_sequence=hit->sequence;++c->hits;
    state.size=sizeof state;state.version=1;
    if(!wwhd_actor_get(handle,&state)||(state.flags&WWHD_ACTOR_SCENE_OWNED))return;
    if(wwhd_actor_stop(handle)){c->pause=1;c->walking=0;wwhd_actor_set_animation(handle,"wave",.1f,0);}
    wwhd_log_int("coral informational hits",(s32)c->hits);
}
static const wwhd_actor_behavior_v1 behavior={32,1,sizeof(Creature),create,execute,0,talk,on_hit};
WWHD_HOOK(WWHD_ADDR_daPy_Execute,register_creature,(void* link)) {
    (void)link;
    if(!registered&&wwhd_actor_register("coral-wanderer",&behavior)){
        registered=1;wwhd_scene_music_register(music);
    }
}
