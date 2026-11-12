#pragma once
#include "TailDynamics.h"
#include <cstring>

namespace flowing_hair
{
    // Persistent attachment-local centerline. Rebase the OLD points through
    // inverse(new attachment) * old attachment before pulling from the new base.
    // This retains world-space history without large world-coordinate storage.
    class TailFollow
    {
        enum { N=24 };
        Vec3 mPoints[N+1];
        float mFrame[12];
        bool mReady;
        std::vector<Vec3> mOutput;
        static Vec3 cross(const Vec3& a,const Vec3& b)
        { return Vec3(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x); }
        static Vec3 transform(const float* a,const Vec3& p)
        { return Vec3(a[0]*p.x+a[1]*p.y+a[2]*p.z+a[3],a[4]*p.x+a[5]*p.y+a[6]*p.z+a[7],a[8]*p.x+a[9]*p.y+a[10]*p.z+a[11]); }
        static bool valid(const Vec3& v)
        { return v.x>=-FLT_MAX && v.x<=FLT_MAX && v.y>=-FLT_MAX && v.y<=FLT_MAX && v.z>=-FLT_MAX && v.z<=FLT_MAX; }
        static Vec3 rotateBetween(const Vec3& p,Vec3 a,Vec3 b)
        {
            const float al=length(a),bl=length(b);
            if(al<0.000001f || bl<0.000001f) return p;
            a=a/al;b=b/bl;
            const float c=std::max(-1.0f,std::min(1.0f,dot(a,b)));
            const Vec3 axis=cross(a,b);
            if(c<-.9999f)
            {
                Vec3 perpendicular=cross(a,std::fabs(a.x)<.8f ? Vec3(1,0,0) : Vec3(0,0,1));
                perpendicular=perpendicular/length(perpendicular);
                return perpendicular*(2*dot(perpendicular,p))-p;
            }
            return p+cross(axis,p)+cross(axis,cross(axis,p))/(1+c);
        }
    public:
        TailFollow() : mReady(false) { std::memset(mFrame,0,sizeof(mFrame)); }
        void reset() { mReady=false;mOutput.clear(); }
        static float maximumReachDisplacement(const TailDynamics& target,const std::vector<Vec3>& rest)
        {
            // Any length-preserving chain pose lies within its accumulated link
            // length of the base. Add section radius and the rest point's distance
            // from that base to bound displacement, including a fully folded tail.
            float arc[N+1]={0};
            const Vec3 root=target.center(0,false);
            const float height=root.y-target.center(N,false).y;
            if(!(height>0) || rest.empty()) return 0;
            for(int i=1;i<=N;++i) arc[i]=arc[i-1]+length(target.center(i,false)-target.center(i-1,false));
            float result=0;
            for(size_t v=0;v<rest.size();++v)
            {
                if(!valid(rest[v])) return 0;
                if(rest[v].y>=root.y) continue;
                const float sample=std::max(0.0f,std::min(static_cast<float>(N),(root.y-rest[v].y)/height*N));
                const int i=std::min(N-1,static_cast<int>(sample));const float t=sample-i;
                const Vec3 center=target.center(i,false)*(1-t)+target.center(i+1,false)*t;
                const float reach=arc[i]*(1-t)+arc[i+1]*t+length(rest[v]-center)+length(rest[v]-root);
                if(!(reach>=0 && reach<=FLT_MAX)) return 0;
                result=std::max(result,reach);
            }
            return result*1.01f+.01f; // floating-point margin, not a motion tether
        }
        bool outputWithinReach(const std::vector<Vec3>& rest,float limit) const
        {
            if(mOutput.size()!=rest.size() || rest.empty() || !(limit>0 && limit<100000)) return false;
            for(size_t i=0;i<rest.size();++i)
                if(!valid(mOutput[i]) || !(lengthSquared(mOutput[i]-rest[i])<=limit*limit)) return false;
            return true;
        }
        bool recoverOutput(const TailDynamics& target,const std::vector<Vec3>& rest,
            const float* frame,float limit,bool& recovered)
        {
            recovered=false;
            if(outputWithinReach(rest,limit)) return true;
            // Numeric state failure is local/recoverable, not a broken render
            // relationship. Keep physics active if the current target is valid.
            reset();recovered=true;
            return advance(target,rest,frame,0,50) && outputWithinReach(rest,limit);
        }
        bool advance(const TailDynamics& target,const std::vector<Vec3>& rest,
            const float* frame,float dt,float rigidity)
        {
            if(!frame || rest.empty() || !(dt>=0 && dt<=FLT_MAX)) return false;
            for(int i=0;i<12;++i) if(!(frame[i]>=-FLT_MAX && frame[i]<=FLT_MAX)) return false;
            const Vec3 x(frame[0],frame[4],frame[8]),y(frame[1],frame[5],frame[9]),z(frame[2],frame[6],frame[10]);
            const float determinant=dot(x,cross(y,z));
            if(!(determinant>0.000001f && determinant<1000000)) return false;
            const Vec3 ix=cross(y,z)/determinant,iy=cross(z,x)/determinant,iz=cross(x,y)/determinant;
            const Vec3 translation(frame[3],frame[7],frame[11]);
            bool restart=!mReady || dt>.25f;
            if(mReady && dt>0)
            {
                // Subtract translations BEFORE multiplying, preserving precision
                // far from the world origin. Transform directions separately.
                float delta[12];
                const Vec3 oldTranslation(mFrame[3],mFrame[7],mFrame[11]);
                const Vec3 difference=oldTranslation-translation;
                for(int c=0;c<4;++c)
                {
                    const Vec3 column=c==3 ? difference : Vec3(mFrame[c],mFrame[4+c],mFrame[8+c]);
                    delta[c]=dot(ix,column);delta[4+c]=dot(iy,column);delta[8+c]=dot(iz,column);
                }
                // Teleports/major pose discontinuities reset, ordinary turns do not.
                if(lengthSquared(difference)>100 || delta[0]+delta[5]+delta[10]<0) restart=true;
                if(!restart) for(int i=1;i<=N;++i) mPoints[i]=transform(delta,mPoints[i]);
            }
            if(restart) for(int i=0;i<=N;++i) mPoints[i]=target.center(i,true);
            mReady=true;std::memcpy(mFrame,frame,sizeof(mFrame));
            mPoints[0]=target.center(0,true);
            rigidity=rigidity>=0 && rigidity<=100 ? rigidity : 50;
            // Fast response close to the base; progressively slower follow-through
            // down the chain. Bounded fixed substeps and exact link lengths.
            const float followTime=.045f-rigidity*.00025f;
            float remaining=std::min(dt,1.0f/15.0f);
            for(int step=0;remaining>0 && step<16;++step)
            {
                const float h=std::min(remaining,1.0f/240.0f);remaining-=h;
                const float blend=1-std::exp(-h/followTime);
                for(int i=1;i<=N;++i)
                {
                    const Vec3 link=target.center(i,true)-target.center(i-1,true);
                    const float restLength=length(link);
                    Vec3 next=mPoints[i]+(mPoints[i-1]+link-mPoints[i])*blend;
                    const Vec3 direction=next-mPoints[i-1];const float distance=length(direction);
                    mPoints[i]=mPoints[i-1]+(distance>.000001f ? direction*(restLength/distance) : link);
                    if(!valid(mPoints[i])) { reset();return false; }
                }
            }
            mOutput.resize(rest.size());
            for(size_t i=0;i<rest.size();++i) mOutput[i]=deform(target,rest[i]);
            return true;
        }
        Vec3 deform(const TailDynamics& target,const Vec3& p) const
        {
            const float top=target.center(0,false).y,height=top-target.center(N,false).y;
            if(!mReady || height<=0 || p.y>=top) return p;
            const float s=std::max(0.0f,std::min(static_cast<float>(N),(top-p.y)/height*N));
            const int i=std::min(N-1,static_cast<int>(s));const float t=s-i;
            const Vec3 restCenter=target.center(i,false)*(1-t)+target.center(i+1,false)*t;
            const Vec3 bent=mPoints[i]*(1-t)+mPoints[i+1]*t;
            return bent+rotateBetween(p-restCenter,target.center(i+1,false)-target.center(i,false),mPoints[i+1]-mPoints[i]);
        }
        const std::vector<Vec3>& positions() const { return mOutput; }
        Vec3 center(int i) const { return mPoints[i]; }
    };
}
