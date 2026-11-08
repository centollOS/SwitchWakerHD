#include "mods/story_scene_motion.h"
#include <cassert>
#include <limits>
#include <iostream>
using namespace story_scenes::motion;
static bool near(double a,double b){return std::fabs(a-b)<1e-9;}
int main(){
    const std::vector<Key> keys={{{{0,10,20},{0,0,0}},10},{{{10,20,30},{10,0,0}},20},{{{30,40,50},{30,0,0}},0}};
    Path path;Camera c;assert(!path.sample(0,c));assert(path.set(keys,Easing::Linear)&&path.duration()==30);
    assert(path.sample(-1,c)&&c.eye.x==0);assert(path.sample(10,c)&&c.eye.x==10);assert(path.sample(20,c)&&c.eye.x==20);assert(path.sample(30,c)&&c.eye.x==30);assert(path.sample(100,c)&&c.eye.x==30);assert(!path.sample(std::numeric_limits<double>::quiet_NaN(),c));
    assert(path.set(keys,Easing::Smoothstep));assert(path.sample(2.5,c)&&near(c.eye.x,1.5625));assert(path.sample(5,c)&&near(c.eye.x,5));assert(path.sample(10,c)&&c.eye.x==10);
    for(double t=0;t<=30;t+=0.25){assert(path.sample(t,c));assert(c.eye.x>=0&&c.eye.x<=30&&c.eye.y>=10&&c.eye.y<=40);}
    Camera before,after;assert(path.sample(10-1e-4,before)&&path.sample(10+1e-4,after));assert(std::fabs(after.eye.x-before.eye.x)<1e-7); // zero endpoint velocity at junction
    auto bad=keys;bad[0].frames_to_next=0;assert(!path.set(bad,Easing::Linear)&&path.duration()==0&&!path.sample(1,c));bad=keys;bad[0].frames_to_next=18000;assert(!path.set(bad,Easing::Linear));bad=keys;bad.back().frames_to_next=1;assert(!path.set(bad,Easing::Linear));bad=keys;bad[1].camera.eye.x=std::numeric_limits<double>::infinity();assert(!path.set(bad,Easing::Linear));assert(!path.set({keys[0]},Easing::Linear));
    Follow f;assert(f.start(7,{0,0,0},{0,10,20},{0,0,0},1));assert(f.update(7,{10,0,0},1,c)&&near(c.eye.x,5)&&near(c.target.x,5));
    Follow a,b;assert(a.start(7,{0,0,0},{0,10,20},{0,0,0},0.2)&&b.start(7,{0,0,0},{0,10,20},{0,0,0},0.2));Camera ca,cb;for(int i=0;i<30;++i)assert(a.update(7,{10,0,0},1.0/30,ca));for(int i=0;i<60;++i)assert(b.update(7,{10,0,0},1.0/60,cb));assert(near(ca.eye.x,cb.eye.x)&&near(ca.target.x,cb.target.x));
    assert(!f.update(8,{10,0,0},0.1,c));assert(!f.update(7,{10,0,0},0.1,c)); // stale target remains invalid
    assert(!f.start(0,{0,0,0},{0,0,0},{0,0,0},1));assert(!f.start(1,{350000,0,0},{1,0,0},{0,0,0},1));assert(f.start(1,{0,0,0},{0,1,0},{0,0,0},0));assert(f.update(1,{5,0,0},0.1,c)&&c.eye.x==5);f.reset();assert(!f.update(1,{0,0,0},0.1,c));
    std::cout<<"scene motion tests passed\n";
}
