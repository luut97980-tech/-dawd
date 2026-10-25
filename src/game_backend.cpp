#include "game_backend.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace vt::ios {
namespace {
using Obj=void*;using Klass=void*;using Field=void*;using Method=const void*;
struct Vec3 {float x=0,y=0,z=0;};
struct Quat {float x=0,y=0,z=0,w=1;};
struct Bounds {Vec3 center{},extents{};};
struct Rect {float x=0,y=0,w=0,h=0;};

bool valid(Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::fabs(p.x)<100000.f&&std::fabs(p.y)<100000.f&&std::fabs(p.z)<100000.f;}
bool validBounds(const Bounds&b){
    return valid(b.center)&&valid(b.extents)&&b.extents.x>=0.f&&b.extents.y>=0.f&&b.extents.z>=0.f&&
        b.extents.x<=100000.f&&b.extents.y<=100000.f&&b.extents.z<=100000.f;
}
float distXZ(Vec3 a,Vec3 b){return std::hypot(a.x-b.x,a.z-b.z);}
float dist3D(Vec3 a,Vec3 b){return std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z));}

bool safeReadMemory(const void*src,void*dst,std::size_t size){
    if(!src||!dst||size==0||size>0x1000)return false;
    auto addr=reinterpret_cast<std::uintptr_t>(src);
    if(addr<0x10000)return false;
#if defined(__APPLE__)
    vm_size_t outSize=0;
    if(vm_read_overwrite(mach_task_self(),static_cast<vm_address_t>(addr),static_cast<vm_size_t>(size),
        reinterpret_cast<vm_address_t>(dst),&outSize)!=KERN_SUCCESS||outSize!=size)return false;
    return true;
#else
    std::memcpy(dst,src,size);
    return true;
#endif
}

template<class T>
bool safeReadAt(const void*base,std::size_t offset,T&out){
    if(!base)return false;
    auto addr=reinterpret_cast<std::uintptr_t>(base);
    if(addr<0x10000)return false;
    return safeReadMemory(reinterpret_cast<const char*>(base)+offset,&out,sizeof(T));
}

template<class T>
bool safeWriteAt(void*base,std::size_t offset,const T&val){
    if(!base)return false;
    T probe{};
    if(!safeReadAt(base,offset,probe))return false;
    std::memcpy(reinterpret_cast<char*>(base)+offset,&val,sizeof(T));
    return true;
}

bool isValidObjectPointer(Obj o){
    if(!o)return false;
    auto addr=reinterpret_cast<std::uintptr_t>(o);
    if(addr<0x10000||(addr&(sizeof(void*)-1))!=0)return false;
    void* klassPtr=nullptr;
    if(!safeReadAt(o,0,klassPtr)||!klassPtr)return false;
    auto kaddr=reinterpret_cast<std::uintptr_t>(klassPtr);
    if(kaddr<0x10000||(kaddr&(sizeof(void*)-1))!=0)return false;
    void* probe=nullptr;
    if(!safeReadAt(klassPtr,0,probe))return false;
    return true;
}

bool iequals(const char*a,const char*b){
    if(!a||!b)return false;
    while(*a&&*b){
        if(std::tolower(static_cast<unsigned char>(*a))!=std::tolower(static_cast<unsigned char>(*b)))return false;
        ++a;++b;
    }
    return *a==*b;
}

struct API {
    void*(*domain_get)()=nullptr;
    void**(*domain_get_assemblies)(void*,std::size_t*)=nullptr;
    void*(*assembly_get_image)(void*)=nullptr;
    const char*(*image_get_name)(void*)=nullptr;
    Klass(*class_from_name)(void*,const char*,const char*)=nullptr;
    Klass(*class_get_parent)(Klass)=nullptr;
    const char*(*class_get_name)(Klass)=nullptr;
    const char*(*class_get_namespace)(Klass)=nullptr;
    Method(*class_get_method_from_name)(Klass,const char*,int)=nullptr;
    Field(*class_get_field_from_name)(Klass,const char*)=nullptr;
    const void*(*field_get_type)(Field)=nullptr;
    int(*type_get_type)(const void*)=nullptr;
    void(*field_get_value)(Obj,Field,void*)=nullptr;
    void(*field_static_get_value)(Field,void*)=nullptr;
    Obj(*runtime_invoke)(Method,Obj,void**,Obj*)=nullptr;
    Klass(*object_get_class)(Obj)=nullptr;
    void*(*object_unbox)(Obj)=nullptr;
    std::uintptr_t(*gchandle_new)(Obj,bool)=nullptr;
    Obj(*gchandle_get_target)(std::uintptr_t)=nullptr;
    void(*gchandle_free)(std::uintptr_t)=nullptr;
    Obj(*array_new)(Klass,std::size_t)=nullptr;
    std::size_t(*array_length)(Obj)=nullptr;
    std::uint32_t(*array_object_header_size)()=nullptr;
    void*(*thread_current)()=nullptr;
    void*(*thread_attach)(void*)=nullptr;
    void(*thread_detach)(void*)=nullptr;
    // Optional IL2CPP helpers (resolved when present in UnityFramework)
    void(*field_set_value)(Obj,Field,void*)=nullptr;
    std::size_t(*field_get_offset)(Field)=nullptr;
    int(*field_get_flags)(Field)=nullptr;
    Field(*class_get_fields)(Klass,void**)=nullptr;
    const char*(*field_get_name)(Field)=nullptr;
    const void*(*class_get_type)(Klass)=nullptr;
    Obj(*type_get_object)(const void*)=nullptr;
    Method(*class_get_methods)(Klass,void**)=nullptr;
    const char*(*method_get_name)(Method)=nullptr;
    std::uint32_t(*method_get_param_count)(Method)=nullptr;
    const void*(*method_get_param)(Method,std::uint32_t)=nullptr;
    bool(*method_is_generic)(Method)=nullptr;
    bool hasNativeLayout=false;

    explicit API(const GameBackend::Resolver& r){
#define LOAD(n) n=reinterpret_cast<decltype(n)>(r("il2cpp_" #n));if(!n)throw std::runtime_error("Missing il2cpp_" #n)
        LOAD(domain_get);LOAD(domain_get_assemblies);LOAD(assembly_get_image);LOAD(image_get_name);
        LOAD(class_from_name);LOAD(class_get_parent);LOAD(class_get_name);LOAD(class_get_namespace);
        LOAD(class_get_method_from_name);LOAD(class_get_field_from_name);LOAD(field_get_type);LOAD(type_get_type);
        LOAD(field_get_value);LOAD(field_static_get_value);LOAD(runtime_invoke);
        LOAD(object_get_class);LOAD(object_unbox);LOAD(gchandle_new);LOAD(gchandle_get_target);LOAD(gchandle_free);
        LOAD(array_new);LOAD(array_length);LOAD(array_object_header_size);
        LOAD(thread_current);LOAD(thread_attach);LOAD(thread_detach);
#undef LOAD
#define OPT(n) n=reinterpret_cast<decltype(n)>(r("il2cpp_" #n))
        OPT(field_set_value);OPT(field_get_offset);OPT(field_get_flags);
        OPT(class_get_fields);OPT(field_get_name);
        OPT(class_get_type);OPT(type_get_object);
        OPT(class_get_methods);OPT(method_get_name);OPT(method_get_param_count);
        OPT(method_get_param);OPT(method_is_generic);
#undef OPT
        hasNativeLayout=(field_get_offset!=nullptr);
    }
};

struct Root {
    API* a=nullptr;std::uintptr_t h=0;
    Root()=default;Root(API&api,Obj o):a(&api),h(o?api.gchandle_new(o,false):0){if(o&&!h)throw std::runtime_error("GC handle failed");}
    Root(const Root&)=delete;Root&operator=(const Root&)=delete;
    Root(Root&&other)noexcept:a(other.a),h(other.h){other.a=nullptr;other.h=0;}
    Root&operator=(Root&&other)noexcept{if(this!=&other){reset();a=other.a;h=other.h;other.a=nullptr;other.h=0;}return *this;}
    ~Root(){reset();}
    Obj get()const{return h?a->gchandle_get_target(h):nullptr;}
    void reset(){if(h&&a){a->gchandle_free(h);h=0;}}
    void set(API&api,Obj o){auto next=o?api.gchandle_new(o,false):0;if(o&&!next)throw std::runtime_error("GC handle failed");reset();a=&api;h=next;}
};

struct WorldMarker {
    Vec3 anchor{};
    std::string color="green";
    std::string label;
    std::string key;
    int stage=-1;
    bool selected=false;
    bool showRing=true;
    bool showDistance=true;
};

struct RouteNode {
    Vec3 anchor{};
    std::string key;
    bool drop=false;
    int stage=0;
    bool goal=false;
};

struct ProjectedPt {
    float x=0,y=0,depth=0;
    bool ok=false;
};

bool isMiniControlName(const std::string&name){
    static const std::set<std::string> kMini={
        "ActorBattlePlayer","ActorCatchUpPlayer","ActorDuelPlayer","ActorFashionShowPlayer",
        "ActorInfiniteFloorPlayer","ActorKMGPlayer","ActorObbyPlayer","ActorPingPongPlayer",
        "ActorSmashOutPlayer","ActorSpeedUpPlayer","ActorTreasureHuntPlayer","ActorMiniGameControl"
    };
    return kMini.contains(name);
}

int parseDigits(const std::string&s,std::size_t pos,std::size_t len){
    if(pos+len>s.size())return -1;
    int v=0;
    for(std::size_t i=0;i<len;++i){
        if(!std::isdigit(static_cast<unsigned char>(s[pos+i])))return -1;
        v=v*10+(s[pos+i]-'0');
    }
    return v;
}

struct HoleLayout {
    std::string stageName;
    int stage=0;
    int limit=0;
    std::string variant;
};

[[maybe_unused]] bool parseHolePath(const std::vector<std::string>&path,HoleLayout&out){
    if(path.size()<7)return false;
    const std::size_t n=path.size();
    if(path[n-5]!="Hide_Stage"||path[n-2]!="Stamp")return false;
    const auto&sRoot=path[n-4];
    const auto&sBoard=path[n-3];
    const auto&sStamp=path[n-1];
    if(sRoot.size()!=15||sRoot.compare(0,6,"Stage_")!=0||sRoot.compare(9,6,"_Root")!=0)return false;
    int st=parseDigits(sRoot,6,2);
    int lim=parseDigits(sRoot,9,1);
    if(st<1||st>7||(lim!=1&&lim!=3&&lim!=5&&lim!=7))return false;
    if(sBoard.size()!=16||sBoard.compare(0,12,"Stage_Board_")!=0||sBoard[14]!='_')return false;
    if(parseDigits(sBoard,12,2)!=st)return false;
    char var=sBoard[15];
    if(var<'a'||var>'e')return false;
    if(sStamp.compare(0,15,"DropBoard_stage")!=0||sStamp.find("_Stamp_")==std::string::npos)return false;
    out={sRoot,st,lim,sBoard};
    return true;
}

[[maybe_unused]] bool parseHoleBoardPath(const std::vector<std::string>&path,int&stageOut,std::string&variantOut){
    if(path.size()<7)return false;
    const std::size_t n=path.size();
    if(path[n-5]!="Hide_Stage"||path[n-2]!="Board")return false;
    const auto&sBoard=path[n-3];
    if(sBoard.size()!=16||sBoard.compare(0,12,"Stage_Board_")!=0||sBoard[14]!='_')return false;
    int st=parseDigits(sBoard,12,2);
    char var=sBoard[15];
    if(st<1||st>7||var<'a'||var>'e')return false;
    stageOut=st;variantOut=sBoard;
    return true;
}

struct DoorChoice {
    std::string kind;
    std::string group;
    std::string side;
};

bool parseDoorIdentity(const std::vector<std::string>&path,DoorChoice&out){
    if(path.size()<5)return false;
    const std::size_t n=path.size();
    if(path[n-3]!="HideObject")return false;
    const auto&folder=path[n-2];
    const auto&leaf=path[n-1];
    if(folder=="Door_Root"){
        auto us=leaf.rfind('_');
        if(us==std::string::npos||us+2!=leaf.size()||leaf.compare(0,4,"door")!=0)return false;
        char s=leaf[us+1];
        if(s!='L'&&s!='C'&&s!='R')return false;
        out={"door",leaf.substr(0,us),std::string(1,s)};
        return true;
    }
    if(folder=="Obstacle_Root"&&(leaf=="plir_through"||leaf=="plir_wall")){
        out={"column","entrance-column",leaf=="plir_through"?"through":"wall"};
        return true;
    }
    if(folder=="Wall_Root"&&leaf.compare(0,21,"TwistRoad_Random_wall")==0){
        auto us=leaf.rfind('_');
        if(us!=std::string::npos&&us+2==leaf.size()&&(leaf[us+1]=='L'||leaf[us+1]=='R')){
            std::string num=leaf.substr(21,us-21);
            if(!num.empty()){
                out={"wall","wall"+num,std::string(1,leaf[us+1])};
                return true;
            }
        }
    }
    return false;
}

[[maybe_unused]] bool parseRoadVariant(const std::vector<std::string>&path,std::string&nameOut){
    if(path.size()<7)return false;
    const std::size_t n=path.size();
    if(path[n-2]!="Road_01_Root")return false;
    const auto&leaf=path[n-1];
    if(leaf.size()==7&&leaf.compare(0,5,"Road_")==0){
        int num=parseDigits(leaf,5,2);
        if(num>=1&&num<=27){nameOut=leaf;return true;}
    }
    return false;
}

[[maybe_unused]] int parseRoadLaneRow(const std::vector<std::string>&path){
    for(const auto&part:path){
        if(part.compare(0,10,"Road_line_")==0||part.compare(0,10,"road_line_")==0){
            if(part.size()==11&&part[10]>='1'&&part[10]<='3')return part[10]-'0';
            if(part.size()==12&&part[10]=='0'&&part[11]>='1'&&part[11]<='3')return part[11]-'0';
        }
    }
    return 0;
}

bool parseMazeVariant(const std::vector<std::string>&path,std::string&variantOut){
    for(std::size_t i=0;i+3<path.size();++i){
        if(path[i]=="RandomMaze_Root"){
            const auto&v=path[i+1];
            if(v.size()==17&&v.compare(0,13,"MazeCreate_S_")==0){
                int num=parseDigits(v,13,2);
                char dir=v[16];
                if(num>=1&&num<=22&&(dir=='R'||dir=='T'||dir=='B'||dir=='L')){
                    variantOut=v;return true;
                }
            }
        }
    }
    return false;
}

bool mazeSegmentHitsBox(Vec3 a,Vec3 b,const std::array<float,4>&box){
    float lo=0.f,hi=1.f;
    for(int axis=0;axis<2;++axis){
        float ak=axis==0?a.x:a.z,bk=axis==0?b.x:b.z;
        float delta=bk-ak,mn=box[axis],mx=box[axis+2];
        if(std::fabs(delta)<1e-9f){
            if(ak<mn||ak>mx)return false;
        }else{
            float t0=(mn-ak)/delta,t1=(mx-ak)/delta;
            lo=std::max(lo,std::min(t0,t1));
            hi=std::min(hi,std::max(t0,t1));
            if(lo>hi)return false;
        }
    }
    return true;
}

} // namespace

bool runtimeRequested(const Model&m){return m.enabled[Esp]||m.enabled[Line]||m.enabled[SpamJump]||m.enabled[AutoReady]||m.enabled[AutoHammer];}

struct GameBackend::State {
    API a;void* ownThread=nullptr;
    std::map<std::string,Klass> classes;
    std::map<std::tuple<Klass,std::string,int>,Method> methods;
    std::map<std::pair<Klass,std::string>,Field> fields;
    Root actorRoot,controlRoot,miniGameRoot;
    Root simpleDialogRoot,partyDialogRoot,uiSystemRoot;
    Root joyStickDialogRoot,jumpBtnTrRoot;
    double joyStickScanTime=-1;
    std::string actorUID;
    double nextJump=0,lastJump=-1,lastTick=-1;
    double settleUntil=0,retryAfter=0;
    bool idle=true,needsSettle=false;
    bool simpleLatched=false,partyLatched=false;
    double readyScanTime=-1;
    std::string hammerWindowKey;
    double hammerFightTime=-1;
    bool hammerLatched=false;
    std::uint64_t jumpRequests=0,readyRequests=0,hammerRequests=0;
    std::string readySummary,hammerSummary;
    std::vector<EspItem> points;
    std::vector<RouteSegment> routeSegments;
    std::vector<WorldMarker> worldMarkers;
    std::vector<RouteNode> routeNodes;
    int currentRouteStage=0;
    double espScanTime=-1;
    std::string status="Waiting for IL2CPP",controllerName,currentMapName,espSummary;
    int currentMapId=0;

    std::string cachedRoundKey;
    struct HoleTileRec {
        Root comp;
        Root tr;
        Root boardGo;
        Vec3 cachedPos{};
        bool hasCachedPos=false;
    };
    std::vector<HoleTileRec> holeTiles;
    std::map<std::string,Vec3> holeSavedAnchors;
    bool holeStructureReady=false;

    struct RoadVarRec {
        Root go;
        std::string name;
    };
    struct Road120026State {
        std::string key;
        std::vector<RoadVarRec> vars;
        Root activeGo;
        std::string activeName;
        std::vector<WorldMarker> cachedMarkers;
        std::string cachedSummary;
        bool activeSolved=false;
    } road120026;

    struct DoorObjRec {
        Root go;
        Root parentGo;
        DoorChoice choice;
        Vec3 anchor{};
        std::vector<Root> colliders;
    };
    struct Door120006State {
        std::string key;
        std::vector<DoorObjRec> items;
        bool ready=false;
    } doors120006;

    struct BoardTileRec {
        Root comp;
        Root go;
        Root col;
        Root stableGo;
        Root stableCol;
        Vec3 anchor{};
        Vec3 stableAnchor{};
        bool hasAnchor=false;
        bool hasStable=false;
    };
    struct BoardState {
        std::string key;
        std::vector<BoardTileRec> tiles;
        bool ready=false;
    } boardCache;

    Root activeCamRoot;
    Rect cachedViewport{0.f,0.f,1.f,1.f};
    double camCheckTime=-1;

    struct MazeState {
        std::string key;
        std::string activeVariant;
        Root meshCol;
        Bounds meshBounds{};
        std::vector<Root> boxColliders;
        std::vector<std::vector<std::string>> boxPaths;
        std::string routeSignature;
        std::vector<RouteNode> solvedNodes;
        bool routeReady=false;
    } maze;

    explicit State(Resolver r):a(r){}
    ~State(){clearCaches();controlRoot.reset();actorRoot.reset();if(ownThread)a.thread_detach(ownThread);}

    void clearCaches(){
        holeTiles.clear();holeSavedAnchors.clear();
        holeStructureReady=false;cachedRoundKey.clear();
        road120026=Road120026State{};
        doors120006=Door120006State{};
        boardCache=BoardState{};
        activeCamRoot.reset();camCheckTime=-1;
        maze=MazeState{};
        miniGameRoot.reset();
        simpleDialogRoot.reset();partyDialogRoot.reset();uiSystemRoot.reset();
        joyStickDialogRoot.reset();jumpBtnTrRoot.reset();joyStickScanTime=-1;
        simpleLatched=false;partyLatched=false;readyScanTime=-1;readySummary.clear();
        hammerWindowKey.clear();hammerFightTime=-1;hammerLatched=false;hammerSummary.clear();
        worldMarkers.clear();routeNodes.clear();
    }

    bool isExecutableMethod(Method m)const{
        if(!m)return false;
        if(a.method_is_generic&&a.method_is_generic(m))return false;
#if defined(__APPLE__)
        if(a.hasNativeLayout){
            void* fnPtr=nullptr;
            void* invPtr=nullptr;
            if(!safeReadAt(m,0,fnPtr)||!safeReadAt(m,sizeof(void*),invPtr)||!fnPtr||!invPtr)return false;
        }
#endif
        return true;
    }

    bool isInstanceField(Field f)const{
        if(!f)return false;
        if(a.field_get_flags){
            int flags=a.field_get_flags(f);
            if((flags&0x0010)!=0||(flags&0x0040)!=0)return false;
        }else if(a.hasNativeLayout){
            const void*ft=a.field_get_type(f);
            std::uint16_t attrs=0;
            if(ft&&safeReadAt(ft,0x08,attrs)&&((attrs&0x0010)!=0||(attrs&0x0040)!=0))return false;
        }
        if(a.field_get_offset){
            std::size_t off=a.field_get_offset(f);
            if(off<0x10||off>0x2000)return false;
        }
        return true;
    }

    Klass tryKlass(const char*image,const char*ns,const char*name){
        std::string key=std::string(image?image:"*")+":"+ns+":"+name;
        if(auto it=classes.find(key);it!=classes.end())return it->second;
        auto domain=a.domain_get();if(!domain)return nullptr;
        if(!a.thread_current()&&!ownThread){ownThread=a.thread_attach(domain);}
        std::size_t count=0;auto list=a.domain_get_assemblies(domain,&count);
        if(!list||count>4096)return nullptr;
        for(std::size_t i=0;i<count;++i){
            auto im=a.assembly_get_image(list[i]);const char*n=a.image_get_name(im);
            if(!n)continue;
            if(!image||!*image||std::strcmp(n,image)==0){
                auto c=a.class_from_name(im,ns,name);
                if(c){classes[key]=c;return c;}
            }
        }
        return nullptr;
    }

    Klass klass(const char*image,const char*ns,const char*name){
        auto domain=a.domain_get();if(!domain)throw std::runtime_error("Waiting for IL2CPP domain");
        if(!a.thread_current()&&!ownThread){ownThread=a.thread_attach(domain);if(!ownThread)throw std::runtime_error("IL2CPP attach failed");}
        std::size_t count=0;auto list=a.domain_get_assemblies(domain,&count);
        if(!list||count>4096)throw std::runtime_error("Waiting for assemblies");
        if(auto c=tryKlass(image,ns,name))return c;
        throw std::runtime_error(std::string("Class unavailable: ")+name);
    }

    Klass tryGame(const char*name){
        if(auto c=tryKlass("Assembly-CSharp.dll","",name))return c;
        if(auto c=tryKlass("Assembly-CSharp.dll","PlayTogether.MiniGame",name))return c;
        if(auto c=tryKlass("Assembly-CSharp.dll","PlayTogether",name))return c;
        return nullptr;
    }
    Klass game(const char*name){
        if(auto c=tryGame(name))return c;
        return klass("Assembly-CSharp.dll","",name);
    }
    Klass unity(const char*name){
        if(auto c=tryKlass("UnityEngine.CoreModule.dll","UnityEngine",name))return c;
        if(auto c=tryKlass("UnityEngine.PhysicsModule.dll","UnityEngine",name))return c;
        if(auto c=tryKlass(nullptr,"UnityEngine",name))return c;
        throw std::runtime_error(std::string("Unity class unavailable: ")+name);
    }

    Method tryMethod(Klass c,const char*name,int argc){
        if(!c)return nullptr;
        auto key=std::make_tuple(c,std::string(name),argc);
        if(auto i=methods.find(key);i!=methods.end())return i->second;
        for(auto k=c;k;k=a.class_get_parent(k)){
            auto m=a.class_get_method_from_name(k,name,argc);
            if(m&&isExecutableMethod(m)){methods[key]=m;return m;}
            if(a.class_get_methods&&a.method_get_name&&a.method_get_param_count){
                void*iter=nullptr;
                while(Method cand=a.class_get_methods(k,&iter)){
                    const char*mn=a.method_get_name(cand);
                    if(mn&&std::strcmp(mn,name)==0&&int(a.method_get_param_count(cand))==argc&&isExecutableMethod(cand)){
                        methods[key]=cand;return cand;
                    }
                }
            }
        }
        return nullptr;
    }

    Method method(Klass c,const char*name,int argc){
        if(auto m=tryMethod(c,name,argc))return m;
        throw std::runtime_error(std::string("Method unavailable: ")+name);
    }

    Field tryField(Klass c,const char*name){
        if(!c)return nullptr;
        auto key=std::make_pair(c,std::string(name));
        if(auto i=fields.find(key);i!=fields.end())return i->second;
        for(auto k=c;k;k=a.class_get_parent(k)){
            auto f=a.class_get_field_from_name(k,name);
            if(f){fields[key]=f;return f;}
        }
        if(a.class_get_fields&&a.field_get_name){
            for(auto k=c;k;k=a.class_get_parent(k)){
                void*iter=nullptr;
                while(Field f=a.class_get_fields(k,&iter)){
                    const char*fn=a.field_get_name(f);
                    if(fn&&iequals(fn,name)){fields[key]=f;return f;}
                }
            }
        }
        return nullptr;
    }

    Field field(Klass c,const char*name){
        if(auto f=tryField(c,name))return f;
        throw std::runtime_error(std::string("Field unavailable: ")+name);
    }

    Field findFieldAny(Klass c,std::initializer_list<const char*>names){
        for(const char*n:names){
            if(auto f=tryField(c,n)){
                if(isInstanceField(f))return f;
            }
        }
        return nullptr;
    }

    bool inheritsFrom(Klass c,const char*targetName){
        for(auto k=c;k;k=a.class_get_parent(k)){
            const char*n=a.class_get_name(k);
            if(n&&std::strcmp(n,targetName)==0)return true;
        }
        return false;
    }

    std::string classLabel(Obj o){
        if(!isValidObjectPointer(o))return {};
        Klass k=a.object_get_class(o);
        if(!k)return {};
        const char*n=a.class_get_name(k);
        return n?std::string(n):std::string();
    }

    Obj invoke(Method m,Obj o,void**args=nullptr){
        if(!m)throw std::runtime_error("Method is not executable");
        Obj ex=nullptr;auto r=a.runtime_invoke(m,o,args,&ex);
        if(ex)throw std::runtime_error("Managed call raised an exception");
        return r;
    }

    Obj call(Obj o,const char*n,int argc=0,void**args=nullptr){
        if(!isValidObjectPointer(o))throw std::runtime_error("Waiting for object");
        return invoke(method(a.object_get_class(o),n,argc),o,args);
    }

    template<class T>T unbox(Obj o){
        if(!o)throw std::runtime_error("Missing boxed return value");
        auto p=a.object_unbox(o);if(!p)throw std::runtime_error("Cannot unbox result");
        T v{};std::memcpy(&v,p,sizeof(v));return v;
    }

    template<class T>T read(Obj o,Field f,int expected=0){
        if(!isValidObjectPointer(o)||!f)throw std::runtime_error("Null field owner");
        if(expected&&a.type_get_type(a.field_get_type(f))!=expected)throw std::runtime_error("Field type changed");
        T v{};a.field_get_value(o,f,&v);return v;
    }

    template<class T>bool writeField(Obj o,Field f,T value){
        if(!isValidObjectPointer(o)||!isInstanceField(f))return false;
        if(a.field_set_value){a.field_set_value(o,f,&value);return true;}
        if(a.field_get_offset){
            std::size_t off=a.field_get_offset(f);
            return safeWriteAt(o,off,value);
        }
        return false;
    }

    template<class T>bool readFieldNamed(Obj o,std::initializer_list<const char*>names,T&out){
        if(!isValidObjectPointer(o))return false;
        Klass k=a.object_get_class(o);
        if(Field f=findFieldAny(k,names)){
            a.field_get_value(o,f,&out);
            return true;
        }
        return false;
    }

    Obj readObjFieldNamed(Obj o,std::initializer_list<const char*>names){
        if(!isValidObjectPointer(o))return nullptr;
        Klass k=a.object_get_class(o);
        if(Field f=findFieldAny(k,names)){
            int t=a.type_get_type(a.field_get_type(f));
            if(t==0x12||t==0x15||t==0x1c||t==0x1d){
                Obj val=nullptr;
                a.field_get_value(o,f,&val);
                if(isValidObjectPointer(val))return val;
            }
        }
        return nullptr;
    }

    Obj ref(Obj o,const char*n){return read<Obj>(o,field(a.object_get_class(o),n));}

    Obj findRefByClass(Obj o,const char*targetClassName,std::initializer_list<const char*>names){
        if(!isValidObjectPointer(o))return nullptr;
        if(Obj named=readObjFieldNamed(o,names)){
            Klass nk=a.object_get_class(named);
            if(nk&&(classLabel(named)==targetClassName||inheritsFrom(nk,targetClassName)))return named;
        }
        if(a.class_get_fields){
            Klass k=a.object_get_class(o);
            for(auto cur=k;cur;cur=a.class_get_parent(cur)){
                void*iter=nullptr;
                while(Field f=a.class_get_fields(cur,&iter)){
                    if(!isInstanceField(f))continue;
                    int t=a.type_get_type(a.field_get_type(f));
                    if(t==0x12||t==0x1c){
                        Obj val=nullptr;a.field_get_value(o,f,&val);
                        if(isValidObjectPointer(val)){
                            Klass vk=a.object_get_class(val);
                            if(vk&&(classLabel(val)==targetClassName||inheritsFrom(vk,targetClassName)))return val;
                        }
                    }
                }
            }
        }
        return nullptr;
    }

    bool alive(Obj o){
        if(!isValidObjectPointer(o))return false;
        Klass k=a.object_get_class(o);
        if(!k)return false;
        if(inheritsFrom(k,"ActorControl")||inheritsFrom(k,"List`1")||inheritsFrom(k,"Dictionary`2"))return false;
#if defined(__APPLE__)
        if(a.hasNativeLayout){
            if(!inheritsFrom(k,"Object"))return false;
            void* cachedPtr=nullptr;
            if(!safeReadAt(o,0x10,cachedPtr)||!cachedPtr)return false;
        }
#endif
        void*args[]={o};
        try{
            return unbox<bool>(invoke(method(unity("Object"),"op_Implicit",1),nullptr,args));
        }catch(...){
            return false;
        }
    }

    std::string objectName(Obj o){
        if(!o||!alive(o))return {};
        Method m=tryMethod(unity("Object"),"get_name",0);
        if(!m)return {};
        Obj str=nullptr;
        try{str=invoke(m,o);}catch(...){return {};}
        if(!isValidObjectPointer(str))return {};
        std::int32_t len=0;
        if(!safeReadAt(str,0x10,len)||len<=0||len>256)return {};
        std::string out;out.reserve(std::size_t(len));
        for(int i=0;i<len;++i){
            std::uint16_t ch=0;
            if(!safeReadAt(str,0x14+std::size_t(i)*2,ch))break;
            if(ch<0x80)out.push_back(static_cast<char>(ch));
            else if(ch<0x800){
                out.push_back(static_cast<char>(0xC0|(ch>>6)));
                out.push_back(static_cast<char>(0x80|(ch&0x3F)));
            }else{
                out.push_back(static_cast<char>(0xE0|(ch>>12)));
                out.push_back(static_cast<char>(0x80|((ch>>6)&0x3F)));
                out.push_back(static_cast<char>(0x80|(ch&0x3F)));
            }
        }
        return out;
    }

    Obj componentGO(Obj comp){
        if(!comp||!alive(comp))return nullptr;
        Klass k=a.object_get_class(comp);
        if(inheritsFrom(k,"GameObject"))return comp;
        if(!inheritsFrom(k,"Component"))return nullptr;
        try{return call(comp,"get_gameObject");}catch(...){return nullptr;}
    }

    Obj getTransform(Obj goOrComp){
        if(!goOrComp||!alive(goOrComp))return nullptr;
        Klass k=a.object_get_class(goOrComp);
        if(inheritsFrom(k,"Transform"))return goOrComp;
        if(!inheritsFrom(k,"GameObject")&&!inheritsFrom(k,"Component"))return nullptr;
        try{return call(goOrComp,"get_transform");}catch(...){return nullptr;}
    }

    Obj getParent(Obj tr){
        if(!tr||!alive(tr))return nullptr;
        try{return call(tr,"get_parent");}catch(...){return nullptr;}
    }

    bool activeGO(Obj go){
        if(!go||!alive(go))return false;
        try{
            if(auto m=tryMethod(a.object_get_class(go),"get_activeInHierarchy",0))return unbox<bool>(invoke(m,go));
            if(auto m=tryMethod(a.object_get_class(go),"get_activeSelf",0))return unbox<bool>(invoke(m,go));
        }catch(...){}
        return false;
    }

    bool colliderEnabled(Obj col){
        if(!col||!alive(col))return false;
        try{return unbox<bool>(call(col,"get_enabled"));}catch(...){return false;}
    }

    bool colliderTrigger(Obj col){
        if(!col||!alive(col))return false;
        try{return unbox<bool>(call(col,"get_isTrigger"));}catch(...){return false;}
    }

    bool solidCollider(Obj col){
        if(!col||!alive(col))return false;
        Obj go=componentGO(col);
        return activeGO(go)&&colliderEnabled(col)&&!colliderTrigger(col);
    }

    Bounds boundsOfCollider(Obj col){
        if(!col||!alive(col))throw std::runtime_error("Collider unavailable");
        Bounds b=unbox<Bounds>(call(col,"get_bounds"));
        if(!validBounds(b))throw std::runtime_error("Invalid collider bounds");
        return b;
    }

    Vec3 worldPos(Obj tr){
        if(!tr||!alive(tr))throw std::runtime_error("Transform unavailable");
        Vec3 v=unbox<Vec3>(call(tr,"get_position"));
        if(!valid(v))throw std::runtime_error("Invalid transform position");
        return v;
    }

    std::vector<std::string> hierarchyNames(Obj tr){
        std::vector<std::string> names;
        std::set<Obj> seen;
        Obj cur=tr;
        for(int i=0;i<16&&cur&&alive(cur);++i){
            if(seen.contains(cur))break;
            seen.insert(cur);
            Obj go=componentGO(cur);
            names.push_back(objectName(go));
            cur=getParent(cur);
        }
        std::reverse(names.begin(),names.end());
        return names;
    }

    Obj topAncestorGO(Obj compOrGo){
        Obj tr=getTransform(compOrGo);
        if(!tr||!alive(tr))return nullptr;
        if(Method mRoot=tryMethod(unity("Transform"),"get_root",0)){
            try{
                Obj r=invoke(mRoot,tr);
                if(r&&alive(r))return componentGO(r);
            }catch(...){}
        }
        std::set<Obj> seen;
        for(int i=0;i<32;++i){
            if(seen.contains(tr))break;
            seen.insert(tr);
            Obj p=getParent(tr);
            if(!p||!alive(p))break;
            tr=p;
        }
        return componentGO(tr);
    }

    std::vector<Obj> readManagedList(Obj listObj,std::size_t maxItems=2048){
        std::vector<Obj> out;
        if(!isValidObjectPointer(listObj))return out;
        Obj itemsArr=nullptr;
        std::int32_t size=0;
        Klass lk=a.object_get_class(listObj);
        Field fItems=findFieldAny(lk,{"_items","items"});
        Field fSize=findFieldAny(lk,{"_size","size"});
        if(!fItems||!fSize)return out;
        if(a.type_get_type(a.field_get_type(fItems))!=0x1d)return out;
        if(a.type_get_type(a.field_get_type(fSize))!=0x08)return out;
        a.field_get_value(listObj,fItems,&itemsArr);
        a.field_get_value(listObj,fSize,&size);
        if(!isValidObjectPointer(itemsArr)||size<=0||std::size_t(size)>maxItems)return out;
        std::size_t cap=a.array_length(itemsArr);
        if(cap<std::size_t(size))return out;
        auto head=a.array_object_header_size();
        if(head<16||head>128)return out;
        out.reserve(std::size_t(size));
        for(std::int32_t i=0;i<size;++i){
            Obj item=nullptr;
            if(safeReadAt(itemsArr,head+std::size_t(i)*sizeof(Obj),item)&&isValidObjectPointer(item)){
                out.push_back(item);
            }
        }
        return out;
    }

    // 100% non-generic hierarchy traversal using verified dump.cs methods:
    // Transform.get_childCount() (RVA 0xA18D374), Transform.GetChild(int) (RVA 0xA18E660),
    // GameObject.GetComponentCount() (RVA 0xA16E02C), GameObject.QueryComponentAtIndex(int) (RVA 0xA16E118).
    // None of these 4 methods have generic overloads in UnityEngine.CoreModule.dll!
    std::vector<Obj> getComponentsInChildrenByName(Obj rootGo,const char* targetClassName){
        std::vector<Obj> out;
        if(!rootGo||!alive(rootGo)||!targetClassName)return out;
        Obj rootTr=getTransform(rootGo);
        if(!rootTr||!alive(rootTr))return out;

        Method mChildCount=tryMethod(unity("Transform"),"get_childCount",0);
        Method mGetChild=tryMethod(unity("Transform"),"GetChild",1);
        Method mCompCount=tryMethod(unity("GameObject"),"GetComponentCount",0);
        Method mQueryComp=tryMethod(unity("GameObject"),"QueryComponentAtIndex",1);
        if(!mChildCount||!mGetChild||!mCompCount||!mQueryComp)return out;

        std::vector<Obj> stack;
        stack.reserve(256);
        stack.push_back(rootTr);
        std::set<Obj> visited;
        while(!stack.empty()&&visited.size()<4096&&out.size()<2048){
            Obj tr=stack.back();
            stack.pop_back();
            if(!tr||!alive(tr)||visited.contains(tr))continue;
            visited.insert(tr);

            Obj go=componentGO(tr);
            if(go&&alive(go)){
                int cCount=0;
                try{cCount=unbox<int>(invoke(mCompCount,go));}catch(...){cCount=0;}
                for(int i=0;i<cCount&&i<64;++i){
                    void* args[]={&i};
                    try{
                        Obj comp=invoke(mQueryComp,go,args);
                        if(comp&&alive(comp)){
                            Klass ck=a.object_get_class(comp);
                            if(ck&&(classLabel(comp)==targetClassName||inheritsFrom(ck,targetClassName))){
                                out.push_back(comp);
                            }
                        }
                    }catch(...){}
                }
            }

            int chCount=0;
            try{chCount=unbox<int>(invoke(mChildCount,tr));}catch(...){chCount=0;}
            for(int i=chCount-1;i>=0&&i<512;--i){
                void* args[]={&i};
                try{
                    Obj childTr=invoke(mGetChild,tr,args);
                    if(childTr&&alive(childTr))stack.push_back(childTr);
                }catch(...){}
            }
        }
        return out;
    }

    Obj actorSystem(){Obj o=nullptr;a.field_static_get_value(field(game("ActorSystem"),"Self"),&o);return o;}
    Obj actor(){auto o=actorSystem();return isValidObjectPointer(o)?call(o,"get_MyActorCharacter"):nullptr;}

    Obj currentControl(Obj actorObj){
        if(!isValidObjectPointer(actorObj))return nullptr;
        auto control=ref(actorObj,"actorControl");if(!isValidObjectPointer(control))return nullptr;
        if(ref(control,"actorBase")!=actorObj)throw std::runtime_error("Local controller link changed");
        auto klass=a.object_get_class(control);bool supported=false;
        for(auto c=klass;c;c=a.class_get_parent(c)){
            const char*n=a.class_get_name(c);const char*ns=a.class_get_namespace(c);
            if(n&&ns&&!*ns&&std::strcmp(n,"ActorControl")==0){supported=true;break;}
        }
        if(!supported)throw std::runtime_error("Local controller is not an ActorControl");
        if(read<bool>(control,field(klass,"_terminate"),2))return nullptr;
        controllerName=a.class_get_name(klass);return control;
    }

    void resetFocus(){controlRoot.reset();actorRoot.reset();actorUID.clear();nextJump=0;lastJump=-1;}

    // Exact dump.cs field & getter names for ActorMiniGameControl / ActorKMGPlayer / ActorDefaultControlPlayer
    bool checkJumpBlocked(Obj control,Klass klass,std::string&reasonOut){
        const bool isMini=isMiniControlName(controllerName)||inheritsFrom(klass,"ActorMiniGameControl");
        if(isMini){
            std::uint8_t goal=0;
            if(!readFieldNamed<std::uint8_t>(control,{"<IsGoalIn>k__BackingField","_isGoalIn","isGoalIn"},goal)){
                if(Method mGoal=tryMethod(klass,"get_IsGoalIn",0)){
                    try{goal=unbox<bool>(invoke(mGoal,control))?1:0;}catch(...){}
                }
            }
            if(goal==1){reasonOut="Spam nhảy: đã về đích • dừng nhảy";return true;}

            std::uint8_t death=0;
            if(!readFieldNamed<std::uint8_t>(control,{"<IsDeathInGame>k__BackingField","_isDeathInGame","isDeathInGame"},death)){
                if(Method mDeath=tryMethod(klass,"get_IsDeathInGame",0)){
                    try{death=unbox<bool>(invoke(mDeath,control))?1:0;}catch(...){}
                }
            }
            if(controllerName=="ActorKMGPlayer"){
                std::uint8_t die=0,watch=0;
                std::int32_t kmgState=0;
                readFieldNamed<std::uint8_t>(control,{"isDie","_isDie"},die);
                readFieldNamed<std::uint8_t>(control,{"<IsUserWatchMode>k__BackingField","isWatchMode","_isWatchMode"},watch);
                readFieldNamed<std::int32_t>(control,{"<CurrentCharacterState>k__BackingField","_state","state"},kmgState);
                if(kmgState>=51&&kmgState<=54){reasonOut="Spam nhảy: đã về đích • dừng nhảy";return true;}
                if(watch==1){reasonOut="Spam nhảy: đang chế độ xem";return true;}
                if(die==1||(kmgState>=8&&kmgState<=13)||kmgState==50){
                    reasonOut="Spam nhảy: chờ nhân vật hồi sinh";return true;
                }
            }else if(death==1){
                reasonOut="Spam nhảy: chờ nhân vật hồi sinh";return true;
            }
        }
        return false;
    }

    bool updateJoyStickState(Obj actorObj,double time,ImVec2&jumpBtnPosOut,std::string&blockedReasonOut){
        const bool allowSlow=(joyStickScanTime<0||(time-joyStickScanTime)>=0.50);
        if(allowSlow)joyStickScanTime=time;
        Obj dlg=findDialogByClass("DialogJoyStick",joyStickDialogRoot,allowSlow);
        if(!dlg||!alive(dlg))return true;

        if(a.hasNativeLayout){
            Obj ownerActor=nullptr;
            if(safeReadAt(dlg,0x1F0,ownerActor)&&ownerActor&&ownerActor!=actorObj){
                return true;
            }
            std::uint8_t inVehicle=0,inSheetMusic=0;
            Obj actionLink=nullptr;
            safeReadAt(dlg,0x1F8,inVehicle);
            safeReadAt(dlg,0x209,inSheetMusic);
            safeReadAt(dlg,0x218,actionLink);
            if(inVehicle==1||inSheetMusic==1||isValidObjectPointer(actionLink)){
                blockedReasonOut="Spam nhảy: tạm dừng khi lái xe / tương tác";
                return false;
            }
        }

        // Resolve Jump button Transform inside DialogJoyStick once per dialog instance
        Obj jtr=jumpBtnTrRoot.get();
        if(!jtr||!alive(jtr)){
            jumpBtnTrRoot.reset();
            if(allowSlow){
                Obj dlgGo=componentGO(dlg);
                Obj dlgTr=getTransform(dlgGo);
                Method mChildCount=tryMethod(unity("Transform"),"get_childCount",0);
                Method mGetChild=tryMethod(unity("Transform"),"GetChild",1);
                if(dlgTr&&mChildCount&&mGetChild){
                    std::vector<Obj> stack={dlgTr};
                    std::set<Obj> seen;
                    while(!stack.empty()&&seen.size()<192){
                        Obj tr=stack.back();stack.pop_back();
                        if(!tr||!alive(tr)||seen.contains(tr))continue;
                        seen.insert(tr);
                        Obj go=componentGO(tr);
                        if(!go||!alive(go)||!activeGO(go))continue;
                        std::string nm=objectName(go);
                        if(nm.find("Jump")!=std::string::npos||nm.find("jump")!=std::string::npos){
                            jumpBtnTrRoot.set(a,tr);
                            jtr=tr;
                            break;
                        }
                        int chCount=0;
                        try{chCount=unbox<int>(invoke(mChildCount,tr));}catch(...){chCount=0;}
                        for(int i=chCount-1;i>=0&&i<64;--i){
                            void*args[]={&i};
                            try{
                                Obj ch=invoke(mGetChild,tr,args);
                                if(ch&&alive(ch))stack.push_back(ch);
                            }catch(...){}
                        }
                    }
                }
            }
        }

        if(jtr&&alive(jtr)&&activeGO(componentGO(jtr))){
            try{
                Vec3 wp=worldPos(jtr);
                if(Klass scrKlass=tryKlass("UnityEngine.CoreModule.dll","UnityEngine","Screen")){
                    Method mW=tryMethod(scrKlass,"get_width",0);
                    Method mH=tryMethod(scrKlass,"get_height",0);
                    if(mW&&mH){
                        int sw=unbox<int>(invoke(mW,nullptr));
                        int sh=unbox<int>(invoke(mH,nullptr));
                        if(sw>200&&sh>200&&wp.x>=float(sw)*0.68f&&wp.x<=float(sw)*0.98f&&wp.y>=float(sh)*0.02f&&wp.y<=float(sh)*0.42f){
                            jumpBtnPosOut={wp.x/float(sw),1.f-(wp.y/float(sh))};
                        }
                    }
                }
            }catch(...){}
        }
        return true;
    }

    void jumpTick(Obj actorObj,double time,double intervalMs,std::string&jumpNoteOut,bool jumpPressed,ImVec2&jumpBtnPosOut){
        if(!std::isfinite(intervalMs)||intervalMs<0||intervalMs>1000)throw std::runtime_error("Invalid spam interval");
        auto control=currentControl(actorObj);
        if(!control){controlRoot.reset();nextJump=0;lastJump=-1;return;}
        if(control!=controlRoot.get()){controlRoot.set(a,control);nextJump=0;lastJump=-1;}
        auto klass=a.object_get_class(control);
        std::string blockedReason;
        if(checkJumpBlocked(control,klass,blockedReason)||!updateJoyStickState(actorObj,time,jumpBtnPosOut,blockedReason)){
            jumpNoteOut=blockedReason;
            return;
        }
        if(!jumpPressed){
            nextJump=0;
            jumpNoteOut="Spam nhảy: BẬT • Đè nút Nhảy để spam";
            return;
        }
        if(time<nextJump||time==lastJump){
            jumpNoteOut="Spam nhảy: ĐANG NHẢY • Giữ nút để tiếp tục";
            return;
        }

        // Exact dump.cs field names:
        // ActorDefaultControlPlayer: `_isCamJump` (0x4E1)
        // ActorMiniGameControl: `<IsCanJump>k__BackingField` (0x2A9)
        Field canJumpField=findFieldAny(klass,{"_canJump","_isCamJump","<IsCanJump>k__BackingField","canJump","_isCanJump","isCanJump"});
        std::uint8_t origCanJump=1;
        bool liftedField=false;

        if(canJumpField){
            a.field_get_value(control,canJumpField,&origCanJump);
            if(origCanJump==0){
                std::uint8_t one=1;
                liftedField=writeField(control,canJumpField,one);
            }
        }

        struct RestoreGuard {
            State* s;Obj ctrl;Field f;std::uint8_t orig;bool byField;
            ~RestoreGuard(){
                if(byField)s->writeField(ctrl,f,orig);
            }
        } restoreGuard{this,control,canJumpField,origCanJump,liftedField};

        call(control,"OnJump");
        ++jumpRequests;lastJump=time;nextJump=time+intervalMs/1000.;
        jumpNoteOut="Spam nhảy: ĐANG NHẢY • Giữ nút để tiếp tục";
    }

    Obj findActiveComponentInChildren(Obj rootGo,const char*targetClassName){
        if(!rootGo||!alive(rootGo)||!activeGO(rootGo)||!targetClassName)return nullptr;
        Obj rootTr=getTransform(rootGo);
        if(!rootTr||!alive(rootTr))return nullptr;
        Method mChildCount=tryMethod(unity("Transform"),"get_childCount",0);
        Method mGetChild=tryMethod(unity("Transform"),"GetChild",1);
        Method mCompCount=tryMethod(unity("GameObject"),"GetComponentCount",0);
        Method mQueryComp=tryMethod(unity("GameObject"),"QueryComponentAtIndex",1);
        if(!mChildCount||!mGetChild||!mCompCount||!mQueryComp)return nullptr;

        std::vector<Obj> stack;
        stack.reserve(128);
        stack.push_back(rootTr);
        std::set<Obj> visited;
        while(!stack.empty()&&visited.size()<512){
            Obj tr=stack.back();
            stack.pop_back();
            if(!tr||!alive(tr)||visited.contains(tr))continue;
            visited.insert(tr);
            Obj go=componentGO(tr);
            if(!go||!alive(go)||!activeGO(go))continue;

            int cCount=0;
            try{cCount=unbox<int>(invoke(mCompCount,go));}catch(...){cCount=0;}
            for(int i=0;i<cCount&&i<32;++i){
                void*args[]={&i};
                try{
                    Obj comp=invoke(mQueryComp,go,args);
                    if(comp&&alive(comp)&&classLabel(comp)==targetClassName)return comp;
                }catch(...){}
            }

            int chCount=0;
            try{chCount=unbox<int>(invoke(mChildCount,tr));}catch(...){chCount=0;}
            for(int i=chCount-1;i>=0&&i<256;--i){
                void*args[]={&i};
                try{
                    Obj childTr=invoke(mGetChild,tr,args);
                    if(childTr&&alive(childTr))stack.push_back(childTr);
                }catch(...){}
            }
        }
        return nullptr;
    }

    Obj findUISystem(){
        Obj cached=uiSystemRoot.get();
        if(cached&&isValidObjectPointer(cached))return cached;
        uiSystemRoot.reset();
        for(const char*sysName:{"UISystem","DialogSystem","UIManager"}){
            if(Klass uk=tryGame(sysName)){
                for(const char*fn:{"Self","Instance","_instance"}){
                    if(Field sf=tryField(uk,fn)){
                        Obj val=nullptr;
                        a.field_static_get_value(sf,&val);
                        if(val&&isValidObjectPointer(val)&&classLabel(val)==sysName){
                            uiSystemRoot.set(a,val);
                            return val;
                        }
                    }
                }
            }
        }
        if(Klass sysHelper=tryGame("SystemHelper")){
            for(const char*getter:{"get_UI","get_Dialog"}){
                if(Method m=tryMethod(sysHelper,getter,0)){
                    try{
                        Obj val=invoke(m,nullptr);
                        if(val&&isValidObjectPointer(val)){
                            uiSystemRoot.set(a,val);
                            return val;
                        }
                    }catch(...){}
                }
            }
        }
        return nullptr;
    }

    Obj findDialogByClass(const char*targetClassName,Root&cacheRoot,bool allowSlowScan){
        Obj cached=cacheRoot.get();
        if(cached&&alive(cached)&&classLabel(cached)==targetClassName)return cached;
        cacheRoot.reset();

        Klass k=tryGame(targetClassName);
        if(!k)return nullptr;

        // 1. Check static fields (SingleTonDialog<T>.Self / Instance / static_fields +0xB8)
        for(Klass cur=k;cur;cur=a.class_get_parent(cur)){
            for(const char*sname:{"Self","Instance","_instance","s_Instance"}){
                if(Field sf=a.class_get_field_from_name(cur,sname)){
                    Obj val=nullptr;
                    a.field_static_get_value(sf,&val);
                    if(val&&alive(val)&&classLabel(val)==targetClassName){
                        cacheRoot.set(a,val);
                        return val;
                    }
                }
            }
            if(a.class_get_fields&&a.field_get_flags){
                void*iter=nullptr;
                while(Field f=a.class_get_fields(cur,&iter)){
                    if((a.field_get_flags(f)&0x0010)!=0){
                        int t=a.type_get_type(a.field_get_type(f));
                        if(t==0x12||t==0x15||t==0x1c){
                            Obj val=nullptr;
                            a.field_static_get_value(f,&val);
                            if(val&&alive(val)&&classLabel(val)==targetClassName){
                                cacheRoot.set(a,val);
                                return val;
                            }
                        }
                    }
                }
            }
            if(a.hasNativeLayout){
                void*staticFields=nullptr;
                if(safeReadAt(cur,0xB8,staticFields)&&staticFields){
                    Obj cand=nullptr;
                    if(safeReadAt(staticFields,0,cand)&&cand&&alive(cand)&&classLabel(cand)==targetClassName){
                        cacheRoot.set(a,cand);
                        return cand;
                    }
                }
            }
        }

        if(!allowSlowScan)return nullptr;

        // 2. UnityEngine.Object.FindObjectOfType(System.Type) when available
        if(a.class_get_type&&a.type_get_object){
            const void*ilType=a.class_get_type(k);
            Obj sysType=ilType?a.type_get_object(ilType):nullptr;
            if(sysType&&isValidObjectPointer(sysType)){
                if(Method mFind=tryMethod(unity("Object"),"FindObjectOfType",1)){
                    void*args[]={sysType};
                    try{
                        Obj found=invoke(mFind,nullptr,args);
                        if(found&&alive(found)&&classLabel(found)==targetClassName){
                            cacheRoot.set(a,found);
                            return found;
                        }
                    }catch(...){}
                }
            }
        }

        // 3. Check UISystem fields, lists, dictionaries, and active UI hierarchy
        if(Obj uiSys=findUISystem()){
            if(Obj direct=findRefByClass(uiSys,targetClassName,{"_minigameSimpleInfo","minigameSimpleInfo","_groupPlay","groupPlay","_currentDialog","currentDialog"})){
                if(alive(direct)){cacheRoot.set(a,direct);return direct;}
            }
            if(a.class_get_fields){
                Klass uk=a.object_get_class(uiSys);
                std::vector<Obj> uiRoots;
                if(Obj ugo=componentGO(uiSys))uiRoots.push_back(ugo);
                for(Klass cur=uk;cur;cur=a.class_get_parent(cur)){
                    void*iter=nullptr;
                    while(Field f=a.class_get_fields(cur,&iter)){
                        if(!isInstanceField(f))continue;
                        int t=a.type_get_type(a.field_get_type(f));
                        if(t!=0x12&&t!=0x15&&t!=0x1c)continue;
                        Obj val=nullptr;
                        a.field_get_value(uiSys,f,&val);
                        if(!isValidObjectPointer(val))continue;
                        Klass vk=a.object_get_class(val);
                        if(!vk)continue;
                        if(classLabel(val)==targetClassName&&alive(val)){
                            cacheRoot.set(a,val);
                            return val;
                        }
                        if(inheritsFrom(vk,"List`1")){
                            for(Obj item:readManagedList(val,256)){
                                if(item&&alive(item)&&classLabel(item)==targetClassName){
                                    cacheRoot.set(a,item);
                                    return item;
                                }
                            }
                        }else if(a.hasNativeLayout&&inheritsFrom(vk,"Dictionary`2")){
                            Obj entries=nullptr;std::int32_t count=0;
                            if(safeReadAt(val,0x18,entries)&&safeReadAt(val,0x20,count)&&isValidObjectPointer(entries)&&count>0&&count<=256){
                                auto head=a.array_object_header_size();
                                for(std::int32_t i=0;i<count;++i){
                                    std::int32_t hash=0;
                                    if(!safeReadAt(entries,head+std::size_t(i)*24,hash)||hash<0)continue;
                                    Obj entryVal=nullptr;
                                    if(safeReadAt(entries,head+std::size_t(i)*24+16,entryVal)&&entryVal&&alive(entryVal)&&classLabel(entryVal)==targetClassName){
                                        cacheRoot.set(a,entryVal);
                                        return entryVal;
                                    }
                                }
                            }
                        }else if(inheritsFrom(vk,"Component")||inheritsFrom(vk,"GameObject")){
                            if(Obj rgo=componentGO(val)){
                                if(activeGO(rgo)&&uiRoots.size()<16)uiRoots.push_back(rgo);
                            }
                        }
                    }
                }
                for(Obj rgo:uiRoots){
                    if(Obj found=findActiveComponentInChildren(rgo,targetClassName)){
                        cacheRoot.set(a,found);
                        return found;
                    }
                }
            }
        }
        return nullptr;
    }

    bool invokeReadyAction(Obj dialog,Obj button,Obj handler){
        // 1. Try UnityEngine.UI.Button.onClick.Invoke() on the Ready button (+0x138 / +0x188)
        if(button&&alive(button)){
            Obj btnComp=nullptr;
            Klass bk=a.object_get_class(button);
            if(bk&&inheritsFrom(bk,"Button")){
                btnComp=button;
            }else if(bk&&inheritsFrom(bk,"GameObject")){
                if(Method mCompCount=tryMethod(unity("GameObject"),"GetComponentCount",0)){
                    if(Method mQueryComp=tryMethod(unity("GameObject"),"QueryComponentAtIndex",1)){
                        int cCount=0;
                        try{cCount=unbox<int>(invoke(mCompCount,button));}catch(...){cCount=0;}
                        for(int i=0;i<cCount&&i<16;++i){
                            void*args[]={&i};
                            try{
                                Obj c=invoke(mQueryComp,button,args);
                                if(c&&alive(c)&&inheritsFrom(a.object_get_class(c),"Button")){btnComp=c;break;}
                            }catch(...){}
                        }
                    }
                }
            }
            if(btnComp&&alive(btnComp)){
                Obj onClickEvt=readObjFieldNamed(btnComp,{"m_OnClick","onClick"});
                if(!onClickEvt){
                    if(Method mGetClick=tryMethod(a.object_get_class(btnComp),"get_onClick",0)){
                        try{onClickEvt=invoke(mGetClick,btnComp);}catch(...){onClickEvt=nullptr;}
                    }
                }
                if(onClickEvt&&isValidObjectPointer(onClickEvt)){
                    if(Method mInv=tryMethod(a.object_get_class(onClickEvt),"Invoke",0)){
                        try{invoke(mInv,onClickEvt);++readyRequests;return true;}catch(...){}
                    }
                }
            }
        }

        // 2. Try direct 0-arg Ready method on dialog (OnClickReady / OnClickStart / OnClickPlay / etc.)
        if(dialog&&alive(dialog)){
            Klass dk=a.object_get_class(dialog);
            for(const char*mname:{"OnClickReady","OnClickStart","OnClickPlay","OnClickMatch","OnClickJoin","OnClickEnter","OnClickOk","OnClickYes","OnReady","ClickReady","Ready"}){
                Method m=a.class_get_method_from_name(dk,mname,0);
                if(m&&isExecutableMethod(m)){
                    try{invoke(m,dialog);++readyRequests;return true;}catch(...){}
                }
            }
        }

        // 3. Try invoking the attached handler delegate (+0x170 on DialogMinigameSimpleInfo)
        if(handler&&isValidObjectPointer(handler)){
            if(Method mInv=tryMethod(a.object_get_class(handler),"Invoke",0)){
                try{invoke(mInv,handler);++readyRequests;return true;}catch(...){}
            }
        }
        return false;
    }

    bool simpleReadyTick(bool allowSlowScan){
        Obj dialog=findDialogByClass("DialogMinigameSimpleInfo",simpleDialogRoot,allowSlowScan);
        if(!dialog||!alive(dialog)){simpleLatched=false;return false;}
        if(!activeGO(componentGO(dialog))){simpleLatched=false;return false;}

        Obj button=readObjFieldNamed(dialog,{"_readyButton","readyButton","btnReady","_btnReady","m_btnReady","playButton","_playButton"});
        if(!button&&a.hasNativeLayout){
            Obj cand=nullptr;
            if(safeReadAt(dialog,0x138,cand)&&isValidObjectPointer(cand))button=cand;
        }
        if(!button||!alive(button)||!activeGO(componentGO(button))){
            simpleLatched=false;
            return false;
        }

        std::uint8_t isReady=0,isLocked=0,usesTickets=0;
        readFieldNamed<std::uint8_t>(dialog,{"_isReady","isReady","<IsReady>k__BackingField"},isReady);
        readFieldNamed<std::uint8_t>(dialog,{"_isLock","isLock","_isWait","isWait"},isLocked);
        readFieldNamed<std::uint8_t>(dialog,{"_isUseTicket","isUseTicket","usesTickets"},usesTickets);
        if(a.hasNativeLayout){
            safeReadAt(dialog,0x163,isReady);
            safeReadAt(dialog,0x162,isLocked);
            safeReadAt(dialog,0x16C,usesTickets);
        }

        if(isReady!=0){
            readySummary="Tự sẵn sàng: Game báo đã sẵn sàng";
            return true;
        }
        if(isLocked!=0){
            readySummary="Tự sẵn sàng: Chờ game mở khóa nút sẵn sàng";
            return true;
        }

        Obj handler=readObjFieldNamed(dialog,{"_onClickReady","onClickReady","_readyAction","readyAction","_callback","callback"});
        if(!handler&&a.hasNativeLayout){
            Obj cand=nullptr;
            if(safeReadAt(dialog,0x170,cand)&&isValidObjectPointer(cand))handler=cand;
        }
        if(a.hasNativeLayout&&!isValidObjectPointer(handler)){
            readySummary="Tự sẵn sàng: Chờ game gắn xử lý nút sẵn sàng";
            return true;
        }

        if(!simpleLatched){
            if(invokeReadyAction(dialog,button,handler)){
                simpleLatched=true;
                readySummary=usesTickets?"Tự sẵn sàng: Đã bấm Sẵn sàng dùng vé":"Tự sẵn sàng: Đã bấm Sẵn sàng";
            }else{
                readySummary="Tự sẵn sàng: Chờ xử lý nút sẵn sàng";
            }
        }
        return true;
    }

    bool partyReadyTick(bool allowSlowScan){
        Obj dialog=findDialogByClass("DialogGroupPlay",partyDialogRoot,allowSlowScan);
        if(!dialog||!alive(dialog)){partyLatched=false;return false;}
        if(!activeGO(componentGO(dialog))){partyLatched=false;return false;}

        Obj button=readObjFieldNamed(dialog,{"_readyButton","readyButton","btnReady","_btnReady","m_btnReady","playButton","_playButton"});
        if(!button&&a.hasNativeLayout){
            Obj cand=nullptr;
            if(safeReadAt(dialog,0x188,cand)&&isValidObjectPointer(cand))button=cand;
        }
        std::int32_t partyState=0;
        if(!readFieldNamed<std::int32_t>(dialog,{"_state","state","_readyState","readyState"},partyState)&&a.hasNativeLayout){
            safeReadAt(dialog,0x1D4,partyState);
        }
        const bool eligible=(partyState==1)&&button&&alive(button)&&activeGO(componentGO(button));
        if(!eligible){
            partyLatched=false;
            if(partyState>1){
                readySummary="Tự sẵn sàng: Đã sẵn sàng Tiệc (CityParty)";
                return true;
            }
            return false;
        }
        if(!partyLatched){
            std::uint8_t usesTickets=0;
            if(!readFieldNamed<std::uint8_t>(dialog,{"_isUseTicket","isUseTicket","usesTickets"},usesTickets)&&a.hasNativeLayout){
                safeReadAt(dialog,0x1D8,usesTickets);
            }
            if(invokeReadyAction(dialog,button,nullptr)){
                partyLatched=true;
                readySummary=usesTickets?"Tự sẵn sàng: Đã bấm Sẵn sàng CityParty dùng vé":"Tự sẵn sàng: Đã bấm Sẵn sàng CityParty";
            }
        }
        return true;
    }

    void readyTick(double time,std::string&readyNoteOut){
        const bool allowSlowScan=(readyScanTime<0||(time-readyScanTime)>=0.30);
        if(allowSlowScan)readyScanTime=time;
        if(simpleReadyTick(allowSlowScan)){
            readyNoteOut=readySummary;
            return;
        }
        if(partyReadyTick(allowSlowScan)){
            readyNoteOut=readySummary;
            return;
        }
        readySummary="Tự sẵn sàng: BẬT • Chờ bảng Sẵn sàng";
        readyNoteOut=readySummary;
    }

    void readDuelState(Obj gameObj,std::uint8_t&isAttackOut,std::uint8_t&canAttackOut,std::uint32_t&roundOut,std::uint8_t&endedOut,std::int32_t&scoreOut){
        isAttackOut=0;canAttackOut=0;roundOut=0;endedOut=0;scoreOut=0;
        if(!gameObj||!isValidObjectPointer(gameObj))return;
        Klass gk=a.object_get_class(gameObj);
        if(!readFieldNamed<std::uint8_t>(gameObj,{"<IsStateAttack>k__BackingField","_isStateAttack","isStateAttack","IsStateAttack"},isAttackOut)){
            if(Method m=tryMethod(gk,"get_IsStateAttack",0)){
                try{isAttackOut=unbox<bool>(invoke(m,gameObj))?1:0;}catch(...){}
            }else if(a.hasNativeLayout){
                safeReadAt(gameObj,0x1D8,isAttackOut);
            }
        }
        if(!readFieldNamed<std::uint8_t>(gameObj,{"<IsCanAttack>k__BackingField","_isCanAttack","isCanAttack","IsCanAttack"},canAttackOut)){
            if(Method m=tryMethod(gk,"get_IsCanAttack",0)){
                try{canAttackOut=unbox<bool>(invoke(m,gameObj))?1:0;}catch(...){}
            }else if(a.hasNativeLayout){
                safeReadAt(gameObj,0x1D9,canAttackOut);
            }
        }
        if(!readFieldNamed<std::uint32_t>(gameObj,{"<CurrentRound>k__BackingField","_curRound","curRound","_currentRound","currentRound","_round","round"},roundOut)){
            if(Method m=tryMethod(gk,"get_CurrentRound",0)){
                try{roundOut=unbox<std::uint32_t>(invoke(m,gameObj));}catch(...){}
            }else if(a.hasNativeLayout){
                safeReadAt(gameObj,0x1DC,roundOut);
            }
        }
        if(!readFieldNamed<std::uint8_t>(gameObj,{"<IsEndGame>k__BackingField","_isEndGame","isEndGame","_isEnd","isEnd"},endedOut)){
            if(Method m=tryMethod(gk,"get_IsEndGame",0)){
                try{endedOut=unbox<bool>(invoke(m,gameObj))?1:0;}catch(...){}
            }else if(a.hasNativeLayout){
                safeReadAt(gameObj,0x214,endedOut);
            }
        }
        if(!readFieldNamed<std::int32_t>(gameObj,{"<Score>k__BackingField","_score","score"},scoreOut)&&a.hasNativeLayout){
            safeReadAt(gameObj,0x218,scoreOut);
        }
    }

    void hammerTick(Obj actorObj,double time,double delayMs,std::string&hammerNoteOut){
        if(!std::isfinite(delayMs)||delayMs<0.||delayMs>10000.)throw std::runtime_error("Invalid hammer delay");
        Obj gameObj=findMiniGame(actorObj);
        if(!gameObj||!alive(gameObj)){
            hammerWindowKey.clear();hammerFightTime=-1;hammerLatched=false;
            hammerSummary="Tự động búa: BẬT • Chờ vào map Búa (120019)";
            hammerNoteOut=hammerSummary;
            return;
        }
        std::string gLabel=classLabel(gameObj);
        int gId=getMiniGameId(gameObj);
        if(gLabel!="MiniGameKMGDuel"&&gId!=120019){
            hammerWindowKey.clear();hammerFightTime=-1;hammerLatched=false;
            hammerSummary="Tự động búa: BẬT • Chờ map Búa (120019)";
            hammerNoteOut=hammerSummary;
            return;
        }

        std::uint8_t isAttack=0,canAttack=0,ended=0;
        std::uint32_t round=0;
        std::int32_t score=0;
        readDuelState(gameObj,isAttack,canAttack,round,ended,score);
        if(round>100)round=0;

        std::string wKey=std::to_string(reinterpret_cast<std::uintptr_t>(gameObj))+":"+std::to_string(round);
        if(wKey!=hammerWindowKey){
            hammerWindowKey=wKey;
            hammerFightTime=-1;
            hammerLatched=false;
        }
        if(ended==1){
            hammerFightTime=-1;
            hammerLatched=false;
            hammerSummary="Tự động búa: Ván đấu đã kết thúc";
            hammerNoteOut=hammerSummary;
            return;
        }
        if(isAttack!=1||canAttack!=1){
            hammerFightTime=-1;
            hammerLatched=false;
            hammerSummary="Tự động búa: Chờ chữ FIGHT (round "+std::to_string(round)+")";
            hammerNoteOut=hammerSummary;
            return;
        }
        if(hammerLatched){
            hammerSummary="Tự động búa: Đã đập búa #"+std::to_string(hammerRequests)+" (round "+std::to_string(round)+") • Chờ lượt tiếp";
            hammerNoteOut=hammerSummary;
            return;
        }

        if(hammerFightTime<0.)hammerFightTime=time;
        const double dueTime=hammerFightTime+(delayMs/1000.);
        if(time+1e-6<dueTime){
            int remMs=int(std::ceil((dueTime-time)*1000.));
            hammerSummary="Tự động búa: Chờ delay "+std::to_string(remMs)+"ms (round "+std::to_string(round)+")";
            hammerNoteOut=hammerSummary;
            return;
        }

        Obj control=currentControl(actorObj);
        if(!control&&a.hasNativeLayout){
            Obj cand=nullptr;
            if(safeReadAt(gameObj,0x238,cand)&&isValidObjectPointer(cand))control=cand;
        }
        if(!isValidObjectPointer(control)){
            hammerSummary="Tự động búa: Chờ ActorDuelPlayer";
            hammerNoteOut=hammerSummary;
            return;
        }

        std::uint8_t isStun=0;
        Klass ck=a.object_get_class(control);
        if(!readFieldNamed<std::uint8_t>(control,{"<IsStun>k__BackingField","_isStun","isStun","<IsFaint>k__BackingField","_isFaint","isFaint"},isStun)){
            if(Method mStun=tryMethod(ck,"get_IsStun",0)){
                try{isStun=unbox<bool>(invoke(mStun,control))?1:0;}catch(...){}
            }else if(a.hasNativeLayout){
                safeReadAt(control,0x3C0,isStun);
            }
        }
        if(isStun==1){
            hammerSummary="Tự động búa: Nhân vật đang choáng";
            hammerNoteOut=hammerSummary;
            return;
        }

        Method mAct=tryMethod(ck,"OnAction",0);
        if(!mAct)mAct=tryMethod(ck,"OnAttack",0);
        if(!mAct)throw std::runtime_error("Method unavailable: OnAction");

        // Latch before invoking native OnAction so one FIGHT window sends exactly one strike
        hammerLatched=true;
        invoke(mAct,control);
        ++hammerRequests;
        int elapsedMs=int(std::max(0.,std::round((time-hammerFightTime)*1000.)));
        hammerSummary="Tự động búa: ĐÃ ĐẬP BÚA #"+std::to_string(hammerRequests)+" (sau "+std::to_string(elapsedMs)+"ms • đặt "+std::to_string(int(delayMs))+"ms • round "+std::to_string(round)+")";
        hammerNoteOut=hammerSummary;
    }

    Vec3 position(Obj actorObj){
        auto tr=ref(actorObj,"baseTransfrom");
        if(!alive(tr))throw std::runtime_error("Transform unavailable");
        return worldPos(tr);
    }

    // Exact dump.cs lookup:
    // 1. `PlayTogether.MiniGame.MiniGameKMGUnit.Self` (static field at 0x0)
    // 2. Fallback via `SystemHelper.get_Map()` -> `m_CurrentMap` -> `currentMiniGameUnit` / `miniGameUnit`
    Obj findMiniGame(Obj actorObj){
        Obj ctrl=currentControl(actorObj);
        if(!ctrl)return nullptr;
        Klass ctrlKlass=a.object_get_class(ctrl);
        const bool inMiniGame=isMiniControlName(controllerName)||inheritsFrom(ctrlKlass,"ActorMiniGameControl");
        if(!inMiniGame){
            miniGameRoot.reset();
            return nullptr;
        }

        Obj cached=miniGameRoot.get();
        if(cached&&alive(cached))return cached;
        miniGameRoot.reset();

        if(Klass kmgUnitKlass=tryGame("MiniGameKMGUnit")){
            if(Field selfField=tryField(kmgUnitKlass,"Self")){
                Obj selfObj=nullptr;
                a.field_static_get_value(selfField,&selfObj);
                if(selfObj&&alive(selfObj)){
                    miniGameRoot.set(a,selfObj);
                    return selfObj;
                }
            }
        }

        if(Klass sysHelper=tryGame("SystemHelper")){
            if(Method mGetMap=tryMethod(sysHelper,"get_Map",0)){
                try{
                    Obj mapSys=invoke(mGetMap,nullptr);
                    if(mapSys&&alive(mapSys)){
                        Obj curMap=readObjFieldNamed(mapSys,{"m_CurrentMap","_currentMap","currentMap"});
                        if(curMap&&alive(curMap)){
                            Obj mg=readObjFieldNamed(curMap,{"miniGameUnit","currentMiniGameUnit","_miniGameUnit"});
                            if(mg&&alive(mg)){
                                miniGameRoot.set(a,mg);
                                return mg;
                            }
                        }
                    }
                }catch(...){}
            }
        }
        return nullptr;
    }

    // Exact dump.cs field & getter on MiniGameKMGUnit:
    // `private uint <KMGTableID>k__BackingField; // 0x110`
    // `public uint get_KMGTableID() // RVA 0x36A25DC`
    int getMiniGameId(Obj gameObj){
        if(!isValidObjectPointer(gameObj))return 0;
        std::uint32_t id=0;
        if(readFieldNamed<std::uint32_t>(gameObj,{"<KMGTableID>k__BackingField","_miniGameId","miniGameId"},id)&&id>=100000&&id<=999999){
            return int(id);
        }
        if(Method m=tryMethod(a.object_get_class(gameObj),"get_KMGTableID",0)){
            try{
                id=unbox<std::uint32_t>(invoke(m,gameObj));
                if(id>=100000&&id<=999999)return int(id);
            }catch(...){}
        }
        return int(id);
    }

    // Exact dump.cs field & getter on MiniGameUnit:
    // `protected GameBattleState currentBattleState; // 0x6C` (sbyte)
    // `public GameBattleState get_CurrentBattleState() // RVA 0x36AA534`
    int getBattleState(Obj gameObj){
        std::int8_t st=5;
        if(readFieldNamed<std::int8_t>(gameObj,{"currentBattleState","_battleState","battleState"},st)){
            if(st>=0&&st<=15)return int(st);
        }
        if(Method m=tryMethod(a.object_get_class(gameObj),"get_CurrentBattleState",0)){
            try{
                st=unbox<std::int8_t>(invoke(m,gameObj));
                if(st>=0&&st<=15)return int(st);
            }catch(...){}
        }
        return 5;
    }

    // Exact dump.cs field on MiniGameKMGUnit:
    // `public HideObjectManager hideObjectManager; // 0x140`
    Obj getHideObjectManager(Obj gameObj){
        if(Obj m=readObjFieldNamed(gameObj,{"hideObjectManager","_hideObjectManager","m_HideObjectManager"})){
            if(alive(m))return m;
        }
        return nullptr;
    }

    // Exact dump.cs fields on HideStampObjectData:
    // `public MeshRenderer ObjectRenderer; // 0x30`
    // `private Collider <ObjectCollider>k__BackingField; // 0x40` (`get_ObjectCollider()`)
    Obj getStampCollider(Obj stampObj){
        if(Obj col=readObjFieldNamed(stampObj,{"<ObjectCollider>k__BackingField","_collider","collider"})){
            if(alive(col))return col;
        }
        if(Method m=tryMethod(a.object_get_class(stampObj),"get_ObjectCollider",0)){
            try{
                Obj col=invoke(m,stampObj);
                if(col&&alive(col))return col;
            }catch(...){}
        }
        return nullptr;
    }

    int getStampState(Obj stampObj){
        std::int32_t st=-1;
        if(readFieldNamed<std::int32_t>(stampObj,{"_curHideState","_state","state"},st))return st;
        if(Method m=tryMethod(a.object_get_class(stampObj),"get_State",0)){
            try{return unbox<std::int32_t>(invoke(m,stampObj));}catch(...){}
        }
        return -1;
    }

    bool tileSurfaceAnchor(Obj stampObj,Vec3&outAnchor){
        Obj ren=readObjFieldNamed(stampObj,{"ObjectRenderer","_renderer","renderer","meshRenderer"});
        if(ren&&alive(ren)){
            try{
                Bounds b=unbox<Bounds>(call(ren,"get_bounds"));
                if(validBounds(b)&&b.extents.x>1e-5f&&b.extents.z>1e-5f){
                    outAnchor={b.center.x,b.center.y+b.extents.y,b.center.z};
                    return true;
                }
            }catch(...){}
        }
        Obj col=getStampCollider(stampObj);
        if(col&&alive(col)){
            try{
                Bounds b=boundsOfCollider(col);
                if(b.extents.x>1e-5f&&b.extents.z>1e-5f){
                    outAnchor={b.center.x,b.center.y+b.extents.y,b.center.z};
                    return true;
                }
            }catch(...){}
        }
        Obj tr=getTransform(stampObj);
        if(tr&&alive(tr)){
            try{
                outAnchor=worldPos(tr);
                return valid(outAnchor);
            }catch(...){}
        }
        return false;
    }

    bool inactiveHoleBounds(Obj col,Bounds&outBounds){
        if(!col||!alive(col))return false;
        std::string type=classLabel(col);
        Bounds localB{};
        try{
            if(type=="BoxCollider"){
                localB.center=unbox<Vec3>(call(col,"get_center"));
                Vec3 sz=unbox<Vec3>(call(col,"get_size"));
                localB.extents={sz.x*0.5f,sz.y*0.5f,sz.z*0.5f};
            }else if(type=="MeshCollider"){
                Obj mesh=call(col,"get_sharedMesh");
                if(!mesh||!alive(mesh))return false;
                localB=unbox<Bounds>(call(mesh,"get_bounds"));
            }else return false;
        }catch(...){return false;}

        Obj tr=getTransform(componentGO(col));
        if(!tr||!alive(tr))return false;

        Vec3 lo{1e30f,1e30f,1e30f},hi{-1e30f,-1e30f,-1e30f};
        for(int dx:{-1,1})for(int dy:{-1,1})for(int dz:{-1,1}){
            Vec3 corner{
                localB.center.x+float(dx)*localB.extents.x,
                localB.center.y+float(dy)*localB.extents.y,
                localB.center.z+float(dz)*localB.extents.z
            };
            void*args[]={&corner};
            Vec3 wp{};
            try{wp=unbox<Vec3>(call(tr,"TransformPoint",1,args));}catch(...){return false;}
            if(!valid(wp))return false;
            lo.x=std::min(lo.x,wp.x);lo.y=std::min(lo.y,wp.y);lo.z=std::min(lo.z,wp.z);
            hi.x=std::max(hi.x,wp.x);hi.y=std::max(hi.y,wp.y);hi.z=std::max(hi.z,wp.z);
        }
        outBounds.center={(lo.x+hi.x)*0.5f,(lo.y+hi.y)*0.5f,(lo.z+hi.z)*0.5f};
        outBounds.extents={(hi.x-lo.x)*0.5f,(hi.y-lo.y)*0.5f,(hi.z-lo.z)*0.5f};
        return validBounds(outBounds)&&outBounds.extents.x>0.f&&outBounds.extents.z>0.f;
    }

    // 1. Map 120018 (Kính Thật/Giả) & MiniGameKMGRealBoard (Ván Thật/Giả) - Cached once per round
    void collectRealBoardOrGlass(Obj gameObj,bool isGlass){
        Obj manager=getHideObjectManager(gameObj);
        if(!manager){espSummary="Kính/Ván: chờ HideObjectManager";return;}

        std::string rKey=std::to_string(reinterpret_cast<std::uintptr_t>(gameObj))+":"+
            std::to_string(reinterpret_cast<std::uintptr_t>(manager))+":"+(isGlass?"G":"B");
        if(boardCache.key!=rKey||!boardCache.ready){
            boardCache=BoardState{};
            boardCache.key=rKey;
            Obj listObj=readObjFieldNamed(manager,{"hideStampGameObjectList","hideStampObjectDataList","_hideStampObjectDataList"});
            auto stamps=readManagedList(listObj,2048);
            if(stamps.empty()){
                Obj boardRoot=isGlass?manager:readObjFieldNamed(manager,{"BoardRootTransform","_boardRoot","boardRoot"});
                if(!boardRoot||!alive(boardRoot))boardRoot=manager;
                Obj rootGo=componentGO(boardRoot);
                if(rootGo)stamps=getComponentsInChildrenByName(rootGo,"HideStampObjectData");
            }
            if(stamps.empty()){espSummary="Kính/Ván: chờ dữ liệu ô";return;}
            boardCache.tiles.reserve(stamps.size());
            for(Obj obj:stamps){
                if(!obj||!alive(obj))continue;
                BoardTileRec rec{};
                rec.comp.set(a,obj);
                if(Obj go=componentGO(obj))rec.go.set(a,go);
                if(Obj col=getStampCollider(obj))rec.col.set(a,col);
                Vec3 anc{};
                if(tileSurfaceAnchor(obj,anc)){rec.anchor=anc;rec.hasAnchor=true;}
                if(isGlass){
                    if(Obj stGo=readObjFieldNamed(obj,{"StableStateObject","stableObject","_stableObject"})){
                        if(alive(stGo))rec.stableGo.set(a,stGo);
                    }
                }
                boardCache.tiles.push_back(std::move(rec));
            }
            boardCache.ready=!boardCache.tiles.empty();
        }

        std::set<Obj> seenColliders;
        int greenCount=0,redCount=0;
        for(auto&rec:boardCache.tiles){
            Obj obj=rec.comp.get();
            if(!obj||!alive(obj))continue;
            int state=getStampState(obj);
            if(state<0||state>5)continue;

            if(isGlass&&state==5){
                Obj stableGo=rec.stableGo.get();
                if(stableGo&&alive(stableGo)&&activeGO(stableGo)){
                    if(!rec.hasStable){
                        auto cols=getComponentsInChildrenByName(stableGo,"Collider");
                        Obj bestCol=nullptr;float bestArea=0.f;Vec3 bestAnchor{};
                        for(Obj c:cols){
                            if(!solidCollider(c))continue;
                            try{
                                Bounds b=boundsOfCollider(c);
                                if(b.extents.x<=1e-5f||b.extents.z<=1e-5f||b.extents.y>std::min(b.extents.x,b.extents.z))continue;
                                float area=b.extents.x*b.extents.z;
                                if(area>bestArea){bestArea=area;bestCol=c;bestAnchor={b.center.x,b.center.y+b.extents.y,b.center.z};}
                            }catch(...){}
                        }
                        if(bestCol){
                            rec.stableCol.set(a,bestCol);
                            rec.stableAnchor=bestAnchor;
                            rec.hasStable=true;
                        }
                    }
                    Obj bestCol=rec.stableCol.get();
                    if(rec.hasStable&&bestCol&&alive(bestCol)&&!seenColliders.contains(bestCol)){
                        seenColliders.insert(bestCol);
                        worldMarkers.push_back({rec.stableAnchor,"green","",std::to_string(reinterpret_cast<std::uintptr_t>(bestCol)),-1,false,true,false});
                        ++greenCount;
                        continue;
                    }
                }
            }

            Obj go=rec.go.get();
            if(!go||!activeGO(go))continue;
            Obj col=rec.col.get();
            if(!col||!alive(col))col=getStampCollider(obj);
            if(!col||!alive(col)||!activeGO(componentGO(col)))continue;
            if(seenColliders.contains(col))continue;

            bool solid=(state==5)?solidCollider(col):false;
            std::string color=(state==5&&solid)?"green":"red";
            if(!rec.hasAnchor){
                Vec3 anc{};
                if(tileSurfaceAnchor(obj,anc)){rec.anchor=anc;rec.hasAnchor=true;}
                else continue;
            }
            seenColliders.insert(col);
            worldMarkers.push_back({rec.anchor,color,"",std::to_string(reinterpret_cast<std::uintptr_t>(col)),-1,false,true,false});
            if(color=="green")++greenCount;else ++redCount;
        }
        std::sort(worldMarkers.begin(),worldMarkers.end(),[](const WorldMarker&a,const WorldMarker&b){return a.key<b.key;});
        espSummary=std::string(isGlass?"Kính 120018":"Ván Thật/Giả")+": "+std::to_string(greenCount)+" xanh, "+std::to_string(redCount)+" đỏ";
    }

    // 2. Map 120026 (Obby Race *3 Vàng / VCNV02) - Zero-lag cached implementation
    void collectRoad120026(Obj gameObj,int battleState){
        Obj manager=getHideObjectManager(gameObj);
        if(!manager){espSummary="120026: chờ HideObjectManager";return;}

        std::string rKey=std::to_string(reinterpret_cast<std::uintptr_t>(gameObj))+":"+
            std::to_string(reinterpret_cast<std::uintptr_t>(manager));

        // Step 1: Cache the 27 Road_XX root GameObjects once per round without full hierarchy walks
        if(road120026.key!=rKey||road120026.vars.size()!=27){
            road120026=Road120026State{};
            road120026.key=rKey;
            Obj hideList=readObjFieldNamed(manager,{"hideGameObjectList","_hideGameObjectList"});
            auto objects=readManagedList(hideList,2048);
            if(objects.empty()){espSummary="120026: chờ danh sách đường";return;}
            for(Obj go:objects){
                if(!go||!alive(go))continue;
                std::string nm=objectName(go);
                if(nm.size()==7&&nm.compare(0,5,"Road_")==0){
                    int num=parseDigits(nm,5,2);
                    if(num>=1&&num<=27){
                        Obj pTr=getParent(getTransform(go));
                        if(pTr&&alive(pTr)&&objectName(componentGO(pTr))=="Road_01_Root"){
                            RoadVarRec rec{};
                            rec.go.set(a,go);
                            rec.name=nm;
                            road120026.vars.push_back(std::move(rec));
                        }
                    }
                }
            }
        }

        if(battleState!=5||road120026.vars.size()!=27){
            road120026.activeSolved=false;
            road120026.activeName.clear();
            road120026.activeGo.reset();
            espSummary="120026: chờ bắt đầu lượt chọn đường";
            return;
        }

        // Fast path: if the active Road_XX variant has already been solved and is still active,
        // reuse its cached markers in O(1) time (only 1 activeGO check, 0 hierarchy/collider scans!)
        Obj curActiveGo=road120026.activeGo.get();
        if(road120026.activeSolved&&curActiveGo&&alive(curActiveGo)&&activeGO(curActiveGo)){
            worldMarkers=road120026.cachedMarkers;
            espSummary=road120026.cachedSummary;
            return;
        }

        const RoadVarRec*selected=nullptr;
        for(const auto&v:road120026.vars){
            Obj g=v.go.get();
            if(g&&alive(g)&&activeGO(g)){
                if(selected){selected=nullptr;break;}
                selected=&v;
            }
        }
        if(!selected){espSummary="120026: chờ kích hoạt 1 phương án đường";return;}

        // Step 2: Fast targeted Transform DFS that tracks Road_line_1..3 down the tree
        // and only queries Collider components inside Road_line_1..3 subtrees (zero hierarchyNames calls!)
        Obj selGo=selected->go.get();
        Obj rootTr=getTransform(selGo);
        Method mChildCount=tryMethod(unity("Transform"),"get_childCount",0);
        Method mGetChild=tryMethod(unity("Transform"),"GetChild",1);
        Method mCompCount=tryMethod(unity("GameObject"),"GetComponentCount",0);
        Method mQueryComp=tryMethod(unity("GameObject"),"QueryComponentAtIndex",1);
        if(!rootTr||!mChildCount||!mGetChild||!mCompCount||!mQueryComp)return;

        std::map<int,std::vector<std::pair<Obj,Bounds>>> lanes;
        struct StackNode {Obj tr;int laneRow;};
        std::vector<StackNode> stack;
        stack.reserve(128);
        stack.push_back({rootTr,0});
        std::set<Obj> visited;

        while(!stack.empty()&&visited.size()<2048){
            auto[tr,laneRow]=stack.back();
            stack.pop_back();
            if(!tr||!alive(tr)||visited.contains(tr))continue;
            visited.insert(tr);

            Obj go=componentGO(tr);
            if(!go||!alive(go)||!activeGO(go))continue;

            int curRow=laneRow;
            if(curRow==0){
                std::string nm=objectName(go);
                if(nm.compare(0,10,"Road_line_")==0||nm.compare(0,10,"road_line_")==0){
                    if(nm.size()==11&&nm[10]>='1'&&nm[10]<='3')curRow=nm[10]-'0';
                    else if(nm.size()==12&&nm[10]=='0'&&nm[11]>='1'&&nm[11]<='3')curRow=nm[11]-'0';
                }
            }

            if(curRow>=1&&curRow<=3){
                int cCount=0;
                try{cCount=unbox<int>(invoke(mCompCount,go));}catch(...){cCount=0;}
                for(int i=0;i<cCount&&i<16;++i){
                    void*args[]={&i};
                    try{
                        Obj comp=invoke(mQueryComp,go,args);
                        if(!comp||!alive(comp))continue;
                        std::string t=classLabel(comp);
                        if(t!="BoxCollider"&&t!="MeshCollider")continue;
                        if(!colliderEnabled(comp)||colliderTrigger(comp))continue;
                        Bounds b=boundsOfCollider(comp);
                        if(b.extents.x<0.1f||b.extents.z<0.1f||b.extents.y>std::min(b.extents.x,b.extents.z))continue;
                        lanes[curRow].push_back({comp,b});
                    }catch(...){}
                }
            }

            int chCount=0;
            try{chCount=unbox<int>(invoke(mChildCount,tr));}catch(...){chCount=0;}
            for(int i=chCount-1;i>=0&&i<256;--i){
                void*args[]={&i};
                try{
                    Obj childTr=invoke(mGetChild,tr,args);
                    if(childTr&&alive(childTr))stack.push_back({childTr,curRow});
                }catch(...){}
            }
        }

        int verifiedRows=0;
        for(auto&[row,candidates]:lanes){
            std::sort(candidates.begin(),candidates.end(),[](const auto&a,const auto&b){
                return a.second.extents.x*a.second.extents.z>b.second.extents.x*b.second.extents.z;
            });
            if(!candidates.empty()){
                const Bounds&b=candidates[0].second;
                ++verifiedRows;
                for(float frac:{-0.6f,0.f,0.6f}){
                    Vec3 pt{b.center.x,b.center.y+b.extents.y+0.04f,b.center.z+frac*b.extents.z};
                    const bool isCenter=(std::fabs(frac)<0.01f);
                    worldMarkers.push_back({pt,"green",isCenter?("H"+std::to_string(row)):std::string(),"road:"+std::to_string(row),row,true,isCenter,isCenter});
                }
            }
        }
        espSummary="120026 ("+selected->name+"): "+std::to_string(verifiedRows)+"/3 hàng sàn";
        if(verifiedRows>0){
            road120026.activeGo.set(a,selGo);
            road120026.activeName=selected->name;
            road120026.cachedMarkers=worldMarkers;
            road120026.cachedSummary=espSummary;
            road120026.activeSolved=true;
        }
    }

    // 3. Map 120006 (Map Cổng YES / NO - Map 3 Cửa + Line) - Cached once per round
    void collectDoors120006(Obj gameObj,int battleState,Vec3 playerPos){
        Obj manager=getHideObjectManager(gameObj);
        if(!manager){espSummary="120006: chờ HideObjectManager";return;}

        std::string rKey=std::to_string(reinterpret_cast<std::uintptr_t>(gameObj))+":"+
            std::to_string(reinterpret_cast<std::uintptr_t>(manager));
        if(doors120006.key!=rKey||!doors120006.ready){
            doors120006=Door120006State{};
            doors120006.key=rKey;
            Obj hideList=readObjFieldNamed(manager,{"hideGameObjectList","_hideGameObjectList"});
            auto objects=readManagedList(hideList,2048);
            if(objects.empty()){espSummary="120006: chờ danh sách cửa";return;}
            for(Obj go:objects){
                if(!go||!alive(go))continue;
                Obj tr=getTransform(go);
                if(!tr)continue;
                auto path=hierarchyNames(tr);
                DoorChoice ch{};
                if(!parseDoorIdentity(path,ch))continue;
                Vec3 anc{};
                try{anc=worldPos(tr);}catch(...){continue;}
                DoorObjRec rec{};
                rec.go.set(a,go);
                if(Obj pTr=getParent(tr)){
                    if(Obj pGo=componentGO(pTr))rec.parentGo.set(a,pGo);
                }
                rec.choice=ch;
                rec.anchor=anc;
                for(Obj c:getComponentsInChildrenByName(go,"Collider")){
                    if(c&&alive(c)){
                        Root cr(a,c);
                        rec.colliders.push_back(std::move(cr));
                    }
                }
                doors120006.items.push_back(std::move(rec));
            }
            doors120006.ready=!doors120006.items.empty();
        }
        if(!doors120006.ready){espSummary="120006: chờ danh sách cửa";return;}

        struct DoorRow {
            DoorChoice choice;
            Vec3 anchor{};
            bool active=false,parentActive=false,known=false,solid=false,suppress=false;
            int colliderCount=0;
            std::string color="yellow";
        };
        std::map<std::string,std::vector<DoorRow>> doorGroups;
        std::map<std::string,std::vector<DoorRow>> wallColGroups;

        for(auto&rec:doors120006.items){
            Obj go=rec.go.get();
            if(!go||!alive(go))continue;
            DoorRow row{};
            row.choice=rec.choice;
            row.anchor=rec.anchor;
            Obj pGo=rec.parentGo.get();
            row.parentActive=pGo&&alive(pGo)&&activeGO(pGo);
            row.active=activeGO(go);
            row.colliderCount=int(rec.colliders.size());
            row.solid=false;
            for(auto&cr:rec.colliders){
                Obj c=cr.get();
                if(c&&solidCollider(c)){row.solid=true;break;}
            }
            row.known=row.parentActive&&row.colliderCount>0;
            if(row.choice.kind=="door")doorGroups[row.choice.group].push_back(row);
            else wallColGroups[row.choice.group].push_back(row);
        }

        const bool playing=(battleState==5);
        std::vector<DoorRow> allDoors,allWallCols;

        for(auto&[grp,rows]:doorGroups){
            std::set<std::string> sides;
            bool allParent=true;
            for(const auto&r:rows){sides.insert(r.choice.side);if(!r.parentActive)allParent=false;}
            const bool complete=(rows.size()==3&&sides.size()==3&&allParent);
            int inactiveCount=0;bool collidersAgree=true;
            for(const auto&r:rows){
                if(!r.active)++inactiveCount;
                if(r.colliderCount>0&&r.solid!=r.active)collidersAgree=false;
            }
            if(playing&&complete&&inactiveCount==1&&collidersAgree){
                for(auto&r:rows){r.known=true;r.solid=r.active;r.color=r.active?"red":"green";}
            }else{
                bool allKnown=complete;int openCount=0;
                for(const auto&r:rows){if(!r.known)allKnown=false;else if(!r.solid)++openCount;}
                for(auto&r:rows)r.color=(playing&&allKnown&&openCount<=1)?(r.solid?"red":"green"):"yellow";
            }
            for(const auto&r:rows)allDoors.push_back(r);
        }

        for(auto&[grp,rows]:wallColGroups){
            std::set<std::string> sides;bool allParent=true;int activeCount=0;
            for(const auto&r:rows){
                sides.insert(r.choice.side);
                if(!r.parentActive)allParent=false;
                if(r.active)++activeCount;
            }
            const bool paired=(playing&&rows.size()==2&&sides.size()==2&&allParent&&activeCount==1);
            if(paired){
                if(rows[0].choice.kind=="column"){
                    for(auto&r:rows){
                        if(!r.active)r.suppress=true;
                        else{
                            bool pass=(r.choice.side=="through");
                            r.color=pass?"green":"red";
                            r.solid=!pass;
                        }
                    }
                }else{
                    for(auto&r:rows){
                        r.color=r.active?"red":"green";
                        r.solid=r.active;
                    }
                }
            }else{
                for(auto&r:rows){
                    if(r.known)r.color=r.solid?"red":"green";
                    else r.color="yellow";
                }
            }
            for(const auto&r:rows)allWallCols.push_back(r);
        }

        int yesCount=0,noCount=0;
        auto emitDoorMarker=[&](const DoorRow&r){
            if(r.suppress)return;
            std::string lbl=r.color=="green"?"YES":(r.color=="red"?"NO":"CHƯA RÕ");
            if(r.color=="green")++yesCount;else if(r.color=="red")++noCount;
            worldMarkers.push_back({r.anchor,r.color,lbl,r.choice.group+":"+r.choice.side,0,r.color=="green",true,true});
        };
        for(const auto&r:allDoors)emitDoorMarker(r);
        for(const auto&r:allWallCols)emitDoorMarker(r);

        struct Gate {std::string key;Vec3 center{},anchor{};bool passable=false;};
        std::vector<Gate> gates;
        auto makeGate=[&](const std::string&key,const std::vector<DoorRow>&group,std::size_t expectedSize)->bool{
            if(group.size()!=expectedSize)return false;
            Vec3 c{0,0,0};
            const DoorRow*openRow=nullptr;int openCnt=0;
            for(const auto&r:group){
                c.x+=r.anchor.x;c.y+=r.anchor.y;c.z+=r.anchor.z;
                if(r.color=="green"&&!r.solid){openRow=&r;++openCnt;}
            }
            c.x/=float(group.size());c.y/=float(group.size());c.z/=float(group.size());
            bool pass=(openCnt==1&&openRow!=nullptr);
            gates.push_back({key,c,pass?openRow->anchor:c,pass});
            return true;
        };

        bool gatesOk=playing&&valid(playerPos);
        for(int n=1;n<=4&&gatesOk;++n){
            std::string k="wall"+std::to_string(n);
            gatesOk=makeGate(k,wallColGroups[k],2);
        }
        if(gatesOk){
            const auto&cols=wallColGroups["entrance-column"];
            if(cols.size()==2){
                Vec3 c{(cols[0].anchor.x+cols[1].anchor.x)*0.5f,(cols[0].anchor.y+cols[1].anchor.y)*0.5f,(cols[0].anchor.z+cols[1].anchor.z)*0.5f};
                const DoorRow*act=cols[0].active?&cols[0]:(cols[1].active?&cols[1]:nullptr);
                bool pass=(act&&act->color=="green"&&!act->solid);
                gates.push_back({"entrance-column",c,act?act->anchor:c,pass});
            }else gatesOk=false;
        }
        if(gatesOk)gatesOk=makeGate("door1",doorGroups["door1"],3);

        if(gatesOk&&gates.size()==6){
            float dx=gates.back().center.x-gates.front().center.x;
            float dz=gates.back().center.z-gates.front().center.z;
            bool useX=std::fabs(dx)>std::fabs(dz);
            float delta=useX?dx:dz;
            float sign=delta>0.f?1.f:(delta<0.f?-1.f:0.f);
            if(sign!=0.f&&std::max(std::fabs(dx),std::fabs(dz))>=5.f){
                auto coord=[&](Vec3 p){return sign*(useX?p.x:p.z);};
                std::sort(gates.begin(),gates.end(),[&](const Gate&a,const Gate&b){return coord(a.center)<coord(b.center);});
                float progress=coord(playerPos);
                routeNodes.push_back({{playerPos.x,playerPos.y+0.05f,playerPos.z},"@player",false,0,false});
                auto appendNode=[&](const Gate&g,float offset,const char*suffix){
                    Vec3 a=g.anchor;
                    if(useX)a.x+=sign*offset;else a.z+=sign*offset;
                    a.y+=0.2f;
                    if(distXZ(routeNodes.back().anchor,a)>0.05f){
                        routeNodes.push_back({a,g.key+":"+suffix,false,0,false});
                    }
                };
                constexpr float approach=2.f;
                for(const auto&g:gates){
                    if(progress>coord(g.center)+approach)continue;
                    if(!g.passable){
                        if(progress<coord(g.center)-approach)appendNode(g,-approach,"stop");
                        break;
                    }
                    if(progress<coord(g.anchor)-approach)appendNode(g,-approach,"approach");
                    if(progress<=coord(g.anchor))appendNode(g,0.f,"pass");
                    appendNode(g,approach,"exit");
                }
            }
        }
        currentRouteStage=0;
        espSummary="120006 (Cổng YES/NO): "+std::to_string(yesCount)+" YES, "+std::to_string(noCount)+" NO";
    }

    // 4. Map 120015 (Map Tìm Lỗ + Line) - Hiển thị TẤT CẢ các lỗ rớt xuống được màu xanh lá (không lag!)
    void collectHoles120015(Obj gameObj,int battleState,Vec3 playerPos){
        (void)battleState;
        Obj manager=getHideObjectManager(gameObj);
        if(!manager){espSummary="120015: chờ HideObjectManager";return;}
        Obj stampListObj=readObjFieldNamed(manager,{"hideStampGameObjectList","hideStampObjectDataList","_hideStampObjectDataList"});
        auto registeredVec=readManagedList(stampListObj,2048);
        if(registeredVec.empty()){espSummary="120015: chờ danh sách lỗ";return;}

        // One-time cache of drop-hole tiles (_maxStep == 0) + their parent Stage_Board GameObject
        // Zero string parsing (`get_name` / `hierarchyNames`) and zero scene Collider scan -> zero lag!
        std::string roundKey=std::to_string(reinterpret_cast<std::uintptr_t>(gameObj))+":"+
            std::to_string(reinterpret_cast<std::uintptr_t>(manager))+":"+
            std::to_string(registeredVec.size());
        if(cachedRoundKey!=roundKey||!holeStructureReady){
            holeTiles.clear();holeSavedAnchors.clear();
            cachedRoundKey=roundKey;
            holeTiles.reserve(registeredVec.size());
            for(Obj s:registeredVec){
                if(!s||!alive(s))continue;
                // Only cache tiles with _maxStep == 0 (real drop holes, skip solid multi-step blocks)
                std::int32_t maxStep=0;
                readFieldNamed<std::int32_t>(s,{"_maxStep","maxStep"},maxStep);
                if(maxStep!=0)continue;

                Obj t=readObjFieldNamed(s,{"_transform","transform"});
                if(!t||!alive(t))t=getTransform(s);
                if(!t||!alive(t))continue;

                // Parent chain: DropBoard_stageXX_Stamp_YY -> Stamp -> Stage_Board_XX_x
                // Even when a hole is already open (its own GameObject deactivated), its Stage_Board_XX_x remains active!
                Obj p1=getParent(t);
                Obj p2=p1?getParent(p1):nullptr;
                Obj bGo=p2?componentGO(p2):(p1?componentGO(p1):componentGO(s));

                HoleTileRec rec{};
                rec.comp.set(a,s);
                rec.tr.set(a,t);
                if(bGo&&alive(bGo))rec.boardGo.set(a,bGo);
                try{
                    Vec3 wp=worldPos(t);
                    if(valid(wp)){rec.cachedPos=wp;rec.hasCachedPos=true;}
                }catch(...){}
                holeTiles.push_back(std::move(rec));
            }
            holeStructureReady=!holeTiles.empty();
        }
        if(!holeStructureReady){espSummary="120015: chờ dữ liệu ô";return;}

        // Deduplicate active checks on Stage_Board_XX_x (at most 35 board variants total -> super fast!)
        std::map<Obj,bool> boardActiveCache;
        auto isBoardActive=[&](Obj bGo)->bool{
            if(!bGo)return false;
            if(auto it=boardActiveCache.find(bGo);it!=boardActiveCache.end())return it->second;
            bool act=alive(bGo)&&activeGO(bGo);
            boardActiveCache[bGo]=act;
            return act;
        };

        struct HoleCand {std::string key;Vec3 anchor{};float dist=0.f;};
        std::vector<HoleCand> allDropHoles;
        allDropHoles.reserve(64);

        for(auto&rec:holeTiles){
            Obj obj=rec.comp.get();
            if(!obj||!alive(obj))continue;

            // Check if this hole belongs to the active Stage_Board variant of its floor
            Obj bGo=rec.boardGo.get();
            if(bGo){
                if(!isBoardActive(bGo))continue;
            }else{
                Obj go=componentGO(obj);
                if(!activeGO(go))continue;
            }

            // State 0..4 are drop-able holes (State 0 = false tile that drops when stepped on,
            // State 1..3 = already opened hole you can drop right through; State 5 = Stable solid block)
            int state=getStampState(obj);
            if(state<0||state==5)continue;
            if(state==0){
                Obj col=getStampCollider(obj);
                if(!col||!alive(col))continue;
            }

            if(!rec.hasCachedPos){
                Obj t=rec.tr.get();
                if(!t||!alive(t))continue;
                try{
                    Vec3 wp=worldPos(t);
                    if(valid(wp)){rec.cachedPos=wp;rec.hasCachedPos=true;}
                }catch(...){}
                if(!rec.hasCachedPos)continue;
            }

            // Show all drop holes at or below player's current floor (skip floors already passed above head)
            if(valid(playerPos)&&rec.cachedPos.y>playerPos.y+1.5f)continue;

            Vec3 anchor{rec.cachedPos.x,rec.cachedPos.y+0.05f,rec.cachedPos.z};
            std::string key=std::to_string(reinterpret_cast<std::uintptr_t>(obj));
            allDropHoles.push_back({key,anchor,valid(playerPos)?distXZ(playerPos,anchor):0.f});
        }

        // Find the current floor Y (highest floor <= playerPos.y + 0.8f) to connect the guiding Line
        float currentFloorY=-1e30f;
        for(const auto&c:allDropHoles){
            if(c.anchor.y<=playerPos.y+0.8f&&c.anchor.y>currentFloorY){
                currentFloorY=c.anchor.y;
            }
        }

        std::string bestKey;
        float bestDist=1e30f;
        if(currentFloorY>-1e20f){
            for(const auto&c:allDropHoles){
                if(std::fabs(c.anchor.y-currentFloorY)<=1.5f&&c.dist<bestDist){
                    bestDist=c.dist;
                    bestKey=c.key;
                }
            }
        }else if(!allDropHoles.empty()){
            for(const auto&c:allDropHoles){
                if(c.dist<bestDist){bestDist=c.dist;bestKey=c.key;}
            }
        }

        // Emit green ESP for ALL drop-able holes!
        for(const auto&h:allDropHoles){
            bool isBest=(h.key==bestKey);
            if(isBest&&valid(playerPos)){
                currentRouteStage=0;
                routeNodes.push_back({{playerPos.x,playerPos.y+0.04f,playerPos.z},"@feet",false,0,false});
                routeNodes.push_back({{h.anchor.x,h.anchor.y+0.04f,h.anchor.z},h.key,false,0,false});
            }
            worldMarkers.push_back({h.anchor,"green",isBest?"LỖ RỚT":"LỖ",h.key,0,isBest,true,isBest});
        }

        // Goal marker at bottom floor
        Obj endpointGo=readObjFieldNamed(gameObj,{"endPointGo","_endPointGo","EndPointGo"});
        if(!endpointGo||!alive(endpointGo))endpointGo=readObjFieldNamed(manager,{"EndPoint","endPoint","endPointGo"});
        if(endpointGo&&alive(endpointGo)&&activeGO(endpointGo)){
            try{
                Vec3 goalPos=worldPos(getTransform(endpointGo));
                if(valid(goalPos)){
                    worldMarkers.push_back({goalPos,"green","ĐÍCH","@goal",0,true,true,true});
                    if(allDropHoles.empty()&&valid(playerPos)){
                        routeNodes.clear();
                        routeNodes.push_back({{playerPos.x,playerPos.y+0.04f,playerPos.z},"@feet",false,0,false});
                        routeNodes.push_back({{goalPos.x,goalPos.y+0.04f,goalPos.z},"@goal",false,0,true});
                    }
                }
            }catch(...){}
        }

        espSummary="120015 (Map Lỗ): "+std::to_string(allDropHoles.size())+" lỗ rớt được";
    }

    // 5. Map 120041 (MiniGameKMGFogMaze - Đích Mê Cung + Line A*8)
    // Exact dump.cs fields on MiniGameKMGFogMaze:
    // `public FowManager FogManager; // 0x1D8`
    // `public GameObject EndPointGo; // 0x1E0`
    // `private HeadUpLandMark headUpLandMark; // 0x1E8` (inherits `protected Camera mainCamera; // 0x90`)
    void collectMaze120041(Obj gameObj,int battleState,Vec3 playerPos){
        Obj fowManager=readObjFieldNamed(gameObj,{"FogManager","_fowManager","fowManager"});
        Obj endpointGo=readObjFieldNamed(gameObj,{"EndPointGo","endPointGo","_endPointGo"});
        Obj manager=getHideObjectManager(gameObj);
        if((!endpointGo||!alive(endpointGo))&&manager){
            endpointGo=readObjFieldNamed(manager,{"EndPoint","endPoint","EndPointGo"});
        }
        Vec3 goalPos{};bool hasGoal=false;
        if(endpointGo&&alive(endpointGo)){
            try{goalPos=worldPos(getTransform(endpointGo));hasGoal=valid(goalPos);}catch(...){}
        }
        if(!hasGoal&&fowManager&&alive(fowManager)){
            Obj endTr=readObjFieldNamed(fowManager,{"EndingPoint","_endingPoint","endingPoint"});
            if(endTr&&alive(endTr)){
                try{goalPos=worldPos(endTr);hasGoal=valid(goalPos);}catch(...){}
            }
        }
        if(!hasGoal){
            espSummary="120041: chờ cổng đích mê cung";return;
        }

        worldMarkers.push_back({goalPos,"green","ĐÍCH","@maze_goal",0,true,true,true});

        Vec3 startPos=playerPos;
        if(fowManager&&alive(fowManager)){
            Obj startTr=readObjFieldNamed(fowManager,{"StartingPoint","_startingPoint","startingPoint"});
            if(startTr&&alive(startTr)){
                try{Vec3 sp=worldPos(startTr);if(valid(sp))startPos=sp;}catch(...){}
            }
        }

        if(battleState>=6&&battleState<=10){
            espSummary="120041 (Mê Cung): đã hiện ĐÍCH • hết lượt";
            return;
        }

        std::string mapKey=std::to_string(reinterpret_cast<std::uintptr_t>(gameObj));
        if(maze.key!=mapKey||maze.activeVariant.empty()){
            maze=MazeState{};maze.key=mapKey;

            // Fast path: check hideObjectManager->hideGameObjectList first for active MazeCreate_S_XX_D variant
            Obj activeVariantGo=nullptr;
            if(manager){
                Obj hideList=readObjFieldNamed(manager,{"hideGameObjectList","_hideGameObjectList"});
                for(Obj go:readManagedList(hideList,2048)){
                    if(!go||!alive(go))continue;
                    std::string nm=objectName(go);
                    if(nm.size()==17&&nm.compare(0,13,"MazeCreate_S_")==0&&activeGO(go)){
                        maze.activeVariant=nm;
                        activeVariantGo=go;
                        break;
                    }
                }
            }

            // Scan colliders under activeVariantGo (fast) or topAncestorGO(gameObj)
            Obj scanRoot=activeVariantGo?activeVariantGo:topAncestorGO(gameObj);
            for(Obj c:getComponentsInChildrenByName(scanRoot,"Collider")){
                if(!c||!alive(c))continue;
                std::string t=classLabel(c);
                auto path=hierarchyNames(getTransform(componentGO(c)));
                std::string var;
                if(t=="MeshCollider"&&parseMazeVariant(path,var)&&solidCollider(c)){
                    maze.meshCol.set(a,c);
                    if(maze.activeVariant.empty())maze.activeVariant=var;
                    try{maze.meshBounds=boundsOfCollider(c);}catch(...){}
                }else if(t=="BoxCollider"){
                    if(maze.activeVariant.empty()&&parseMazeVariant(path,var)&&solidCollider(c)){
                        maze.activeVariant=var;
                    }
                    Root r(a,c);
                    maze.boxColliders.push_back(std::move(r));
                    maze.boxPaths.push_back(std::move(path));
                }
            }
        }

        Bounds floorB{Vec3{(startPos.x+goalPos.x)*0.5f,std::min(startPos.y,goalPos.y)-0.5f,(startPos.z+goalPos.z)*0.5f},Vec3{60.f,0.5f,60.f}};
        std::vector<Bounds> solidBoxes;
        Vec3 boxMin{1e30f,1e30f,1e30f},boxMax{-1e30f,-1e30f,-1e30f};
        for(std::size_t i=0;i<maze.boxColliders.size();++i){
            Obj c=maze.boxColliders[i].get();
            if(!c||!alive(c)||!solidCollider(c))continue;
            const auto&p=maze.boxPaths[i];
            if(!maze.activeVariant.empty()){
                auto itRoot=std::find(p.begin(),p.end(),"RandomMaze_Root");
                if(itRoot!=p.end()&&(itRoot+1)!=p.end()&&*(itRoot+1)!=maze.activeVariant)continue;
            }
            try{
                Bounds b=boundsOfCollider(c);
                if(p.size()>=2&&p[p.size()-2]=="Default_board"&&p.back()=="Board"){
                    floorB=b;
                }else if(b.extents.x>12.f&&b.extents.z>12.f&&b.extents.y<1.2f){
                    floorB=b;
                }else{
                    solidBoxes.push_back(b);
                    boxMin.x=std::min(boxMin.x,b.center.x-b.extents.x);
                    boxMin.z=std::min(boxMin.z,b.center.z-b.extents.z);
                    boxMax.x=std::max(boxMax.x,b.center.x+b.extents.x);
                    boxMax.z=std::max(boxMax.z,b.center.z+b.extents.z);
                }
            }catch(...){}
        }

        std::string sig=maze.activeVariant+":"+std::to_string(solidBoxes.size());
        if(!maze.routeReady||maze.routeSignature!=sig){
            maze.routeSignature=sig;
            maze.solvedNodes.clear();
            maze.routeReady=false;

            const float floorY=valid(playerPos)?playerPos.y:(floorB.center.y+floorB.extents.y);
            const float pad=0.18f,step=0.5f;
            float mx0=maze.meshBounds.extents.x>0.5f?(maze.meshBounds.center.x-maze.meshBounds.extents.x):(boxMin.x<1e20f?boxMin.x:std::min(startPos.x,goalPos.x)-10.f);
            float mz0=maze.meshBounds.extents.z>0.5f?(maze.meshBounds.center.z-maze.meshBounds.extents.z):(boxMin.z<1e20f?boxMin.z:std::min(startPos.z,goalPos.z)-10.f);
            float mx1=maze.meshBounds.extents.x>0.5f?(maze.meshBounds.center.x+maze.meshBounds.extents.x):(boxMax.x>-1e20f?boxMax.x:std::max(startPos.x,goalPos.x)+10.f);
            float mz1=maze.meshBounds.extents.z>0.5f?(maze.meshBounds.center.z+maze.meshBounds.extents.z):(boxMax.z>-1e20f?boxMax.z:std::max(startPos.z,goalPos.z)+10.f);
            const float x0=std::min({mx0,startPos.x-3.f,goalPos.x-3.f});
            const float z0=std::min({mz0,startPos.z-3.f,goalPos.z-3.f});
            const float xmax=std::max({mx1,startPos.x+3.f,goalPos.x+3.f});
            const float zmax=std::max({mz1,startPos.z+3.f,goalPos.z+3.f});
            const int w=int(std::ceil((xmax-x0)/step))+1;
            const int h=int(std::ceil((zmax-z0)/step))+1;
            if(w>2&&h>2&&w*h<=60000){
                std::vector<std::uint8_t> blocked(std::size_t(w*h),0);
                std::vector<std::array<float,4>> obstacles;
                auto stampRect=[&](float ax,float az,float bx,float bz){
                    obstacles.push_back({ax,az,bx,bz});
                    int loX=std::max(0,int(std::ceil((ax-x0)/step)));
                    int hiX=std::min(w-1,int(std::floor((bx-x0)/step)));
                    int loZ=std::max(0,int(std::ceil((az-z0)/step)));
                    int hiZ=std::min(h-1,int(std::floor((bz-z0)/step)));
                    for(int zz=loZ;zz<=hiZ;++zz)for(int xx=loX;xx<=hiX;++xx)blocked[std::size_t(zz*w+xx)]=1;
                };
                for(const Bounds&b:solidBoxes){
                    // Maze walls are vertical obstacles (extents.y >= 0.25f) and not huge floor slabs
                    if(b.extents.y>=0.25f&&!(b.extents.x>12.f&&b.extents.z>12.f)){
                        stampRect(b.center.x-b.extents.x-pad,b.center.z-b.extents.z-pad,
                                  b.center.x+b.extents.x+pad,b.center.z+b.extents.z+pad);
                    }
                }
                auto cellPt=[&](int idx)->Vec3{return {x0+float(idx%w)*step,floorY+0.08f,z0+float(idx/w)*step};};
                auto nearestFree=[&](Vec3 p)->int{
                    int cx=int(std::round((p.x-x0)/step)),cz=int(std::round((p.z-z0)/step));
                    int best=-1;float bestD=1e30f;
                    for(int r=0;r<=16&&best<0;++r){
                        for(int dz=-r;dz<=r;++dz)for(int dx=-r;dx<=r;++dx){
                            int xx=cx+dx,zz=cz+dz;
                            if(xx<0||xx>=w||zz<0||zz>=h)continue;
                            int idx=zz*w+xx;
                            if(!blocked[std::size_t(idx)]){
                                float d=distXZ(p,cellPt(idx));
                                if(d<bestD){bestD=d;best=idx;}
                            }
                        }
                    }
                    return best;
                };
                int sIdx=nearestFree(startPos),gIdx=nearestFree(goalPos);
                if(sIdx>=0&&gIdx>=0){
                    std::vector<float> dist(std::size_t(w*h),1e30f);
                    std::vector<int> prev(std::size_t(w*h),-1);
                    std::vector<std::uint8_t> closed(std::size_t(w*h),0);
                    using QItem=std::pair<float,int>;
                    std::priority_queue<QItem,std::vector<QItem>,std::greater<QItem>> pq;
                    dist[std::size_t(sIdx)]=0.f;prev[std::size_t(sIdx)]=sIdx;
                    pq.push({0.f,sIdx});
                    int gx=gIdx%w,gz=gIdx/w;
                    while(!pq.empty()){
                        auto[f,u]=pq.top();pq.pop();
                        if(closed[std::size_t(u)])continue;
                        closed[std::size_t(u)]=1;
                        if(u==gIdx)break;
                        int ux=u%w,uz=u/w;
                        float du=dist[std::size_t(u)];
                        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx){
                            if(!dx&&!dz)continue;
                            int nx=ux+dx,nz=uz+dz;
                            if(nx<0||nx>=w||nz<0||nz>=h)continue;
                            int v=nz*w+nx;
                            if(blocked[std::size_t(v)]||closed[std::size_t(v)])continue;
                            if(dx&&dz&&(blocked[std::size_t(uz*w+nx)]||blocked[std::size_t(nz*w+ux)]))continue;
                            float nd=du+((dx&&dz)?1.41421356f:1.f);
                            if(nd+1e-6f<dist[std::size_t(v)]){
                                dist[std::size_t(v)]=nd;prev[std::size_t(v)]=u;
                                float hCost=std::hypot(float(nx-gx),float(nz-gz));
                                pq.push({nd+hCost,v});
                            }
                        }
                    }
                    if(prev[std::size_t(gIdx)]>=0){
                        std::vector<Vec3> chain;
                        for(int cur=gIdx;;cur=prev[std::size_t(cur)]){
                            chain.push_back(cellPt(cur));
                            if(cur==sIdx||prev[std::size_t(cur)]==cur)break;
                        }
                        std::reverse(chain.begin(),chain.end());
                        auto segClear=[&](Vec3 a,Vec3 b){
                            for(const auto&obs:obstacles)if(mazeSegmentHitsBox(a,b,obs))return false;
                            return true;
                        };
                        std::vector<Vec3> smooth={chain.front()};
                        std::size_t at=0;
                        while(at+1<chain.size()){
                            std::size_t next=at+1;
                            for(std::size_t j=chain.size()-1;j>at+1;--j){
                                if(segClear(chain[at],chain[j])){next=j;break;}
                            }
                            smooth.push_back(chain[next]);
                            at=next;
                        }
                        for(std::size_t i=0;i<smooth.size();++i){
                            maze.solvedNodes.push_back({smooth[i],"maze:"+std::to_string(i),false,0,i+1==smooth.size()});
                        }
                        maze.routeReady=(maze.solvedNodes.size()>=2);
                    }
                }
            }
        }

        // Dynamic live route from player's current position to the maze exit
        if(maze.routeReady&&maze.solvedNodes.size()>=2){
            // Find the closest node ahead on the solved path so the line follows the player live
            std::size_t bestIdx=0;
            float bestD=1e30f;
            for(std::size_t i=0;i<maze.solvedNodes.size();++i){
                float d=distXZ(playerPos,maze.solvedNodes[i].anchor);
                if(d<bestD){bestD=d;bestIdx=i;}
            }
            if(bestIdx+1<maze.solvedNodes.size()&&bestD<2.5f)++bestIdx;
            routeNodes.push_back({{playerPos.x,playerPos.y+0.08f,playerPos.z},"@feet",false,0,false});
            for(std::size_t i=bestIdx;i<maze.solvedNodes.size();++i){
                Vec3 pt=maze.solvedNodes[i].anchor;
                pt.y=playerPos.y+0.08f;
                routeNodes.push_back({pt,maze.solvedNodes[i].key,false,0,maze.solvedNodes[i].goal});
            }
            currentRouteStage=0;
            espSummary="120041 ("+(maze.activeVariant.empty()?"Mê Cung":maze.activeVariant)+"): ĐÍCH + Line dẫn đường";
        }else{
            // Fallback direct guiding line from player's feet to the Maze Goal so Line is ALWAYS visible!
            currentRouteStage=0;
            routeNodes.push_back({{playerPos.x,playerPos.y+0.08f,playerPos.z},"@feet",false,0,false});
            routeNodes.push_back({{goalPos.x,goalPos.y+0.15f,goalPos.z},"@maze_goal",false,0,true});
            espSummary="120041 ("+(maze.activeVariant.empty()?"Mê Cung":maze.activeVariant)+"): ĐÍCH + Line hướng đích";
        }
    }

    // Multi-source active 3D Camera resolver:
    // 1. Camera.get_main()
    // 2. MiniGameKMGFogMaze / MiniGameKMGCrossyRoad -> headUpLandMark -> mainCamera (0x90)
    // 3. SystemHelper.get_CinemachineBrain() -> get_OutputCamera() / m_OutputCamera (0x50)
    // 4. SystemHelper.get_Map() -> m_CurrentMap -> cinemachineBrain (0x40) -> m_OutputCamera (0x50)
    Obj resolveActiveCamera(){
        if(Method mMain=tryMethod(unity("Camera"),"get_main",0)){
            try{
                Obj cam=invoke(mMain,nullptr);
                if(cam&&alive(cam))return cam;
            }catch(...){}
        }
        Obj mg=miniGameRoot.get();
        if(mg&&alive(mg)){
            Obj landMark=readObjFieldNamed(mg,{"headUpLandMark","_headUpLandMark"});
            if(landMark&&alive(landMark)){
                Obj cam=readObjFieldNamed(landMark,{"mainCamera","_mainCamera"});
                if(cam&&alive(cam))return cam;
            }
        }
        if(Klass sysHelper=tryGame("SystemHelper")){
            if(Method mBrain=tryMethod(sysHelper,"get_CinemachineBrain",0)){
                try{
                    Obj brain=invoke(mBrain,nullptr);
                    if(brain&&alive(brain)){
                        Obj cam=readObjFieldNamed(brain,{"m_OutputCamera","OutputCamera"});
                        if(cam&&alive(cam))return cam;
                        if(Method mOut=tryMethod(a.object_get_class(brain),"get_OutputCamera",0)){
                            cam=invoke(mOut,brain);
                            if(cam&&alive(cam))return cam;
                        }
                    }
                }catch(...){}
            }
            if(Method mGetMap=tryMethod(sysHelper,"get_Map",0)){
                try{
                    Obj mapSys=invoke(mGetMap,nullptr);
                    if(mapSys&&alive(mapSys)){
                        Obj curMap=readObjFieldNamed(mapSys,{"m_CurrentMap","_currentMap","currentMap"});
                        if(curMap&&alive(curMap)){
                            Obj brain=readObjFieldNamed(curMap,{"cinemachineBrain","_cinemachineBrain"});
                            if(brain&&alive(brain)){
                                Obj cam=readObjFieldNamed(brain,{"m_OutputCamera","OutputCamera"});
                                if(cam&&alive(cam))return cam;
                            }
                        }
                    }
                }catch(...){}
            }
        }
        return nullptr;
    }

    void projectCurrentWorldToScreen(Vec3 playerPos){
        points.clear();routeSegments.clear();
        if(worldMarkers.empty()&&routeNodes.empty())return;
        Obj cam=activeCamRoot.get();
        if(!cam||!alive(cam)||(lastTick-camCheckTime)>=0.5){
            camCheckTime=lastTick;
            Obj resolved=resolveActiveCamera();
            if(resolved&&alive(resolved)){
                if(resolved!=cam)activeCamRoot.set(a,resolved);
                cam=resolved;
                try{
                    Rect vr=unbox<Rect>(call(cam,"get_rect"));
                    if(std::isfinite(vr.w)&&std::isfinite(vr.h)&&vr.w>0.05f&&vr.h>0.05f)cachedViewport=vr;
                }catch(...){}
            }
        }
        if(!cam||!alive(cam)){needsSettle=true;return;}
        const Rect viewport=cachedViewport;
        Method mW2V=tryMethod(a.object_get_class(cam),"WorldToViewportPoint",1);
        if(!mW2V)return;

        auto project=[&](Vec3 p)->ProjectedPt{
            void*v[]={&p};
            Obj ex=nullptr;
            Obj boxed=a.runtime_invoke(mW2V,cam,v,&ex);
            if(ex||!boxed)return {};
            void*unboxed=a.object_unbox(boxed);
            if(!unboxed)return {};
            Vec3 vp{};
            std::memcpy(&vp,unboxed,sizeof(vp));
            if(!valid(vp))return {};
            return {viewport.x+vp.x*viewport.w,1.f-(viewport.y+vp.y*viewport.h),vp.z,true};
        };

        int ringCount=0;
        for(const auto&m:worldMarkers){
            if(!valid(m.anchor)||m.key=="@feet")continue;
            auto p=project(m.anchor);
            if(!p.ok)continue;

            EspItem item{};
            if(p.depth>0.01f&&p.x>=0.f&&p.x<=1.f&&p.y>=0.f&&p.y<=1.f){
                item.x=p.x;item.y=p.y;item.headY=p.y;
            }else if(m.key=="@maze_goal"||m.key=="@goal"||m.label=="ĐÍCH"){
                // Clamp off-screen Goal marker to screen border so ĐÍCH is always visible!
                float vx=p.x-0.5f,vy=p.y-0.5f;
                if(p.depth<=0.01f){vx=-vx;vy=-vy;}
                float scale=std::max(std::fabs(vx),std::fabs(vy));
                if(scale<1e-4f){vx=0.f;vy=-0.44f;}
                else{vx=(vx/scale)*0.44f;vy=(vy/scale)*0.44f;}
                item.x=std::clamp(0.5f+vx,0.06f,0.94f);
                item.y=std::clamp(0.5f+vy,0.08f,0.92f);
                item.headY=item.y;
            }else{
                continue;
            }

            item.color=m.color;item.selected=m.selected;
            item.colorKind=(m.color=="red")?1:((m.color=="yellow")?2:0);
            item.showDistance=false; // distance is already formatted in item.label if m.showDistance is true
            item.distance=valid(playerPos)?std::round(dist3D(m.anchor,playerPos)*10.f)/10.f:0.f;
            if(m.showDistance&&std::isfinite(item.distance)&&item.distance>=0.f&&!m.label.empty()){
                char buf[96];
                if(item.distance<10.f)std::snprintf(buf,sizeof(buf),"%s • %.1fm",m.label.c_str(),double(item.distance));
                else std::snprintf(buf,sizeof(buf),"%s • %.0fm",m.label.c_str(),double(item.distance));
                item.label=buf;
            }else{
                item.label=m.label;
            }
            if(m.showRing&&ringCount<256&&p.depth>0.01f&&p.x>=0.f&&p.x<=1.f&&p.y>=0.f&&p.y<=1.f){
                constexpr float radius=0.8f;
                bool ringOk=true;
                for(int i=0;i<12;++i){
                    float angle=float(i)*(3.1415926535f/6.f);
                    Vec3 rp{m.anchor.x+radius*std::cos(angle),m.anchor.y+0.035f,m.anchor.z+radius*std::sin(angle)};
                    auto qp=project(rp);
                    if(!qp.ok||qp.depth<=0.01f||std::fabs(qp.x)>8.f||std::fabs(qp.y)>8.f){ringOk=false;break;}
                    item.ring[std::size_t(i)]={qp.x,qp.y};
                }
                if(ringOk){item.hasRing=true;++ringCount;}
            }
            points.push_back(std::move(item));
        }

        if(routeNodes.size()>=2){
            std::vector<ProjectedPt> pts(routeNodes.size());
            for(std::size_t i=0;i<routeNodes.size();++i)pts[i]=project(routeNodes[i].anchor);
            constexpr float nearZ=0.01f;
            for(std::size_t i=1;i<routeNodes.size();++i){
                ProjectedPt aPt=pts[i-1],bPt=pts[i];
                if(!aPt.ok||!bPt.ok)continue;
                if(aPt.depth<nearZ&&bPt.depth<nearZ)continue;
                if(aPt.depth<nearZ||bPt.depth<nearZ){
                    float t=(nearZ-aPt.depth)/(bPt.depth-aPt.depth);
                    Vec3 wa=routeNodes[i-1].anchor,wb=routeNodes[i].anchor;
                    Vec3 cutW{wa.x+t*(wb.x-wa.x),wa.y+t*(wb.y-wa.y),wa.z+t*(wb.z-wa.z)};
                    ProjectedPt cut=project(cutW);
                    if(!cut.ok||cut.depth<=0.f)continue;
                    if(aPt.depth<nearZ)aPt=cut;else bPt=cut;
                }
                float dx=bPt.x-aPt.x,dy=bPt.y-aPt.y;
                float lo=0.f,hi=1.f;bool ok=true;
                const std::pair<float,float> edges[4]={{-dx,aPt.x},{dx,1.f-aPt.x},{-dy,aPt.y},{dy,1.f-aPt.y}};
                for(auto[p,q]:edges){
                    if(std::fabs(p)<1e-12f){if(q<0.f)ok=false;continue;}
                    float t=q/p;
                    if(p<0.f)lo=std::max(lo,t);else hi=std::min(hi,t);
                }
                if(!ok||lo>hi)continue;
                auto clamp01=[](float v){return std::clamp(v,0.f,1.f);};
                RouteSegment seg{};
                seg.a={clamp01(aPt.x+lo*dx),clamp01(aPt.y+lo*dy)};
                seg.b={clamp01(aPt.x+hi*dx),clamp01(aPt.y+hi*dy)};
                seg.x0=seg.a.x;seg.y0=seg.a.y;
                seg.x1=seg.b.x;seg.y1=seg.b.y;
                seg.drop=routeNodes[i].drop;
                seg.dashed=seg.drop;
                seg.currentStage=(routeNodes[i].stage==currentRouteStage);
                seg.colorKind=seg.drop?1:(seg.currentStage?0:2);
                routeSegments.push_back(seg);
            }
        }
    }

    void espTick(Obj actorObj,double time){
        Vec3 playerPos{};
        try{playerPos=position(actorObj);}catch(...){return;}
        if(time-espScanTime>=0.08){
            espScanTime=time;
            worldMarkers.clear();routeNodes.clear();
            Obj gameObj=findMiniGame(actorObj);
            if(!gameObj||!alive(gameObj)){
                currentMapName.clear();currentMapId=0;
                points.clear();routeSegments.clear();
                espSummary="ESP/Line: Đang ở sảnh ("+(controllerName.empty()?"ActorDefaultControlPlayer":controllerName)+") • Tự hiện khi vào Minigame";
                return;
            }
            currentMapName=classLabel(gameObj);
            currentMapId=getMiniGameId(gameObj);
            int battleState=getBattleState(gameObj);

            if(currentMapName=="MiniGameKMGRealBoard"||(currentMapName=="MiniGameKMGTwistRoad"&&currentMapId==120018)){
                collectRealBoardOrGlass(gameObj,currentMapId==120018&&currentMapName=="MiniGameKMGTwistRoad");
            }else if(currentMapName=="MiniGameKMGTwistRoad"&&currentMapId==120026){
                collectRoad120026(gameObj,battleState);
            }else if(currentMapName=="MiniGameKMGTwistRoad"&&currentMapId==120006){
                collectDoors120006(gameObj,battleState,playerPos);
            }else if(currentMapName=="MiniGameKMGTwistRoad"&&currentMapId==120015){
                collectHoles120015(gameObj,battleState,playerPos);
            }else if(currentMapName=="MiniGameKMGFogMaze"||currentMapId==120041){
                collectMaze120041(gameObj,battleState,playerPos);
            }else if(currentMapName=="MiniGameKMGDuel"||currentMapId==120019){
                std::uint8_t isAtt=0,canAtt=0,ended=0;std::uint32_t rnd=0;std::int32_t sc=0;
                readDuelState(gameObj,isAtt,canAtt,rnd,ended,sc);
                espSummary="Duel 120019: round "+std::to_string(rnd)+" • "+((isAtt==1&&canAtt==1)?"CÓ THỂ ĐÁNH":"chưa đến lượt")+" • điểm "+std::to_string(sc);
            }else{
                espSummary="Map hiện tại ("+currentMapName+" #"+std::to_string(currentMapId)+"): chờ map hỗ trợ";
            }
        }
        projectCurrentWorldToScreen(playerPos);
    }
};

GameBackend::GameBackend(Resolver r):state(std::make_unique<State>(std::move(r))){}
GameBackend::~GameBackend()=default;
const std::vector<EspItem>&GameBackend::esp()const{return state->points;}
const std::vector<RouteSegment>&GameBackend::route()const{return state->routeSegments;}
std::string GameBackend::diagnostic()const{
    auto&s=*state;
    return s.status+"\nController: "+(s.controllerName.empty()?"none":s.controllerName)+
        "\nMiniGame: "+(s.currentMapName.empty()?"none":s.currentMapName+" ("+std::to_string(s.currentMapId)+")")+
        "\nJump requests sent (not confirmed jumps): "+std::to_string(s.jumpRequests)+
        "\nESP markers: "+std::to_string(s.points.size())+" | Line segments: "+std::to_string(s.routeSegments.size())+
        "\nRuntime gate: "+(s.idle?"idle (no game calls)":"active");
}
void GameBackend::stop(Model&m){
    auto&s=*state;m.enabled.fill(false);s.points.clear();s.routeSegments.clear();
    s.clearCaches();s.resetFocus();s.espScanTime=-1;
    s.settleUntil=0;s.retryAfter=0;s.idle=true;
    m.status="Đã tắt ESP, Line, Spam nhảy, Tự sẵn sàng và Tự động búa";s.status=m.status;
}
void GameBackend::suspend(double time){
    auto&s=*state;s.points.clear();s.routeSegments.clear();
    s.clearCaches();s.resetFocus();s.espScanTime=-1;s.retryAfter=0;
    s.needsSettle=true;
    s.settleUntil=std::isfinite(time)?time+.75:0;
}
void GameBackend::toggle(Model&m,int id){
    if(id<0||id>=FeatureCount||!m.available[id]||!m.connected||!m.controlsAllowed)return;
    bool on=!m.enabled[id];m.enabled[id]=on;
    if(id==SpamJump){state->nextJump=0;state->lastJump=-1;if(!on)state->controlRoot.reset();}
    if(id==AutoReady){
        state->simpleLatched=false;state->partyLatched=false;state->readyScanTime=-1;
        if(!on){state->simpleDialogRoot.reset();state->partyDialogRoot.reset();state->readySummary.clear();}
    }
    if(id==AutoHammer){
        state->hammerWindowKey.clear();state->hammerFightTime=-1;state->hammerLatched=false;
        if(!on)state->hammerSummary.clear();
    }
    if(id==Esp&&!on)state->points.clear();
    if(id==Line&&!on)state->routeSegments.clear();
    state->settleUntil=0;
    const char*label=id==Esp?"ESP":(id==Line?"Line":(id==SpamJump?"Spam nhảy":(id==AutoReady?"Tự sẵn sàng":"Tự động búa")));
    m.toast=std::string(label)+(on?" • BẬT":" • TẮT");m.toastUntil=m.time+1.;
}
void GameBackend::tick(Model&m,double time){
    auto&s=*state;
    try{
        if(!m.connected||!m.controlsAllowed){
            if(!s.idle)stop(m);
            else{m.status="Chờ kết nối game";s.status=m.status;}
            return;
        }
        if(!std::isfinite(time)||time<0){s.points.clear();s.routeSegments.clear();m.status="Thời gian cập nhật không hợp lệ";return;}
        if(time<s.lastTick){s.nextJump=0;s.lastJump=-1;s.espScanTime=-1;s.settleUntil=0;s.retryAfter=0;}
        s.lastTick=time;
        if(!runtimeRequested(m)){
            s.points.clear();s.routeSegments.clear();
            if(!s.idle){s.clearCaches();s.resetFocus();s.espScanTime=-1;s.idle=true;m.player.clear();}
            s.status="Chờ bật ESP, Line, Spam nhảy hoặc Tự sẵn sàng";m.status=s.status;
            return;
        }
        if(!m.license.valid){
            s.points.clear();s.routeSegments.clear();
            s.status="Chờ kích hoạt Key bản quyền";
            m.status=s.status;
            return;
        }
        s.idle=false;
        if(time<s.retryAfter){m.status=s.status;return;}
        auto actorObj=s.actor();
        if(!s.alive(actorObj)){
            s.points.clear();s.routeSegments.clear();s.clearCaches();s.resetFocus();m.player.clear();
            s.needsSettle=true;
            s.status="Chờ nhân vật local";m.status=s.status;return;
        }
        auto uid=std::to_string(s.read<std::int64_t>(actorObj,s.field(s.a.object_get_class(actorObj),"<SUID>k__BackingField"),0x0a));
        if(actorObj!=s.actorRoot.get()||uid!=s.actorUID){
            const bool hadActor=s.actorRoot.get()!=nullptr;
            s.clearCaches();s.resetFocus();s.actorRoot.set(s.a,actorObj);s.actorUID=uid;
            if(hadActor||s.needsSettle){s.settleUntil=time+.75;s.needsSettle=false;}
        }
        m.player=uid;
        if(time<s.settleUntil){s.points.clear();s.routeSegments.clear();s.status="Đang chờ cảnh ổn định";m.status=s.status;return;}
        std::string detail;
        auto control=s.currentControl(actorObj);(void)control;
        if(m.enabled[AutoHammer]){
            std::string hammerNote;
            try{
                s.hammerTick(actorObj,time,m.hammerDelayMs,hammerNote);
                if(!hammerNote.empty())detail=hammerNote;
            }catch(const std::exception&e){
                detail=std::string("Tự động búa: ")+e.what();
            }
        }
        if(m.enabled[SpamJump]){
            std::string jumpNote;
            try{s.jumpTick(actorObj,time,m.jumpIntervalMs,jumpNote,m.jumpPressed,m.jumpButtonPos);}
            catch(const std::exception&e){s.nextJump=time+.25;jumpNote=std::string("Spam nhảy: ")+e.what();}
            if(!jumpNote.empty()){
                if(detail.empty()||detail.find("Chờ vào map Búa")!=std::string::npos||detail.find("Chờ map Búa")!=std::string::npos)detail=jumpNote;
                else if(jumpNote.find("ĐANG NHẢY")!=std::string::npos)detail+=" | "+jumpNote;
            }
        }else s.controlRoot.reset();
        if(m.enabled[AutoReady]){
            std::string readyNote;
            try{
                s.readyTick(time,readyNote);
                if(!readyNote.empty()){
                    if(detail.empty())detail=readyNote;
                    else if(readyNote.find("Đã bấm")!=std::string::npos||readyNote.find("Game báo")!=std::string::npos)detail+=" | "+readyNote;
                }
            }catch(const std::exception&e){
                if(!detail.empty())detail+=" | ";
                detail+=std::string("Tự sẵn sàng: ")+e.what();
            }
        }
        if(m.enabled[Esp]||m.enabled[Line]){
            try{
                s.espTick(actorObj,time);
                if(!m.enabled[Esp])s.points.clear();
                if(!m.enabled[Line])s.routeSegments.clear();
                if(!s.espSummary.empty()){
                    if(detail.empty()||detail=="Tự sẵn sàng: BẬT • Chờ bảng Sẵn sàng"||detail.find("Chờ vào map Búa")!=std::string::npos||detail.find("Chờ map Búa")!=std::string::npos)detail=s.espSummary;
                }
            }catch(const std::exception&e){
                s.points.clear();s.routeSegments.clear();s.needsSettle=true;
                if(!detail.empty())detail+=" | ";
                detail+=std::string("ESP/Line: ")+e.what();
            }
        }else{
            s.points.clear();s.routeSegments.clear();
        }
        s.retryAfter=0;
        m.status=detail.empty()?"ESP Minigame / Line / Tự nhảy / Tự sẵn sàng đang hoạt động":detail;s.status=m.status;
    }catch(const std::exception&e){
        s.points.clear();s.routeSegments.clear();s.clearCaches();s.resetFocus();m.player.clear();
        s.needsSettle=true;
        s.status=std::string("Chờ cảnh: ")+e.what();m.status=s.status;
        s.retryAfter=std::isfinite(time)?time+.75:0;
    }catch(...){
        s.points.clear();s.routeSegments.clear();s.clearCaches();s.resetFocus();m.player.clear();
        s.needsSettle=true;
        s.status="Backend lỗi không xác định";m.status=s.status;
        s.retryAfter=std::isfinite(time)?time+.75:0;
    }
}
}
