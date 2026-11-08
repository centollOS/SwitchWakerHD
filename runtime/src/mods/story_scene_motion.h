// Pure bounded camera motion. No game objects, pointers, or ownership live here.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
namespace story_scenes::motion {
struct Point {double x=0,y=0,z=0;};
struct Camera {Point eye,target;};
enum class Easing {Linear,Smoothstep};
struct Key {Camera camera;unsigned frames_to_next=0;};
inline bool valid(Point p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::fabs(p.x)<=350000&&std::fabs(p.y)<=350000&&std::fabs(p.z)<=350000;}
inline Point mix(Point a,Point b,double t){return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};}
class Path {
    std::vector<Key> keys_;unsigned duration_=0;Easing easing_=Easing::Linear;
public:
    bool set(std::vector<Key> keys,Easing easing){
        keys_.clear();duration_=0;if(keys.size()<2||keys.size()>16||(easing!=Easing::Linear&&easing!=Easing::Smoothstep))return false;
        for(size_t n=0;n<keys.size();++n){const auto& k=keys[n];if(!valid(k.camera.eye)||!valid(k.camera.target))return false;
            if(n+1==keys.size()){if(k.frames_to_next)return false;}
            else if(!k.frames_to_next||k.frames_to_next>18000-duration_)return false;else duration_+=k.frames_to_next;}
        keys_=std::move(keys);easing_=easing;return true;
    }
    unsigned duration()const{return keys_.empty()?0:duration_;}
    bool sample(double elapsed,Camera& result)const{
        if(keys_.empty()||!std::isfinite(elapsed))return false;
        if(elapsed<=0){result=keys_.front().camera;return true;}if(elapsed>=duration_){result=keys_.back().camera;return true;}
        for(size_t n=0;n+1<keys_.size();++n){const unsigned span=keys_[n].frames_to_next;if(elapsed<=span){double t=elapsed/span;if(easing_==Easing::Smoothstep)t=t*t*(3-2*t);result={mix(keys_[n].camera.eye,keys_[n+1].camera.eye,t),mix(keys_[n].camera.target,keys_[n+1].camera.target,t)};return true;}elapsed-=span;}
        return false;
    }
};
// A native adapter resolves an owned character each step. Identity changes abort
// the follow shot instead of silently following a reused native address.
class Follow {
    unsigned long long identity_=0;Point eye_offset_,target_offset_;Camera current_;bool ready_=false;double half_life_=0;
public:
    bool start(unsigned long long identity,Point position,Point eye_offset,Point target_offset,double half_life_seconds){
        ready_=false;if(!identity||!valid(position)||!valid(eye_offset)||!valid(target_offset)||!std::isfinite(half_life_seconds)||half_life_seconds<0||half_life_seconds>10)return false;
        const Camera initial{{position.x+eye_offset.x,position.y+eye_offset.y,position.z+eye_offset.z},{position.x+target_offset.x,position.y+target_offset.y,position.z+target_offset.z}};
        if(!valid(initial.eye)||!valid(initial.target))return false;identity_=identity;eye_offset_=eye_offset;target_offset_=target_offset;half_life_=half_life_seconds;current_=initial;ready_=true;return true;
    }
    bool update(unsigned long long identity,Point position,double dt,Camera& out){
        if(!ready_||identity!=identity_||!valid(position)||!std::isfinite(dt)||dt<0||dt>1){ready_=false;return false;}
        const Camera goal{{position.x+eye_offset_.x,position.y+eye_offset_.y,position.z+eye_offset_.z},{position.x+target_offset_.x,position.y+target_offset_.y,position.z+target_offset_.z}};
        if(!valid(goal.eye)||!valid(goal.target)){ready_=false;return false;}
        const double alpha=half_life_==0?1:-std::expm1(-std::log(2.0)*dt/half_life_);current_={mix(current_.eye,goal.eye,alpha),mix(current_.target,goal.target,alpha)};out=current_;return true;
    }
    void reset(){ready_=false;identity_=0;}
};
}
