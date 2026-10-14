#include "aspect_panes.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace aspect::panes;
static void check(bool ok) { if (!ok) { std::fprintf(stderr,"pane policy check failed\n"); std::exit(1); } }
// a pane's rule; child: the name of its one child, if any
static const PaneRule* rule(const char* name, bool rootChild, bool leaf, float x, float y, float w, float h, const char* child = nullptr) {
    return rule_for(name, rootChild, leaf, x, y, w, h, [child](std::string_view c) { return child && c == child; });
}
// where the pane goes: its rule applied, or unchanged (content)
static Transform place(const char* name, bool rootChild, bool leaf, float x, float y, float sx, float sy, float w, float h,
                       float kx, float ky, const char* child = nullptr) {
    const PaneRule* r = rule(name, rootChild, leaf, x, y, w, h, child);
    return r ? apply(*r, x, y, sx, sy, w, h, kx, ky) : Transform{x, y, sx, sy};
}
struct Pane { const char* name; float x,y,w,h; std::vector<Pane> children; };
static Transform place(const Pane& p, bool rootChild, float kx, float ky) {
    return place(p.name, rootChild, p.children.empty(), p.x, p.y, 1, 1, p.w, p.h, kx, ky);
}
int main() {
    Pane tree{"RootPane",0,0,1280,720,{
        {"P_BG_00",0,0,1280,720,{}},
        {"PF_PauseTV_00",0,0,1280,720,{}},
        {"W_Base_00",640,0,2480,700,{{"W_ItemBase_00",347,-92,478,414,{}}}},
        {"P_ItemMask_00",0,0,1290,730,{}},
        {"B_Slide_00",0,0,1280,720,{}}
    }};
    for(float a : {16.f/9,64.f/27,32.f/9,16.f/10,4.f/3}) {
        float kx=std::max(1.f,a/(16.f/9)),ky=std::max(1.f,(16.f/9)/a);
        check(place(tree,false,kx,ky).sx==1);
        for(int i=0;i<2;i++) {auto t=place(tree.children[i],true,kx,ky);check(t.sx==kx && t.sy==ky);}
        for(int i=2;i<5;i++) {auto t=place(tree.children[i],true,kx,ky);check(t.sx==1 && t.sy==1 && t.x==tree.children[i].x);}
        auto box=place(tree.children[2].children[0],false,kx,ky);check(box.x==347 && box.y==-92);
        // HUD at the edges, only as a child of the layout's root
        auto hud=place("N_TV_00",true,false,-601,338,1,1,30,40,kx,ky);check(hud.x==-601-640*(kx-1) && hud.y==338+360*(ky-1));
        auto deep=place("N_TV_00",false,false,-601,338,1,1,30,40,kx,ky);check(deep.x==-601 && deep.y==338);
        auto speak=place("L_CommandA_00",true,false,0,-247,1,1,270,114,kx,ky);check(speak.x==0 && speak.y==-247-360*(ky-1));
        // N_All_00 only for the button cluster and the place name, told apart by their children
        auto cluster=place("N_All_00",true,false,509,238,1,1,220,245,kx,ky,"P_ZR_00");
        check(cluster.x==509+640*(kx-1) && cluster.y==238+360*(ky-1));
        auto choice=place("N_All_00",true,false,0,-259,1,1,30,40,kx,ky,"N_ChoiseLR_00");check(choice.x==0 && choice.y==-259);
        // the area name (PlaceName_00): only faded in and out by its animations, always top left
        auto area=place("N_All_00",true,false,-597,291,1,1,30,40,kx,ky,"T_PlaceName_00");
        check(area.x==-597-640*(kx-1) && area.y==291+360*(ky-1));
        auto world=place("L_EnemyHP_00",true,false,200,100,1,1,205,40,kx,ky);check(world.x==200*kx && world.y==100*ky);
        // the Hero's Charm gauge: the game moves N_EnemyHP_00 inside the part
        auto gauge=place("N_EnemyHP_00",false,false,-144,103,1,1,30,40,kx,ky);check(gauge.x==-144*kx && gauge.y==103*ky);
        // The telescope's circular wipe (1716x1716) is only stretched once it no longer covers the screen.
        auto wipe=place("W_Wipe_00",false,true,0,0,66,66,1716,1716,kx,ky);
        check(wipe.sy==66 && (1280*kx<=1716 ? wipe.sx==66 : wipe.sx*26>=1280*kx-0.5f));
        // Pictograph Box frame bars reach the new screen edges and keep their inner edges.
        auto top=place("P_FramerT_00",false,true,0,360,1,1,1280,140,kx,ky);
        check(std::fabs(top.y-360*ky)<0.01f && std::fabs(top.y-140*top.sy-220)<0.01f && top.sx==kx);
        auto bottom=place("P_FrameB_00",false,true,0,-360,1,1,1280,265,kx,ky);
        check(std::fabs(bottom.y+360*ky)<0.01f && std::fabs(bottom.y+265*bottom.sy+95)<0.01f);
        auto left=place("P_FrameL_00",false,true,-640,220,1,1,360,315,kx,ky);
        check(std::fabs(left.x+640*kx)<0.01f && std::fabs(left.x+360*left.sx+280)<0.01f && left.sy==1);
        auto right=place("P_FramerR_00",false,true,640,220,1,1,360,315,kx,ky);
        check(std::fabs(right.x-640*kx)<0.01f && std::fabs(right.x-360*right.sx-280)<0.01f);
        // Stick prompts and the conducting bar move up with the top edge.
        auto stick=place("P_RStick_00",false,false,255,198,1,1,60,60,kx,ky);check(stick.x==255 && stick.y==198+360*(ky-1));
        // Auction bid box: with the message window above it, in the top-left corner at the other end.
        auto talk=place("N_AuctionRupy_00",false,false,-321,-56,1,1,30,40,kx,ky);
        auto corner=place("N_AuctionRupy_00",false,false,-551,295,1,1,30,40,kx,ky);
        check(talk.x==-321 && std::fabs(talk.y-(-56-360*(ky-1)))<0.01f);
        check(std::fabs(corner.x-(-551-640*(kx-1)))<0.01f && std::fabs(corner.y-(295+360*(ky-1)))<0.01f);
        auto meter=place("N_Meter_00",false,false,-7,-280,1,1,30,40,kx,ky);check(meter.x==-7 && meter.y==-280-360*(ky-1));
        // Exact native scissor on integral-size targets, including tall pictures and 2x resolution.
        for (uint32_t scale : {1u,2u,3u}) {
            uint32_t w=uint32_t(std::lround(1280*scale*kx)),h=uint32_t(std::lround(720*scale*ky));
            uint32_t x=0,y=0,ex=w,ey=h;clip(w,h,kx,ky,x,y,ex,ey);
            check(ex-x<=1280*scale && ey-y<=720*scale && ex-x>=1280*scale-1 && ey-y>=720*scale-1);
            if (a != 64.f/27) check(ex-x==1280*scale && ey-y==720*scale);
            uint32_t ox=x,oy=y;clip(w,h,kx,ky,x,y,ex,ey);check(x==ox&&y==oy);
        }
    }
    check(parked_hud(700,0) && parked_hud(0,400) && !parked_hud(640,360));
    check(!is_marker("W_Cursor_00") && !is_marker("N_FloorCenter_00") && is_marker("N_EnemyHP_00"));
    check(!rule("T_CommandA_00",false,false,0,0,30,40) && is_marker("T_CommandA_00"));  // a marker is not moved itself
    // backgrounds only as a leaf at the centre covering the screen; frame bars only as a leaf
    check(!rule("P_BG_00",true,false,0,0,1280,720) && !rule("P_BG_00",true,true,1280,0,1280,720) && !rule("P_BG_00",true,true,0,0,300,200));
    check(rule("P_FrameB_00",false,true,0,-360,1280,265) && !rule("P_FrameB_00",false,false,0,-360,1280,265));
    // the message window's own N_BatonSign_00 (a song's notes) stays
    check(rule("N_BatonSign_00",false,false,0,245,30,40) && !rule("N_BatonSign_00",false,false,0,-26,562,58));
    check(!rule("N_All_00",true,false,0,0,30,40));
    uint32_t x=0,y=0,ex=1280,ey=720;clip(1280,720,1,1,x,y,ex,ey);check(x==0&&y==0&&ex==1280&&ey==720);
    std::puts("pane rules: five aspects, filters, frame trees, cursor alignment, HUD and clipping passed");
}
