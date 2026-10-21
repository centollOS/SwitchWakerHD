// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
namespace mod_actor::look_bounds {
inline constexpr double Pi=3.14159265358979323846;
using Euler=std::array<double,3>;using Quaternion=std::array<double,4>;
inline double wrap(double a){a=std::fmod(a+Pi,2*Pi);if(a<0)a+=2*Pi;return a-Pi;}
inline Quaternion quaternion(Euler e){
    const double sx=std::sin(e[0]/2),cx=std::cos(e[0]/2),sy=std::sin(e[1]/2),cy=std::cos(e[1]/2),sz=std::sin(e[2]/2),cz=std::cos(e[2]/2);
    return {sx*cy*cz-cx*sy*sz,cx*sy*cz+sx*cy*sz,cx*cy*sz-sx*sy*cz,cx*cy*cz+sx*sy*sz};
}
inline bool nearest(Quaternion q,const Euler& anchor,Euler& out){
    double norm=0;for(double x:q){if(!std::isfinite(x))return false;norm+=x*x;}for(double a:anchor)if(!std::isfinite(a))return false;
    if(norm<1e-12||!std::isfinite(norm))return false;norm=std::sqrt(norm);for(double&x:q)x/=norm;
    const auto[x,y,z,w]=q;const double m20=2*(x*z-w*y),m21=2*(y*z+w*x),m22=1-2*(x*x+y*y),m10=2*(x*y+w*z),m00=1-2*(y*y+z*z),m01=2*(x*y-w*z),m02=2*(x*z+w*y);
    const double pitch=std::asin(std::clamp(-m20,-1.,1.));Euler first;
    if(std::fabs(std::cos(pitch))>1e-6)first={std::atan2(m21,m22),pitch,std::atan2(m10,m00)};
    else if(pitch>0){const double relation=std::atan2(m01,m02),difference=anchor[0]-anchor[2]+wrap(relation-(anchor[0]-anchor[2]));first={(anchor[0]+anchor[2]+difference)/2,pitch,(anchor[0]+anchor[2]-difference)/2};}
    else {const double relation=std::atan2(-m01,-m02),sum=anchor[0]+anchor[2]+wrap(relation-(anchor[0]+anchor[2]));first={(anchor[0]-anchor[2]+sum)/2,pitch,(-anchor[0]+anchor[2]+sum)/2};}
    const Euler second{first[0]+Pi,Pi-first[1],first[2]+Pi};double best=1e300;
    for(Euler candidate:{first,second}){double score=0;for(unsigned k=0;k<3;++k){candidate[k]=anchor[k]+wrap(candidate[k]-anchor[k]);score+=(candidate[k]-anchor[k])*(candidate[k]-anchor[k]);}if(score<best){out=candidate;best=score;}}
    return true;
}
// Returns the input quaternion BIT-FOR-BIT if its nearest local Euler angles
// already meet the bounds. No normalization/re-encoding drift accumulates.
inline bool project(Quaternion input,const Euler& anchor,double yaw,double pitch,bool bound_pitch,Quaternion& output){
    if(!std::isfinite(yaw)||yaw<0||yaw>Pi||!std::isfinite(pitch)||pitch<0||pitch>Pi/2)return false;
    Euler e;if(!nearest(input,anchor,e))return false;
    const double y=std::clamp(e[1],-yaw,yaw),x=bound_pitch?std::clamp(e[0],-pitch,pitch):e[0];
    if(x==e[0]&&y==e[1]){output=input;return true;}e[0]=x;e[1]=y;output=quaternion(e);return true;
}
}
