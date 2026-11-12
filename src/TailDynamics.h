#pragma once
#include "ClothSolver.h"
#include <algorithm>
#include <cmath>
#include <cfloat>

namespace flowing_hair
{
    // Tail-only reduced-order rod. Rigid cross sections and rotated rest-length
    // centerline segments cannot buckle like the old hollow surface cloth.
    class TailDynamics
    {
        enum { Segments=24 };
        std::vector<Vec3> mRest,mOutput;
        Vec3 mCenters[Segments+1],mBent[Segments+1];
        float mRoot,mHeight,mPitch,mSide,mPitchVelocity,mSideVelocity,mPhase;
        double mTime;
        Vec3 mTurnLag;
        static bool finite(float v) { return v==v && v>=-FLT_MAX && v<=FLT_MAX; }
        static float clamp(float v,float lo,float hi) { return std::max(lo,std::min(hi,v)); }
        static Vec3 rotate(const Vec3& p,float pitch,float side)
        {
            const float cp=std::cos(pitch),sp=std::sin(pitch),cs=std::cos(side),ss=std::sin(side);
            const float y=cp*p.y-sp*p.z;
            return Vec3(cs*p.x-ss*y,ss*p.x+cs*y,sp*p.y+cp*p.z);
        }
        static void spring(float target,float dt,float lo,float hi,float& angle,float& velocity)
        {
            velocity+=(16.0f*(target-angle)-8.0f*velocity)*dt;
            angle+=velocity*dt;
            if(angle<lo || angle>hi) { angle=clamp(angle,lo,hi); velocity=0; }
        }
        Vec3 rotateLag(const Vec3& p,float u) const
        {
            const float angle=length(mTurnLag)*u;
            if(angle<0.000001f) return p;
            const Vec3 axis=mTurnLag/length(mTurnLag);
            const Vec3 cross(axis.y*p.z-axis.z*p.y,axis.z*p.x-axis.x*p.z,axis.x*p.y-axis.y*p.x);
            const float c=std::cos(angle),s=std::sin(angle);
            return p*c+cross*s+axis*(dot(axis,p)*(1-c));
        }
        void rebuild()
        {
            mBent[0]=mCenters[0];
            for(int i=1;i<=Segments;++i)
            {
                const float u=(i-0.5f)/Segments;
                mBent[i]=mBent[i-1]+rotateLag(rotate(mCenters[i]-mCenters[i-1],mPitch*u,mSide*u),u);
            }
            for(size_t i=0;i<mRest.size();++i) mOutput[i]=deform(mRest[i]);
        }
    public:
        TailDynamics() : mRoot(0),mHeight(0),mPitch(0),mSide(0),mPitchVelocity(0),mSideVelocity(0),mPhase(0),mTime(0) {}
        bool initialise(const std::vector<Vec3>& rest,float phase)
        {
            mRest.clear();mOutput.clear();mHeight=0;
            if(rest.size()<3 || !finite(phase)) return false;
            float low=FLT_MAX,high=-FLT_MAX;
            for(size_t i=0;i<rest.size();++i)
            {
                if(!finite(rest[i].x)||!finite(rest[i].y)||!finite(rest[i].z)) return false;
                low=std::min(low,rest[i].y); high=std::max(high,rest[i].y);
            }
            mRoot=high-(high-low)*0.06f;mHeight=mRoot-low;
            if(mHeight<0.1f || mHeight>20) return false;
            mRest=rest;mOutput=rest;mPhase=phase;
            for(int ring=0;ring<=Segments;++ring)
            {
                const float y=mRoot-mHeight*ring/Segments;
                Vec3 sum;size_t count=0,nearest=0;float nearestDistance=FLT_MAX;
                for(size_t i=0;i<rest.size();++i)
                {
                    const float distance=std::fabs(rest[i].y-y);
                    if(distance<nearestDistance) { nearest=i;nearestDistance=distance; }
                    if(distance<=mHeight/Segments*0.5f) { sum+=rest[i];++count; }
                }
                mCenters[ring]=count ? sum/static_cast<float>(count) : rest[nearest];
                mCenters[ring].y=y;
            }
            reset();return true;
        }
        void reset()
        { mPitch=mSide=mPitchVelocity=mSideVelocity=0;mTime=0;mTurnLag=Vec3();if(!mRest.empty()) rebuild(); }
        void advance(float dt,const Vec3& force,float rigidity,float wetness,bool swish=true,const Vec3& turnDelta=Vec3())
        {
            if(mRest.empty() || !finite(dt) || dt<=0) return;
            if(!finite(force.x)||!finite(force.y)||!finite(force.z)) return;
            rigidity=finite(rigidity) ? clamp(rigidity,0,100) : 50;
            wetness=finite(wetness) ? clamp(wetness,0,1) : 0;
            Vec3 turn=turnDelta;
            // Discontinuous pose changes are resets, not huge angular impulses.
            if(!finite(turn.x)||!finite(turn.y)||!finite(turn.z)||lengthSquared(turn)>1.44f || dt>0.25f)
            { turn=Vec3();mTurnLag=Vec3(); }
            // The rear-mounted root sweeps sideways during hip yaw. A hanging
            // tail needs a lateral trailing bend too, not only a twist around Y.
            turn.z-=turn.y*0.55f;
            const float lagTime=0.28f-rigidity*0.0012f;
            const float lagStrength=0.9f-rigidity*0.004f;
            const float resistance=(0.5f+rigidity*0.015f)*(1+wetness*0.65f)+std::max(0.0f,-force.y)*0.025f;
            float remaining=std::min(dt,1.0f/15.0f);
            for(int step=0;remaining>0 && step<16;++step)
            {
                const float h=std::min(remaining,1.0f/240.0f);remaining-=h;mTime+=h;
                // Exact first-order response to angular velocity over this step.
                // Oppose owner rotation; decay after it stops. No pose/palette delay.
                const float decay=std::exp(-h/lagTime);
                mTurnLag=mTurnLag*decay-turn*(lagStrength*lagTime*(1-decay)/dt);
                mTurnLag.x=clamp(mTurnLag.x,-0.08f,0.30f);
                mTurnLag.y=clamp(mTurnLag.y,-0.55f,0.55f);
                mTurnLag.z=clamp(mTurnLag.z,-0.40f,0.40f);
                const float idle=swish ? (0.085f*static_cast<float>(std::sin(mTime*1.6+mPhase))+
                    0.025f*static_cast<float>(std::sin(mTime*0.71+mPhase*1.73)))/(1+rigidity*0.009f+wetness) : 0;
                // Negative local Z is away from these verified rear-facing tails.
                // Limit inward bend instead of projecting/crushing surface vertices.
                const float pitch=clamp(-force.z*0.04f/resistance,-0.04f,1.35f);
                const float side=clamp(force.x*0.017f/resistance+idle,-0.65f,0.65f);
                spring(pitch,h,-0.04f,1.35f,mPitch,mPitchVelocity);
                spring(side,h,-0.65f,0.65f,mSide,mSideVelocity);
            }
            rebuild();
        }
        Vec3 deform(const Vec3& rest) const
        {
            if(mHeight<=0 || rest.y>=mRoot) return rest;
            const float u=clamp((mRoot-rest.y)/mHeight,0,1),sample=u*Segments;
            const int ring=std::min(Segments-1,static_cast<int>(sample));
            const float blend=sample-ring;
            const Vec3 center=mCenters[ring]*(1-blend)+mCenters[ring+1]*blend;
            const Vec3 bent=mBent[ring]*(1-blend)+mBent[ring+1]*blend;
            return bent+rotateLag(rotate(rest-center,mPitch*u,mSide*u),u);
        }
        const std::vector<Vec3>& positions() const { return mOutput; }
        Vec3 center(int ring,bool bent) const { return bent ? mBent[ring] : mCenters[ring]; }
        static int segmentCount() { return Segments; }
        // Diagnostic invariants for native offline regression tests.
        float centerlineLength(bool bent) const
        {
            float value=0;for(int i=1;i<=Segments;++i)
                value+=length((bent ? mBent[i] : mCenters[i])-(bent ? mBent[i-1] : mCenters[i-1]));
            return value;
        }
    };
}
