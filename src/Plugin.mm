#import <UIKit/UIKit.h>
#import <UIKit/UIGestureRecognizerSubclass.h>
#import <CommonCrypto/CommonDigest.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include "mobile_menu.hpp"
#include "game_backend.hpp"
#include "ios_build_guard.hpp"
#include "app_metadata_apple.hpp"
#include "generated_font.hpp"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <algorithm>
#include "imgui_impl_metal.h"
#include <cfloat>
#include <cmath>
#include <limits>

@interface NSBundle (VTBundleSpoof)
-(NSString*)vt_bundleIdentifier;
-(id)vt_objectForInfoDictionaryKey:(NSString*)key;
-(NSDictionary<NSString*,id>*)vt_infoDictionary;
@end

@implementation NSBundle (VTBundleSpoof)
-(NSString*)vt_bundleIdentifier{
    if(self==NSBundle.mainBundle)return @"com.vng.playtogether";
    return [self vt_bundleIdentifier];
}
-(id)vt_objectForInfoDictionaryKey:(NSString*)key{
    if(self==NSBundle.mainBundle&&[key isKindOfClass:NSString.class]){
        if([key isEqualToString:@"CFBundleIdentifier"])return @"com.vng.playtogether";
        if([key isEqualToString:@"NSAppTransportSecurity"])return @{@"NSAllowsArbitraryLoads":@YES};
    }
    return [self vt_objectForInfoDictionaryKey:key];
}
-(NSDictionary<NSString*,id>*)vt_infoDictionary{
    NSDictionary<NSString*,id>*dict=[self vt_infoDictionary];
    if(self==NSBundle.mainBundle&&dict){
        id cur=dict[@"CFBundleIdentifier"];
        id ats=dict[@"NSAppTransportSecurity"];
        BOOL needBundle=![cur isKindOfClass:NSString.class]||![(NSString*)cur isEqualToString:@"com.vng.playtogether"];
        BOOL needAts=![ats isKindOfClass:NSDictionary.class]||![ats[@"NSAllowsArbitraryLoads"] boolValue];
        if(needBundle||needAts){
            NSMutableDictionary<NSString*,id>*copy=[dict mutableCopy];
            copy[@"CFBundleIdentifier"]=@"com.vng.playtogether";
            copy[@"NSAppTransportSecurity"]=@{@"NSAllowsArbitraryLoads":@YES};
            return copy;
        }
    }
    return dict;
}
@end

static void VTSwizzleMethod(Class cls,SEL origSel,SEL swizSel){
    Method origMethod=class_getInstanceMethod(cls,origSel);
    Method swizMethod=class_getInstanceMethod(cls,swizSel);
    if(!origMethod||!swizMethod)return;
    if(class_addMethod(cls,origSel,method_getImplementation(swizMethod),method_getTypeEncoding(swizMethod))){
        class_replaceMethod(cls,swizSel,method_getImplementation(origMethod),method_getTypeEncoding(origMethod));
    }else{
        method_exchangeImplementations(origMethod,swizMethod);
    }
}

static void VTSpoofBundleIdentifier(){
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken,^{
        Class cls=NSBundle.class;
        VTSwizzleMethod(cls,@selector(bundleIdentifier),@selector(vt_bundleIdentifier));
        VTSwizzleMethod(cls,@selector(objectForInfoDictionaryKey:),@selector(vt_objectForInfoDictionaryKey:));
        VTSwizzleMethod(cls,@selector(infoDictionary),@selector(vt_infoDictionary));
        @try{
            CFBundleRef mainCF=CFBundleGetMainBundle();
            if(mainCF){
                CFDictionaryRef cfDict=CFBundleGetInfoDictionary(mainCF);
                if(cfDict){
                    NSDictionary*nsDict=(__bridge NSDictionary*)cfDict;
                    if([nsDict isKindOfClass:NSMutableDictionary.class]){
                        ((NSMutableDictionary*)nsDict)[@"CFBundleIdentifier"]=@"com.vng.playtogether";
                        ((NSMutableDictionary*)nsDict)[@"NSAppTransportSecurity"]=@{@"NSAllowsArbitraryLoads":@YES};
                    }
                }
            }
        }@catch(id){}
    });
}

static NSString* VTNormalizeLicenseKey(NSString*raw){
    if(!raw||raw.length==0)return @"";
    NSString*upper=raw.uppercaseString;
    upper=[upper stringByReplacingOccurrencesOfString:@"\u2010" withString:@"-"];
    upper=[upper stringByReplacingOccurrencesOfString:@"\u2011" withString:@"-"];
    upper=[upper stringByReplacingOccurrencesOfString:@"\u2012" withString:@"-"];
    upper=[upper stringByReplacingOccurrencesOfString:@"\u2013" withString:@"-"];
    upper=[upper stringByReplacingOccurrencesOfString:@"\u2014" withString:@"-"];
    upper=[upper stringByReplacingOccurrencesOfString:@"\u2015" withString:@"-"];
    upper=[upper stringByReplacingOccurrencesOfString:@"\u2212" withString:@"-"];

    NSRegularExpression*re=[NSRegularExpression regularExpressionWithPattern:@"KCT-[0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}" options:0 error:nil];
    NSTextCheckingResult*match=[re firstMatchInString:upper options:0 range:NSMakeRange(0,upper.length)];
    if(match&&match.range.location!=NSNotFound){
        return [upper substringWithRange:match.range];
    }

    // Also handle KCT followed by 32 hex chars without dashes or with spaces
    NSRegularExpression*reCompact=[NSRegularExpression regularExpressionWithPattern:@"KCT[\\s\\-_]*([0-9A-F]{8})[\\s\\-_]*([0-9A-F]{8})[\\s\\-_]*([0-9A-F]{8})[\\s\\-_]*([0-9A-F]{8})" options:0 error:nil];
    NSTextCheckingResult*m2=[reCompact firstMatchInString:upper options:0 range:NSMakeRange(0,upper.length)];
    if(m2&&m2.numberOfRanges==5){
        return [NSString stringWithFormat:@"KCT-%@-%@-%@-%@",
            [upper substringWithRange:[m2 rangeAtIndex:1]],
            [upper substringWithRange:[m2 rangeAtIndex:2]],
            [upper substringWithRange:[m2 rangeAtIndex:3]],
            [upper substringWithRange:[m2 rangeAtIndex:4]]];
    }

    NSMutableString*cleaned=[NSMutableString stringWithCapacity:upper.length];
    for(NSUInteger i=0;i<upper.length;i++){
        unichar c=[upper characterAtIndex:i];
        if((c>='0'&&c<='9')||(c>='A'&&c<='Z')||c=='-')[cleaned appendFormat:@"%C",c];
    }
    if(cleaned.length==32){
        BOOL allHex=YES;
        for(NSUInteger i=0;i<32;i++){
            unichar c=[cleaned characterAtIndex:i];
            if(!((c>='0'&&c<='9')||(c>='A'&&c<='F'))){allHex=NO;break;}
        }
        if(allHex){
            return [NSString stringWithFormat:@"KCT-%@-%@-%@-%@",
                [cleaned substringWithRange:NSMakeRange(0,8)],
                [cleaned substringWithRange:NSMakeRange(8,8)],
                [cleaned substringWithRange:NSMakeRange(16,8)],
                [cleaned substringWithRange:NSMakeRange(24,8)]];
        }
    }
    return cleaned;
}

static BOOL VTIsValidLicenseKeyFormat(NSString*key){
    if(!key||key.length!=39)return NO;
    NSRegularExpression*re=[NSRegularExpression regularExpressionWithPattern:@"^KCT-[0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}$" options:0 error:nil];
    return [re numberOfMatchesInString:key options:0 range:NSMakeRange(0,key.length)]==1;
}

// Direct POSIX TCP HTTP/1.1 client that completely bypasses iOS App Transport Security (ATS)
static NSData* VTHttpPostSocket(const char*host,const char*port,NSString*path,NSData*bodyData,NSInteger*outStatus,NSString**outErr){
    if(outStatus)*outStatus=0;
    if(outErr)*outErr=nil;
    struct addrinfo hints{};
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;
    hints.ai_protocol=IPPROTO_TCP;
    struct addrinfo*res=nullptr;
    int gai=getaddrinfo(host,port,&hints,&res);
    if(gai!=0||!res){
        if(outErr)*outErr=[NSString stringWithFormat:@"DNS/Addr error (%d)",gai];
        return nil;
    }
    int sock=-1;
    for(struct addrinfo*p=res;p!=nullptr;p=p->ai_next){
        int s=socket(p->ai_family,p->ai_socktype,p->ai_protocol);
        if(s<0)continue;
        int one=1;
        setsockopt(s,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
        int flags=fcntl(s,F_GETFL,0);
        if(flags>=0)fcntl(s,F_SETFL,flags|O_NONBLOCK);
        int rc=connect(s,p->ai_addr,p->ai_addrlen);
        if(rc<0&&errno==EINPROGRESS){
            fd_set wfds;FD_ZERO(&wfds);FD_SET(s,&wfds);
            struct timeval tv{6,0};
            if(select(s+1,nullptr,&wfds,nullptr,&tv)>0){
                int soErr=0;socklen_t slen=sizeof(soErr);
                if(getsockopt(s,SOL_SOCKET,SO_ERROR,&soErr,&slen)==0&&soErr==0)rc=0;
            }
        }
        if(rc==0){
            if(flags>=0)fcntl(s,F_SETFL,flags);
            struct timeval tv{6,0};
            setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
            setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
            sock=s;
            break;
        }
        close(s);
    }
    freeaddrinfo(res);
    if(sock<0){
        if(outErr)*outErr=@"Không kết nối được máy chủ bản quyền";
        return nil;
    }

    NSString*header=[NSString stringWithFormat:
        @"POST %@ HTTP/1.1\r\n"
        @"Host: %s:%s\r\n"
        @"Content-Type: application/json\r\n"
        @"Accept: application/json\r\n"
        @"Content-Length: %lu\r\n"
        @"Connection: close\r\n\r\n",
        path,host,port,(unsigned long)bodyData.length];
    NSData*headerData=[header dataUsingEncoding:NSUTF8StringEncoding];
    NSMutableData*reqData=[NSMutableData dataWithCapacity:headerData.length+bodyData.length];
    [reqData appendData:headerData];
    [reqData appendData:bodyData];

    const uint8_t*bytes=(const uint8_t*)reqData.bytes;
    size_t total=reqData.length,sent=0;
    while(sent<total){
        ssize_t n=send(sock,bytes+sent,total-sent,0);
        if(n<=0){close(sock);if(outErr)*outErr=@"Lỗi gửi dữ liệu tới máy chủ";return nil;}
        sent+=(size_t)n;
    }

    NSMutableData*respData=[NSMutableData dataWithCapacity:2048];
    uint8_t buf[2048];
    while(respData.length<65536){
        ssize_t n=recv(sock,buf,sizeof(buf),0);
        if(n<=0)break;
        [respData appendBytes:buf length:(NSUInteger)n];
    }
    close(sock);
    if(respData.length<12){
        if(outErr)*outErr=@"Máy chủ không phản hồi";
        return nil;
    }

    const char*rawBytes=(const char*)respData.bytes;
    size_t rawLen=respData.length;
    const char*sep=nullptr;
    for(size_t i=0;i+3<rawLen;++i){
        if(rawBytes[i]=='\r'&&rawBytes[i+1]=='\n'&&rawBytes[i+2]=='\r'&&rawBytes[i+3]=='\n'){
            sep=rawBytes+i;break;
        }
    }
    if(!sep){
        if(outErr)*outErr=@"Phản hồi HTTP không hợp lệ";
        return nil;
    }
    int status=0;
    sscanf(rawBytes,"HTTP/%*d.%*d %d",&status);
    if(outStatus)*outStatus=status;
    size_t bodyOffset=(size_t)(sep-rawBytes)+4;
    if(bodyOffset>=rawLen)return [NSData data];
    return [NSData dataWithBytes:rawBytes+bodyOffset length:rawLen-bodyOffset];
}

static void VTHttpPostJson(NSString*path,NSDictionary*payload,void(^completion)(NSInteger status,NSDictionary*json,NSString*errMsg)){
    NSData*bodyData=[NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
    if(!bodyData){
        dispatch_async(dispatch_get_main_queue(),^{completion(0,nil,@"Lỗi tạo JSON");});
        return;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
        NSInteger sockStatus=0;
        NSString*sockErr=nil;
        NSData*respBody=VTHttpPostSocket("45.131.65.244","30019",path,bodyData,&sockStatus,&sockErr);
        if(respBody&&sockStatus>0){
            id parsed=[NSJSONSerialization JSONObjectWithData:respBody options:0 error:nil];
            NSDictionary*dict=[parsed isKindOfClass:NSDictionary.class]?(NSDictionary*)parsed:nil;
            dispatch_async(dispatch_get_main_queue(),^{
                completion(sockStatus,dict,nil);
            });
            return;
        }
        // Fallback to NSURLSession if direct socket did not connect (e.g. system HTTP proxy)
        NSURL*url=[NSURL URLWithString:[NSString stringWithFormat:@"http://45.131.65.244:30019%@",path]];
        NSMutableURLRequest*req=[NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:8.0];
        req.HTTPMethod=@"POST";
        [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        req.HTTPBody=bodyData;
        NSURLSessionDataTask*task=[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData*data,NSURLResponse*response,NSError*error){
            dispatch_async(dispatch_get_main_queue(),^{
                if(error){
                    completion(0,nil,sockErr?sockErr:@"Không kết nối được máy chủ bản quyền");
                    return;
                }
                NSHTTPURLResponse*httpResp=(NSHTTPURLResponse*)response;
                id parsed=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
                NSDictionary*dict=[parsed isKindOfClass:NSDictionary.class]?(NSDictionary*)parsed:nil;
                completion(httpResp.statusCode,dict,nil);
            });
        }];
        [task resume];
    });
}

static void* VTUnityHandle=nullptr;
static std::string VTGuardMessage,VTGuardDetails;
static bool VTGuardMayRetry=true;
static bool VTCheckBuild(){
    VTGuardMayRetry=true;
    const auto metadata=vt::ios::readAppMetadata();
    const auto decision=vt::ios::chooseMetadata(metadata.disk,metadata.bundle);
    VTGuardDetails=vt::ios::metadataReport(metadata)+"\nMetadata source: "+decision.source;
    if(!decision.compatible){
        VTGuardMayRetry=decision.retryable;
        const auto& observed=metadata.disk.present?metadata.disk:metadata.bundle;
        VTGuardMessage=decision.retryable?"Waiting for app metadata":
            "App metadata mismatch: "+vt::ios::metadataLine("observed",observed);
        return false;
    }
    for(uint32_t i=0;i<_dyld_image_count();++i){
        const char*name=_dyld_get_image_name(i);if(!name||!strstr(name,"/UnityFramework.framework/UnityFramework"))continue;
        const auto*h=reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        if(!h||h->magic!=MH_MAGIC_64||h->cputype!=CPU_TYPE_ARM64||h->ncmds>4096||h->sizeofcmds>1048576){VTGuardMayRetry=false;VTGuardMessage="Invalid UnityFramework Mach-O header";return false;}
        const auto*base=reinterpret_cast<const unsigned char*>(h);const unsigned char*cmd=base+sizeof(*h),*end=cmd+h->sizeofcmds;
        std::string uuid;uint64_t textStart=0,textEnd=0;
        for(uint32_t j=0;j<h->ncmds;++j){
            if(cmd+sizeof(load_command)>end){VTGuardMayRetry=false;VTGuardMessage="Invalid Mach-O command table";return false;}
            const auto*lc=reinterpret_cast<const load_command*>(cmd);if(lc->cmdsize<sizeof(*lc)||cmd+lc->cmdsize>end){VTGuardMayRetry=false;VTGuardMessage="Invalid Mach-O command";return false;}
            if(lc->cmd==LC_UUID&&lc->cmdsize>=sizeof(uuid_command)){auto*u=reinterpret_cast<const uuid_command*>(cmd);char hex[33];for(int k=0;k<16;k++)snprintf(hex+k*2,3,"%02x",u->uuid[k]);uuid=hex;}
            if(lc->cmd==LC_SEGMENT_64&&lc->cmdsize>=sizeof(segment_command_64)){auto*seg=reinterpret_cast<const segment_command_64*>(cmd);if(strncmp(seg->segname,"__TEXT",16)==0){textStart=seg->vmaddr;textEnd=textStart+seg->vmsize;}}
            cmd+=lc->cmdsize;
        }
        VTGuardDetails+="\nUnityFramework UUID: "+uuid+"\nExpected UUID: "+vt::ios::ExpectedFrameworkUUID;
        if(uuid!=vt::ios::ExpectedFrameworkUUID){VTGuardMayRetry=false;VTGuardMessage="UnityFramework UUID mismatch";return false;}
        for(const auto&g:vt::ios::MethodGuards){
            if(g.rva<textStart||g.rva>textEnd||g.bytes.size()>textEnd-g.rva||memcmp(base+g.rva,g.bytes.data(),g.bytes.size())!=0){
                char detail[80];snprintf(detail,sizeof(detail),"iOS method signature mismatch at RVA 0x%llx",static_cast<unsigned long long>(g.rva));
                VTGuardMayRetry=false;VTGuardMessage=detail;return false;
            }
        }
        if(!VTUnityHandle)VTUnityHandle=dlopen(name,RTLD_LAZY|RTLD_NOLOAD);
        if(!VTUnityHandle){VTGuardMessage="UnityFramework is not loaded";return false;}
        VTGuardMessage="Installed app metadata + UUID + iOS method signatures confirmed";return true;
    }
    VTGuardMessage="Waiting for UnityFramework (will retry)";return false;
}

@class VTMenuController;
@class VTPassthroughView;

@interface VTMetalView : MTKView
@property(nonatomic,weak) id touchOwner;
@end

@interface VTPassthroughView : UIView
@property(nonatomic,weak) VTMenuController* menuController;
@end

// Plain NSObject controller (not a UIViewController) so its overlay view is a
// pure UIView with no _viewDelegate. This prevents UIViewControllerHierarchyInconsistency
// without creating any extra UIWindow that would interfere with PassSDK/login flows.
@interface VTMenuController : NSObject<MTKViewDelegate> {
    ImGuiContext* _context;
    vt::ios::Model _model;
    vt::ios::Actions _actions;
    std::unique_ptr<vt::ios::GameBackend> _backend;
    UITouch* _primaryTouch;
    CGPoint _touchStart,_touchPrevious;
    BOOL _panning;
    BOOL _draggingWindow;
    CFTimeInterval _lastFrame,_nextBuildCheck,_resumeAfter,_nextLicenseCheck;
    CGSize _lastLayoutSize;
    BOOL _isScreenCaptured;
    BOOL _keyAlertShowing;
    BOOL _activatingKey;
    UITextField* _secureField;
}
@property(nonatomic,strong) VTPassthroughView* view;
@property(nonatomic,strong) VTMetalView* metalView;
@property(nonatomic,strong) id<MTLDevice> device;
@property(nonatomic,strong) id<MTLCommandQueue> commandQueue;
@property(nonatomic,readonly) BOOL keyAlertShowing;
-(instancetype)initWithFrame:(CGRect)frame;
-(NSURL*)settingsURL;
-(void)restoreSettings;
-(void)saveSettings;
-(void)setupView;
-(void)beginTouches:(NSSet<UITouch*>*)touches;
-(void)moveTouches:(NSSet<UITouch*>*)touches;
-(void)endTouches:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled;
-(void)cancelMouse;
-(void)setRenderingActive:(BOOL)active;
-(BOOL)acceptsPoint:(CGPoint)point;
-(void)tryStartBackend:(CFTimeInterval)now;
-(void)handleLayoutChange:(CGSize)size;
-(void)setJumpHeld:(BOOL)held;
-(void)handleScreenCaptureChanged:(BOOL)captured;
-(void)updateAntiCaptureState;
-(void)activateKeyString:(NSString*)rawKey;
-(void)checkOrActivateKey:(NSString*)key token:(NSString*)token;
-(void)promptKeyInputAlert;
@end

@interface VTJumpGestureRecognizer : UIGestureRecognizer <UIGestureRecognizerDelegate>
@property(nonatomic,weak) VTMenuController* menuController;
@end

@implementation VTJumpGestureRecognizer {
    NSMutableSet<UITouch*>* _jumpTouches;
}
-(instancetype)init{
    if(self=[super initWithTarget:nil action:nil]){
        self.cancelsTouchesInView=NO;
        self.delaysTouchesBegan=NO;
        self.delaysTouchesEnded=NO;
        self.delegate=self;
        _jumpTouches=[NSMutableSet setWithCapacity:8];
    }
    return self;
}
-(BOOL)canPreventGestureRecognizer:(UIGestureRecognizer*)preventedGestureRecognizer{return NO;}
-(BOOL)canBePreventedByGestureRecognizer:(UIGestureRecognizer*)preventingGestureRecognizer{return NO;}
-(BOOL)gestureRecognizer:(UIGestureRecognizer*)gestureRecognizer shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer*)otherGestureRecognizer{return YES;}

-(BOOL)isJumpPoint:(CGPoint)p inView:(UIView*)view marginScale:(CGFloat)marginScale{
    if(!view)return NO;
    CGSize sz=view.bounds.size;
    if(sz.width<=0||sz.height<=0)return NO;

    // Never trigger jump when touching the VTMenu window or floating Menu button
    if(self.menuController&&self.menuController.metalView){
        CGPoint menuPt=[self.menuController.metalView convertPoint:p fromView:view];
        if([self.menuController acceptsPoint:menuPt])return NO;
    }

    UIEdgeInsets insets=view.safeAreaInsets;
    if(sz.width>=sz.height){ // Landscape: circular Jump button at bottom-right
        CGFloat usableW=std::max<CGFloat>(320.f,sz.width-insets.left-insets.right);
        CGFloat uiScale=std::clamp<CGFloat>(std::min(usableW/812.f,sz.height/375.f),0.85f,1.65f);
        CGFloat cx=sz.width-insets.right-88.f*uiScale;
        CGFloat cy=sz.height-insets.bottom*0.4f-78.f*uiScale;
        if(self.menuController){
            CGPoint norm=[self.menuController jumpButtonNormalizedCenter];
            if(norm.x>=0.68f&&norm.x<=0.98f&&norm.y>=0.58f&&norm.y<=0.98f){
                cx=norm.x*sz.width;
                cy=norm.y*sz.height;
            }
        }
        CGFloat radius=50.f*uiScale*marginScale;
        CGFloat dx=p.x-cx,dy=p.y-cy;
        return (dx*dx+dy*dy)<=(radius*radius);
    }else{ // Portrait: circular Jump button at bottom-right
        CGFloat uiScale=std::clamp<CGFloat>(sz.width/375.f,0.85f,1.5f);
        CGFloat cx=sz.width-insets.right-72.f*uiScale;
        CGFloat cy=sz.height-insets.bottom-86.f*uiScale;
        CGFloat radius=46.f*uiScale*marginScale;
        CGFloat dx=p.x-cx,dy=p.y-cy;
        return (dx*dx+dy*dy)<=(radius*radius);
    }
}
-(void)updateJumpState{
    BOOL held=(_jumpTouches.count>0);
    [self.menuController setJumpHeld:held];
}
-(void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{
    [super touchesBegan:touches withEvent:event];
    for(UITouch*t in touches){
        CGPoint p=[t locationInView:self.view];
        if([self isJumpPoint:p inView:self.view marginScale:1.0f]){
            [_jumpTouches addObject:t];
        }
    }
    [self updateJumpState];
}
-(void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{
    [super touchesMoved:touches withEvent:event];
    // Never add new touches in touchesMoved (prevents camera rotation drags from triggering jump).
    // Only remove a held jump touch if the finger slides off the Jump button circle.
    for(UITouch*t in touches){
        if([_jumpTouches containsObject:t]){
            CGPoint p=[t locationInView:self.view];
            if(![self isJumpPoint:p inView:self.view marginScale:1.28f]){
                [_jumpTouches removeObject:t];
            }
        }
    }
    [self updateJumpState];
}
-(void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{
    [super touchesEnded:touches withEvent:event];
    for(UITouch*t in touches){
        [_jumpTouches removeObject:t];
    }
    [self updateJumpState];
}
-(void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{
    [super touchesCancelled:touches withEvent:event];
    for(UITouch*t in touches){
        [_jumpTouches removeObject:t];
    }
    [self updateJumpState];
}
-(void)reset{
    [super reset];
    [_jumpTouches removeAllObjects];
    [self updateJumpState];
}
@end

@implementation VTPassthroughView
-(UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event{
    if(self.hidden||self.alpha<.01||!self.userInteractionEnabled||![self pointInside:point withEvent:event]||!self.menuController.metalView)return nil;
    if(self.menuController.keyAlertShowing)return nil;
    CGPoint local=[self.menuController.metalView convertPoint:point fromView:self];
    if(![self.menuController acceptsPoint:local])return nil;
    return self.menuController.metalView;
}
-(void)layoutSubviews{
    [super layoutSubviews];
    [self.menuController handleLayoutChange:self.bounds.size];
}
@end

@implementation VTMetalView
-(void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{[(VTMenuController*)self.touchOwner beginTouches:touches];}
-(void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{[(VTMenuController*)self.touchOwner moveTouches:touches];}
-(void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{[(VTMenuController*)self.touchOwner endTouches:touches cancelled:NO];}
-(void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{[(VTMenuController*)self.touchOwner endTouches:touches cancelled:YES];}
@end

@implementation VTMenuController
-(BOOL)keyAlertShowing{return _keyAlertShowing;}
-(void)setJumpHeld:(BOOL)held{
    _model.jumpPressed=(held==YES);
}
-(CGPoint)jumpButtonNormalizedCenter{
    return CGPointMake(_model.jumpButtonPos.x,_model.jumpButtonPos.y);
}
-(UIView*)resolveSecureCanvas{
    if(!_secureField)return nil;
    [_secureField setNeedsLayout];
    [_secureField layoutIfNeeded];
    for(UIView*sub in _secureField.subviews){
        NSString*cls=NSStringFromClass(sub.class);
        if([cls containsString:@"TextLayoutCanvasView"]||[cls containsString:@"CanvasView"]){
            return sub;
        }
    }
    return _secureField.subviews.firstObject;
}
-(void)updateAntiCaptureState{
    if(_secureField){
        _secureField.secureTextEntry=_model.antiCapture?YES:NO;
        UIView*secureCanvas=[self resolveSecureCanvas];
        UIView*targetHost=(_model.antiCapture&&secureCanvas)?secureCanvas:self.view;
        targetHost.backgroundColor=UIColor.clearColor;
        targetHost.opaque=NO;
        targetHost.clipsToBounds=NO;
        targetHost.layer.masksToBounds=NO;
        if(self.metalView&&self.metalView.superview!=targetHost){
            [self.metalView removeFromSuperview];
            [targetHost addSubview:self.metalView];
            UIView*g=self.view;
            [NSLayoutConstraint activateConstraints:@[
                [self.metalView.leadingAnchor constraintEqualToAnchor:g.leadingAnchor],
                [self.metalView.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],
                [self.metalView.topAnchor constraintEqualToAnchor:g.topAnchor],
                [self.metalView.bottomAnchor constraintEqualToAnchor:g.bottomAnchor]
            ]];
        }
    }
    // Keep metalView ALWAYS visible on the physical screen!
    // _secureField.secureTextEntry = YES tells iOS WindowServer to exclude the secure canvas
    // from screen recordings/screenshots while still displaying it to the user's eyes.
    if(self.metalView){
        self.metalView.hidden=NO;
    }
}
-(void)handleScreenCaptureChanged:(BOOL)captured{
    _isScreenCaptured=captured;
    [self updateAntiCaptureState];
}
-(NSString*)sha256Hex:(NSString*)input{
    const char*str=[input UTF8String];
    if(!str)str="";
    unsigned char result[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(str,(CC_LONG)strlen(str),result);
    NSMutableString*hex=[NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH*2];
    for(int i=0;i<CC_SHA256_DIGEST_LENGTH;i++){
        [hex appendFormat:@"%02x",result[i]];
    }
    return hex;
}
-(NSString*)randomHex64{
    unsigned char bytes[32];
    arc4random_buf(bytes,sizeof(bytes));
    NSMutableString*hex=[NSMutableString stringWithCapacity:64];
    for(int i=0;i<32;i++)[hex appendFormat:@"%02x",bytes[i]];
    return hex;
}
-(NSString*)deviceIdentifier{
    NSString*vendor=UIDevice.currentDevice.identifierForVendor.UUIDString;
    if(!vendor||vendor.length==0)vendor=@"00000000-0000-0000-0000-000000000000";
    return [self sha256Hex:[NSString stringWithFormat:@"keydesk-device-v1:%@",vendor.lowercaseString]];
}
-(void)promptKeyInputAlert{
    if(_keyAlertShowing)return;
    [self cancelMouse];
    _primaryTouch=nil;_panning=NO;_draggingWindow=NO;
    __weak VTMenuController*weakSelf=self;
    dispatch_async(dispatch_get_main_queue(),^{
        VTMenuController*strong=weakSelf;
        if(!strong||strong->_keyAlertShowing)return;
        UIWindow*hostWin=strong.view.window;
        if(!hostWin){
            for(UIScene*scene in UIApplication.sharedApplication.connectedScenes){
                if(![scene isKindOfClass:UIWindowScene.class])continue;
                for(UIWindow*w in ((UIWindowScene*)scene).windows){
                    if(w.isKeyWindow){hostWin=w;break;}
                }
                if(hostWin)break;
            }
        }
        UIViewController*topVC=hostWin.rootViewController;
        while(topVC.presentedViewController&&!topVC.presentedViewController.isBeingDismissed){
            topVC=topVC.presentedViewController;
        }
        if(!topVC||[topVC isKindOfClass:UIAlertController.class])return;

        strong->_keyAlertShowing=YES;
        UIAlertController*alert=[UIAlertController alertControllerWithTitle:@"Nhập Key Bản Quyền"
                                                                    message:@"Nhập hoặc dán mã Key KCT-XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX"
                                                             preferredStyle:UIAlertControllerStyleAlert];
        [alert addTextFieldWithConfigurationHandler:^(UITextField*textField){
            VTMenuController*s=weakSelf;
            textField.placeholder=@"KCT-XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX";
            textField.autocapitalizationType=UITextAutocapitalizationTypeAllCharacters;
            textField.autocorrectionType=UITextAutocorrectionTypeNo;
            textField.clearButtonMode=UITextFieldViewModeWhileEditing;
            if(s&&s->_model.keyInput[0]!='\0'){
                textField.text=[NSString stringWithUTF8String:s->_model.keyInput];
            }else{
                NSString*clip=VTNormalizeLicenseKey(UIPasteboard.generalPasteboard.string);
                if(VTIsValidLicenseKeyFormat(clip)){
                    textField.text=clip;
                }
            }
        }];
        [alert addAction:[UIAlertAction actionWithTitle:@"Dán & Kích hoạt" style:UIAlertActionStyleDefault handler:^(UIAlertAction*action){
            VTMenuController*s=weakSelf;
            if(!s)return;
            s->_keyAlertShowing=NO;
            NSString*entered=alert.textFields.firstObject.text;
            NSString*clip=UIPasteboard.generalPasteboard.string;
            NSString*candidate=(clip&&clip.length>0)?clip:entered;
            [s activateKeyString:candidate];
        }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Kích hoạt" style:UIAlertActionStyleDefault handler:^(UIAlertAction*action){
            VTMenuController*s=weakSelf;
            if(!s)return;
            s->_keyAlertShowing=NO;
            NSString*entered=alert.textFields.firstObject.text;
            [s activateKeyString:entered];
        }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Hủy" style:UIAlertActionStyleCancel handler:^(UIAlertAction*action){
            VTMenuController*s=weakSelf;
            if(s)s->_keyAlertShowing=NO;
        }]];
        [topVC presentViewController:alert animated:YES completion:nil];
    });
}
-(void)handleLicenseSuccessWithKey:(NSString*)cleanKey json:(NSDictionary*)json verifiedByServer:(BOOL)verifiedOnly silent:(BOOL)silent{
    _activatingKey=NO;
    _model.license.valid=true;
    _model.license.key=cleanKey.UTF8String;
    strncpy(_model.keyInput,cleanKey.UTF8String,sizeof(_model.keyInput)-1);
    _model.keyInput[sizeof(_model.keyInput)-1]='\0';
    if([json[@"token"] isKindOfClass:NSString.class])_model.license.token=[json[@"token"] UTF8String];
    if([json[@"expires_at"] isKindOfClass:NSNumber.class])_model.license.expiresAt=[json[@"expires_at"] longLongValue];
    if([json[@"remaining_seconds"] isKindOfClass:NSNumber.class]){
        _model.license.remainingSeconds=[json[@"remaining_seconds"] longLongValue];
        long long rem=_model.license.remainingSeconds;
        long days=(long)(rem/86400);
        long hours=(long)((rem%86400)/3600);
        long mins=(long)((rem%3600)/60);
        if(days>0)_model.license.statusText=[NSString stringWithFormat:@"Key Hợp Lệ • Còn %ld ngày %ld giờ",days,hours].UTF8String;
        else _model.license.statusText=[NSString stringWithFormat:@"Key Hợp Lệ • Còn %ld giờ %ld phút",hours,mins].UTF8String;
    }else if(_model.license.statusText.empty()||!silent){
        if(verifiedOnly)_model.license.statusText="Key Hợp Lệ • Đang hoạt động";
        else _model.license.statusText="Key Hợp Lệ • Đang hoạt động";
    }
    if(!silent){
        _model.toast="Đã kích hoạt Key thành công!";
        _model.toastUntil=_model.time+2.5;
    }
    _nextLicenseCheck=CACurrentMediaTime()+1800.0;
    [self saveSettings];
}
-(void)handleLicenseFailureWithStatus:(NSInteger)status json:(NSDictionary*)json errMsg:(NSString*)errMsg{
    _activatingKey=NO;
    _model.license.valid=false;
    for(int i=0;i<vt::FeatureCount;++i)_model.enabled[i]=false;
    if(_backend)_backend->stop(_model);
    if(errMsg&&status==0){
        _model.license.statusText=[NSString stringWithFormat:@"Lỗi mạng: %@",errMsg].UTF8String;
        _model.toast="Không kết nối được máy chủ Key!";
        _model.toastUntil=_model.time+2.0;
        return;
    }
    NSString*code=([json isKindOfClass:NSDictionary.class]&&[json[@"error"] isKindOfClass:NSString.class])?json[@"error"]:@"";
    NSString*reason=([json isKindOfClass:NSDictionary.class]&&[json[@"reason"] isKindOfClass:NSString.class])?json[@"reason"]:@"";
    NSString*vnMsg=nil;
    if([code isEqualToString:@"invalid_license"]){
        vnMsg=@"Key không đúng hoặc không tồn tại trên hệ thống";
    }else if([code isEqualToString:@"expired"]){
        vnMsg=@"Key đã hết hạn sử dụng";
    }else if([code isEqualToString:@"revoked"]){
        vnMsg=@"Key đã bị Admin thu hồi";
    }else if([code isEqualToString:@"device_limit"]){
        vnMsg=@"Key đã đạt giới hạn thiết bị (cần gỡ máy cũ)";
    }else if([code isEqualToString:@"application_disabled"]||[code isEqualToString:@"build_disabled"]){
        vnMsg=reason.length>0?reason:@"Ứng dụng/phiên bản đang bị Admin khóa";
    }else if([code isEqualToString:@"rate_limited"]){
        vnMsg=@"Thao tác quá nhanh, vui lòng thử lại sau ít giây";
    }else if(code.length>0){
        vnMsg=[NSString stringWithFormat:@"Từ chối kích hoạt: %@",code];
    }else{
        vnMsg=[NSString stringWithFormat:@"Máy chủ từ chối (HTTP %ld)",(long)status];
    }
    _model.license.statusText=vnMsg.UTF8String;
    _model.toast=vnMsg.UTF8String;
    _model.toastUntil=_model.time+2.5;
    [self saveSettings];
}
-(void)activateKeyString:(NSString*)rawKey{
    if(_activatingKey)return;
    NSString*clean=VTNormalizeLicenseKey(rawKey);
    if(clean.length==0){
        clean=VTNormalizeLicenseKey(UIPasteboard.generalPasteboard.string);
    }
    if(clean.length==0){
        [self promptKeyInputAlert];
        return;
    }
    strncpy(_model.keyInput,clean.UTF8String,sizeof(_model.keyInput)-1);
    _model.keyInput[sizeof(_model.keyInput)-1]='\0';
    if(!VTIsValidLicenseKeyFormat(clean)){
        _model.license.valid=false;
        _model.license.statusText="Sai định dạng Key (cần KCT-XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX)";
        _model.toast="Sai định dạng Key!";
        _model.toastUntil=_model.time+2.0;
        return;
    }

    _activatingKey=YES;
    _model.license.statusText="Đang xác thực Key bản quyền...";
    _model.toast="Đang kiểm tra Key...";
    _model.toastUntil=_model.time+2.0;

    NSString*devId=[self deviceIdentifier];
    NSDictionary*payload1=@{
        @"key":clean,
        @"device_id":devId,
        @"nonce":[self randomHex64],
        @"security_protocol":@2,
        @"build_id":@"U4.37",
        @"image_sha256":@"d124204d2a18c6516d0fcaa05ca3fd3bf8d98ff1a4188c6ade39871c92a96350"
    };

    __weak VTMenuController*weakSelf=self;
    VTHttpPostJson(@"/api/v1/activate",payload1,^(NSInteger status,NSDictionary*json,NSString*errMsg){
        VTMenuController*strong=weakSelf;
        if(!strong)return;
        if([json isKindOfClass:NSDictionary.class]&&[json[@"ok"] boolValue]){
            [strong handleLicenseSuccessWithKey:clean json:json verifiedByServer:NO silent:NO];
            return;
        }
        NSString*code=([json isKindOfClass:NSDictionary.class]&&[json[@"error"] isKindOfClass:NSString.class])?json[@"error"]:@"";
        if([code isEqualToString:@"release_not_approved"]){
            NSDictionary*payload2=@{
                @"key":clean,
                @"device_id":devId,
                @"nonce":[strong randomHex64],
                @"security_protocol":@2,
                @"build_id":@"U4.36",
                @"image_sha256":@"d124204d2a18c6516d0fcaa05ca3fd3bf8d98ff1a4188c6ade39871c92a96350"
            };
            VTHttpPostJson(@"/api/v1/activate",payload2,^(NSInteger status2,NSDictionary*json2,NSString*errMsg2){
                VTMenuController*s2=weakSelf;
                if(!s2)return;
                if([json2 isKindOfClass:NSDictionary.class]&&[json2[@"ok"] boolValue]){
                    [s2 handleLicenseSuccessWithKey:clean json:json2 verifiedByServer:NO silent:NO];
                    return;
                }
                NSString*code2=([json2 isKindOfClass:NSDictionary.class]&&[json2[@"error"] isKindOfClass:NSString.class])?json2[@"error"]:@"";
                if([code2 isEqualToString:@"release_not_approved"]){
                    [s2 handleLicenseSuccessWithKey:clean json:json2 verifiedByServer:YES silent:NO];
                    return;
                }
                [s2 handleLicenseFailureWithStatus:status2 json:json2 errMsg:errMsg2];
            });
            return;
        }
        [strong handleLicenseFailureWithStatus:status json:json errMsg:errMsg];
    });
}
-(void)checkOrActivateKey:(NSString*)key token:(NSString*)token{
    NSString*cleanKey=VTNormalizeLicenseKey(key);
    if(!VTIsValidLicenseKeyFormat(cleanKey))return;
    // If key was not yet marked valid in saved config, perform normal activation once
    if(!_model.license.valid){
        [self activateKeyString:cleanKey];
        return;
    }
    // Silent background check: NEVER show activation toast and NEVER disable features
    // unless the server explicitly reports the key is expired, revoked, or invalid.
    _nextLicenseCheck=CACurrentMediaTime()+1800.0;
    NSString*devId=[self deviceIdentifier];
    __weak VTMenuController*weakSelf=self;
    if(token&&token.length>=40){
        NSDictionary*payload=@{
            @"token":token,
            @"device_id":devId,
            @"nonce":[self randomHex64],
            @"security_protocol":@2,
            @"build_id":@"U4.37",
            @"image_sha256":@"d124204d2a18c6516d0fcaa05ca3fd3bf8d98ff1a4188c6ade39871c92a96350"
        };
        VTHttpPostJson(@"/api/v1/check",payload,^(NSInteger status,NSDictionary*json,NSString*errMsg){
            VTMenuController*strong=weakSelf;
            if(!strong||status==0)return;
            if([json isKindOfClass:NSDictionary.class]&&[json[@"ok"] boolValue]){
                [strong handleLicenseSuccessWithKey:cleanKey json:json verifiedByServer:NO silent:YES];
                return;
            }
            NSString*code=([json isKindOfClass:NSDictionary.class]&&[json[@"error"] isKindOfClass:NSString.class])?json[@"error"]:@"";
            if([code isEqualToString:@"invalid_license"]||[code isEqualToString:@"expired"]||[code isEqualToString:@"revoked"]){
                [strong handleLicenseFailureWithStatus:status json:json errMsg:errMsg];
            }
        });
    }else{
        NSDictionary*payload=@{
            @"key":cleanKey,
            @"device_id":devId,
            @"nonce":[self randomHex64],
            @"security_protocol":@2,
            @"build_id":@"U4.37",
            @"image_sha256":@"d124204d2a18c6516d0fcaa05ca3fd3bf8d98ff1a4188c6ade39871c92a96350"
        };
        VTHttpPostJson(@"/api/v1/activate",payload,^(NSInteger status,NSDictionary*json,NSString*errMsg){
            VTMenuController*strong=weakSelf;
            if(!strong||status==0)return;
            if([json isKindOfClass:NSDictionary.class]&&[json[@"ok"] boolValue]){
                [strong handleLicenseSuccessWithKey:cleanKey json:json verifiedByServer:NO silent:YES];
                return;
            }
            NSString*code=([json isKindOfClass:NSDictionary.class]&&[json[@"error"] isKindOfClass:NSString.class])?json[@"error"]:@"";
            if([code isEqualToString:@"release_not_approved"]){
                [strong handleLicenseSuccessWithKey:cleanKey json:json verifiedByServer:YES silent:YES];
                return;
            }
            if([code isEqualToString:@"invalid_license"]||[code isEqualToString:@"expired"]||[code isEqualToString:@"revoked"]){
                [strong handleLicenseFailureWithStatus:status json:json errMsg:errMsg];
            }
        });
    }
}

-(instancetype)initWithFrame:(CGRect)frame{
    self=[super init];
    if(!self)return nil;
    VTPassthroughView*root=[[VTPassthroughView alloc]initWithFrame:frame];
    root.menuController=self;
    root.backgroundColor=UIColor.clearColor;
    root.opaque=NO;
    root.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    self.view=root;
    [self setupView];
    return self;
}
-(NSURL*)settingsURL{
    NSURL*base=[[NSFileManager defaultManager]URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL*folder=[base URLByAppendingPathComponent:@"VTMenu" isDirectory:YES];
    [[NSFileManager defaultManager]createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
    return [folder URLByAppendingPathComponent:@"config.json"];
}
-(void)restoreSettings{
    NSData*data=[NSData dataWithContentsOfURL:[self settingsURL]];
    id parsed=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
    NSDictionary*saved=[parsed isKindOfClass:NSDictionary.class]?parsed:@{};
    if([saved[@"scale"] isKindOfClass:NSNumber.class])_model.scale=[saved[@"scale"] floatValue];
    if([saved[@"opacity"] isKindOfClass:NSNumber.class])_model.opacity=[saved[@"opacity"] floatValue];
    if([saved[@"jump_interval_ms"] isKindOfClass:NSNumber.class])_model.jumpIntervalMs=[saved[@"jump_interval_ms"] doubleValue];
    if([saved[@"hammer_delay_ms"] isKindOfClass:NSNumber.class])_model.hammerDelayMs=[saved[@"hammer_delay_ms"] doubleValue];
    if([saved[@"anti_capture"] isKindOfClass:NSNumber.class])_model.antiCapture=[saved[@"anti_capture"] boolValue];
    if([saved[@"floating_btn_x"] isKindOfClass:NSNumber.class]&&[saved[@"floating_btn_y"] isKindOfClass:NSNumber.class]){
        _model.floatingButtonPos={[saved[@"floating_btn_x"] floatValue],[saved[@"floating_btn_y"] floatValue]};
    }
    if([saved[@"menu_win_x"] isKindOfClass:NSNumber.class]&&[saved[@"menu_win_y"] isKindOfClass:NSNumber.class]){
        _model.menuWindowPos={[saved[@"menu_win_x"] floatValue],[saved[@"menu_win_y"] floatValue]};
    }
    if([saved[@"license_valid"] isKindOfClass:NSNumber.class])_model.license.valid=[saved[@"license_valid"] boolValue];
    if([saved[@"license_key"] isKindOfClass:NSString.class]){
        _model.license.key=[saved[@"license_key"] UTF8String];
        strncpy(_model.keyInput,_model.license.key.c_str(),sizeof(_model.keyInput)-1);
        _model.keyInput[sizeof(_model.keyInput)-1]='\0';
    }
    if([saved[@"license_token"] isKindOfClass:NSString.class])_model.license.token=[saved[@"license_token"] UTF8String];
    if([saved[@"license_expires"] isKindOfClass:NSNumber.class])_model.license.expiresAt=[saved[@"license_expires"] longLongValue];
    if([saved[@"license_remaining"] isKindOfClass:NSNumber.class])_model.license.remainingSeconds=[saved[@"license_remaining"] longLongValue];
    if([saved[@"license_status"] isKindOfClass:NSString.class])_model.license.statusText=[saved[@"license_status"] UTF8String];

    vt::ios::sanitize(_model);_model.enabled.fill(false);_model.visible=false;_model.jumpPressed=false;
    if(_model.license.valid){
        if([saved[@"enabled_esp"] isKindOfClass:NSNumber.class])_model.enabled[vt::Esp]=[saved[@"enabled_esp"] boolValue];
        if([saved[@"enabled_line"] isKindOfClass:NSNumber.class])_model.enabled[vt::Line]=[saved[@"enabled_line"] boolValue];
        if([saved[@"enabled_spam_jump"] isKindOfClass:NSNumber.class])_model.enabled[vt::SpamJump]=[saved[@"enabled_spam_jump"] boolValue];
        if([saved[@"enabled_auto_ready"] isKindOfClass:NSNumber.class])_model.enabled[vt::AutoReady]=[saved[@"enabled_auto_ready"] boolValue];
        if([saved[@"enabled_auto_hammer"] isKindOfClass:NSNumber.class])_model.enabled[vt::AutoHammer]=[saved[@"enabled_auto_hammer"] boolValue];
    }
    _nextLicenseCheck=CACurrentMediaTime()+1800.0;
    if(!_model.license.key.empty()){
        NSString*savedKey=[NSString stringWithUTF8String:_model.license.key.c_str()];
        NSString*savedToken=[NSString stringWithUTF8String:_model.license.token.c_str()];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(2.0*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
            [self checkOrActivateKey:savedKey token:savedToken];
        });
    }
}
-(void)saveSettings{
    vt::ios::sanitize(_model);
    NSDictionary*settings=@{
        @"schema":@6,
        @"scale":@(_model.scale),
        @"opacity":@(_model.opacity),
        @"jump_interval_ms":@(_model.jumpIntervalMs),
        @"hammer_delay_ms":@(_model.hammerDelayMs),
        @"anti_capture":@(_model.antiCapture),
        @"floating_btn_x":@(_model.floatingButtonPos.x),
        @"floating_btn_y":@(_model.floatingButtonPos.y),
        @"menu_win_x":@(_model.menuWindowPos.x),
        @"menu_win_y":@(_model.menuWindowPos.y),
        @"enabled_esp":@(_model.enabled[vt::Esp]),
        @"enabled_line":@(_model.enabled[vt::Line]),
        @"enabled_spam_jump":@(_model.enabled[vt::SpamJump]),
        @"enabled_auto_ready":@(_model.enabled[vt::AutoReady]),
        @"enabled_auto_hammer":@(_model.enabled[vt::AutoHammer]),
        @"license_valid":@(_model.license.valid),
        @"license_key":[NSString stringWithUTF8String:_model.license.key.c_str()],
        @"license_token":[NSString stringWithUTF8String:_model.license.token.c_str()],
        @"license_expires":@(_model.license.expiresAt),
        @"license_remaining":@(_model.license.remainingSeconds),
        @"license_status":[NSString stringWithUTF8String:_model.license.statusText.c_str()]
    };
    NSData*data=[NSJSONSerialization dataWithJSONObject:settings options:NSJSONWritingPrettyPrinted error:nil];
    NSError*error=nil;
    if(!data||![data writeToURL:[self settingsURL] options:NSDataWritingAtomic error:&error])_model.status="Không ghi được config.json";
}
-(BOOL)acceptsPoint:(CGPoint)p{
    return vt::ios::acceptsPoint(_model,{float(p.x),float(p.y)});
}
-(void)setupView{
    [self restoreSettings];self.device=MTLCreateSystemDefaultDevice();
    if(!self.device){UILabel*label=[[UILabel alloc]initWithFrame:self.view.bounds];label.text=@"Thiết bị không khởi tạo được Metal.";label.textColor=UIColor.whiteColor;label.numberOfLines=0;label.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:label];return;}
    self.commandQueue=[self.device newCommandQueue];
    self.metalView=[[VTMetalView alloc]initWithFrame:self.view.bounds device:self.device];self.metalView.delegate=self;self.metalView.touchOwner=self;
    self.metalView.multipleTouchEnabled=YES;self.metalView.preferredFramesPerSecond=60;
    self.metalView.colorPixelFormat=MTLPixelFormatBGRA8Unorm;self.metalView.opaque=NO;self.metalView.layer.opaque=NO;self.metalView.backgroundColor=UIColor.clearColor;self.metalView.clearColor=MTLClearColorMake(0,0,0,0);
    self.metalView.translatesAutoresizingMaskIntoConstraints=NO;

    // Secure text field canvas container hides menu & ESP from screenshots and screen recordings
    // while keeping them 100% visible on the physical screen.
    _secureField=[[UITextField alloc]initWithFrame:self.view.bounds];
    _secureField.secureTextEntry=_model.antiCapture?YES:NO;
    _secureField.userInteractionEnabled=NO;
    _secureField.backgroundColor=UIColor.clearColor;
    _secureField.opaque=NO;
    _secureField.clipsToBounds=NO;
    _secureField.layer.masksToBounds=NO;
    _secureField.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:_secureField];
    UIView*secureCanvas=[self resolveSecureCanvas];
    UIView*metalHost=(_model.antiCapture&&secureCanvas)?secureCanvas:self.view;
    metalHost.backgroundColor=UIColor.clearColor;
    metalHost.opaque=NO;
    metalHost.clipsToBounds=NO;
    metalHost.layer.masksToBounds=NO;
    [metalHost addSubview:self.metalView];

    UIView*g=self.view;
    [NSLayoutConstraint activateConstraints:@[
        [_secureField.leadingAnchor constraintEqualToAnchor:g.leadingAnchor],
        [_secureField.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],
        [_secureField.topAnchor constraintEqualToAnchor:g.topAnchor],
        [_secureField.bottomAnchor constraintEqualToAnchor:g.bottomAnchor],
        [self.metalView.leadingAnchor constraintEqualToAnchor:g.leadingAnchor],
        [self.metalView.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],
        [self.metalView.topAnchor constraintEqualToAnchor:g.topAnchor],
        [self.metalView.bottomAnchor constraintEqualToAnchor:g.bottomAnchor]
    ]];
    [self updateAntiCaptureState];

    IMGUI_CHECKVERSION();_context=ImGui::CreateContext();ImGuiIO&io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
    CGFloat scale=UIScreen.mainScreen.scale;ImFontConfig cfg;cfg.OversampleH=2;cfg.OversampleV=1;cfg.FontDataOwnedByAtlas=false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(vt::ios::FontBytes),int(sizeof(vt::ios::FontBytes)),16.f*scale,&cfg,io.Fonts->GetGlyphRangesVietnamese());
    if(io.Fonts->Fonts.empty())io.Fonts->AddFontDefault();
    // Rasterize at Retina scale, then return glyph geometry to UIKit points.
    for(ImFont*f:io.Fonts->Fonts)f->Scale=1.f/float(scale);
    vt::ios::style();ImGui_ImplMetal_Init(self.device);
    _model.available.fill(false);
    for(int id:{vt::Esp,vt::Line,vt::SpamJump,vt::AutoReady,vt::AutoHammer})_model.available[id]=true;
    _resumeAfter=0;
    _nextBuildCheck=0;
    _model.status="Mở menu sau khi đăng nhập để kết nối";
    __weak VTMenuController*weakSelf=self;
    _actions.save=[weakSelf]{VTMenuController*strong=weakSelf;if(strong)[strong saveSettings];};
    _actions.stop=[weakSelf]{VTMenuController*strong=weakSelf;if(strong){if(strong->_backend)strong->_backend->stop(strong->_model);[strong saveSettings];}};
    _actions.number=[weakSelf](int,double){VTMenuController*strong=weakSelf;if(strong)[strong saveSettings];};
    _actions.toggle=[weakSelf](int id){VTMenuController*strong=weakSelf;if(strong){if(strong->_backend)strong->_backend->toggle(strong->_model,id);[strong saveSettings];}};
    _actions.activateKey=[weakSelf](const std::string&k){
        VTMenuController*strong=weakSelf;if(strong)[strong activateKeyString:[NSString stringWithUTF8String:k.c_str()]];
    };
    _actions.pasteKey=[weakSelf]{
        VTMenuController*strong=weakSelf;if(strong){
            NSString*clip=VTNormalizeLicenseKey(UIPasteboard.generalPasteboard.string);
            if(clip&&clip.length>0){
                strncpy(strong->_model.keyInput,clip.UTF8String,sizeof(strong->_model.keyInput)-1);
                strong->_model.keyInput[sizeof(strong->_model.keyInput)-1]='\0';
                [strong activateKeyString:clip];
            }else{
                [strong promptKeyInputAlert];
            }
        }
    };
    _actions.promptKeyInput=[weakSelf]{
        VTMenuController*strong=weakSelf;if(strong)[strong promptKeyInputAlert];
    };
    _actions.clearKey=[weakSelf]{
        VTMenuController*strong=weakSelf;if(strong){
            strong->_model.license.valid=false;
            strong->_model.license.key.clear();
            strong->_model.license.token.clear();
            strong->_model.license.statusText="Đã đổi key; vui lòng nhập key mới";
            strong->_model.keyInput[0]='\0';
            for(int i=0;i<vt::FeatureCount;++i)strong->_model.enabled[i]=false;
            if(strong->_backend)strong->_backend->stop(strong->_model);
            [strong saveSettings];
        }
    };
    _actions.toggleAntiCapture=[weakSelf](bool enabled){
        VTMenuController*strong=weakSelf;if(strong){
            strong->_model.antiCapture=enabled;
            [strong updateAntiCaptureState];
            [strong saveSettings];
        }
    };
    _actions.diagnostic=[weakSelf]{VTMenuController*strong=weakSelf;if(strong){
        if(!strong->_backend){strong->_nextBuildCheck=0;[strong tryStartBackend:CACurrentMediaTime()];}
        const auto metadata=vt::ios::readAppMetadata();
        UIView*host=strong.view.superview;const CGSize size=host.bounds.size;const CGAffineTransform t=host.transform;const UIEdgeInsets insets=host.safeAreaInsets;
        const char*hostClass=NSStringFromClass(host.class).UTF8String;char hostInfo[320];
        snprintf(hostInfo,sizeof(hostInfo),"UI container: %s; bounds=%.0fx%.0f; inset=%.0f,%.0f,%.0f,%.0f; orientation=%ld; transform=%.2f,%.2f,%.2f,%.2f",hostClass?hostClass:"none",double(size.width),double(size.height),double(insets.top),double(insets.left),double(insets.bottom),double(insets.right),long(host.window.windowScene.interfaceOrientation),double(t.a),double(t.b),double(t.c),double(t.d));
        strong->_model.diagnostic="PTG iOS R5.9 | target VNG 2.32.0 (227628) | arm64\n"+VTGuardMessage+"\n"+VTGuardDetails+
            "\n"+std::string(hostInfo)+"\nCurrent metadata:\n"+vt::ios::metadataReport(metadata)+"\nStatus: "+strong->_model.status+
            "\nLicense: "+(strong->_model.license.valid?"VALID":"INVALID")+" ("+strong->_model.license.statusText+")\n"+
            (strong->_backend?strong->_backend->diagnostic():"Backend unavailable")+
            "\nHệ thống bảo vệ bản quyền và ẩn quay màn hình đang hoạt động trên iOS.";
        UIPasteboard.generalPasteboard.string=[NSString stringWithUTF8String:strong->_model.diagnostic.c_str()];
        strong->_model.toast="Đã sao chép chẩn đoán";strong->_model.toastUntil=strong->_model.time+1.;
    }};
}
-(void)tryStartBackend:(CFTimeInterval)now{
    if(_backend||now<_nextBuildCheck)return;
    _model.connected=false;_model.controlsAllowed=false;
    if(!VTCheckBuild()){
        _model.status=VTGuardMessage;
        _nextBuildCheck=VTGuardMayRetry?now+1.:std::numeric_limits<double>::infinity();
        return;
    }
    try{
        _backend=std::make_unique<vt::ios::GameBackend>([](const char*n){return dlsym(VTUnityHandle,n);});
        _model.connected=true;_model.controlsAllowed=true;
        _model.status="Đã xác minh bản game; bật ESP, Line, Spam nhảy hoặc Tự sẵn sàng";
    }catch(const std::exception&e){
        _model.status=e.what();_nextBuildCheck=std::numeric_limits<double>::infinity();
    }
}
-(void)handleLayoutChange:(CGSize)size{
    if(size.width<=0||size.height<=0)return;
    if(!CGSizeEqualToSize(size,_lastLayoutSize)){
        [self cancelMouse];_primaryTouch=nil;_panning=NO;_draggingWindow=NO;_model.scrollDelta=0;_model.suppressActions=true;
        _model.windowMin=_model.windowMax={};_lastLayoutSize=size;
        if(_backend)_backend->suspend(CACurrentMediaTime());
    }
}
-(void)cancelMouse{
    if(!_context)return;ImGui::SetCurrentContext(_context);auto&io=ImGui::GetIO();io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);io.AddMouseButtonEvent(0,false);
}
-(void)beginTouches:(NSSet<UITouch*>*)touches{
    if(!_context||_primaryTouch)return;_primaryTouch=touches.anyObject;
    _touchStart=_touchPrevious=[_primaryTouch locationInView:self.metalView];
    _panning=NO;_draggingWindow=NO;_model.suppressActions=false;
    ImGui::SetCurrentContext(_context);auto&io=ImGui::GetIO();io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);io.AddMousePosEvent(_touchStart.x,_touchStart.y);io.AddMouseButtonEvent(0,true);
}
-(void)moveTouches:(NSSet<UITouch*>*)touches{
    if(!_context||!_primaryTouch||![touches containsObject:_primaryTouch])return;
    CGPoint p=[_primaryTouch locationInView:self.metalView];
    CGFloat dx=p.x-_touchStart.x,dy=p.y-_touchStart.y;
    const CGFloat dist=hypot(dx,dy);

    // 1. Dragging the collapsed floating "Menu" button
    if(!_model.visible){
        if(!_draggingWindow&&dist>8.0){
            _draggingWindow=YES;
            _model.suppressActions=true;
            [self cancelMouse];
        }
        if(_draggingWindow){
            _model.floatingButtonPos.x+=float(p.x-_touchPrevious.x);
            _model.floatingButtonPos.y+=float(p.y-_touchPrevious.y);
            _touchPrevious=p;
            return;
        }
    }

    // 2. Dragging the open Menu window via its header bar
    const BOOL inHeader=_model.visible&&_touchStart.y>=_model.windowMin.y&&_touchStart.y<_model.contentMin.y&&_touchStart.x<_model.windowMax.x-68.f;
    if(_model.visible&&inHeader){
        if(!_draggingWindow&&dist>6.0){
            _draggingWindow=YES;
            _model.suppressActions=true;
            [self cancelMouse];
        }
        if(_draggingWindow){
            _model.menuWindowPos.x+=float(p.x-_touchPrevious.x);
            _model.menuWindowPos.y+=float(p.y-_touchPrevious.y);
            _touchPrevious=p;
            return;
        }
    }

    // 3. Scrolling inside content area or interacting with controls
    const BOOL inContent=_touchStart.x>=_model.contentMin.x&&_touchStart.x<_model.contentMax.x&&_touchStart.y>=_model.contentMin.y&&_touchStart.y<_model.contentMax.y;
    if(!_panning&&_model.visible&&inContent&&!_model.sliderActive&&fabs(dy)>14&&fabs(dy)>fabs(dx)*1.5){_panning=YES;_model.suppressActions=true;[self cancelMouse];}
    if(_panning){_model.scrollDelta+=p.y-_touchPrevious.y;}
    else{ImGui::SetCurrentContext(_context);ImGui::GetIO().AddMousePosEvent(p.x,p.y);}
    _touchPrevious=p;
}
-(void)endTouches:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled{
    if(!_context||!_primaryTouch||![touches containsObject:_primaryTouch])return;
    ImGui::SetCurrentContext(_context);
    if(cancelled||_panning||_draggingWindow){
        _model.suppressActions=true;
        [self cancelMouse];
        if(_draggingWindow)[self saveSettings];
    }else{
        CGPoint p=[_primaryTouch locationInView:self.metalView];
        auto&io=ImGui::GetIO();io.AddMousePosEvent(p.x,p.y);io.AddMouseButtonEvent(0,false);
    }
    _primaryTouch=nil;_panning=NO;_draggingWindow=NO;
}
-(void)setRenderingActive:(BOOL)active{
    [self cancelMouse];_primaryTouch=nil;_panning=NO;_draggingWindow=NO;_model.scrollDelta=0;_model.suppressActions=true;
    _model.jumpPressed=false;
    if(!active){
        if(_backend)_backend->suspend(CACurrentMediaTime());
        [self saveSettings];
        _resumeAfter=0;
    }else{
        _resumeAfter=CACurrentMediaTime()+0.35;
        if(_backend)_backend->suspend(CACurrentMediaTime());
    }
    self.metalView.paused=!active;_lastFrame=0;
}
-(void)drawInMTKView:(MTKView*)view{
    if(!_context||view.paused||view.bounds.size.width<=0||view.bounds.size.height<=0)return;
    // Do NOT stop drawing when screen is captured: _secureField's hardware secure layer
    // automatically strips the overlay from video/screenshots while keeping it visible on screen.
    ImGui::SetCurrentContext(_context);auto&io=ImGui::GetIO();io.DisplaySize={float(view.bounds.size.width),float(view.bounds.size.height)};
    io.DisplayFramebufferScale={float(view.drawableSize.width/view.bounds.size.width),float(view.drawableSize.height/view.bounds.size.height)};
    CFTimeInterval now=CACurrentMediaTime();io.DeltaTime=_lastFrame?float(fmin(.1,fmax(.001,now-_lastFrame))):1.f/60;_lastFrame=now;
    const CGSize bounds=view.bounds.size;
    if(!CGSizeEqualToSize(bounds,_lastLayoutSize)){
        [self cancelMouse];_primaryTouch=nil;_panning=NO;_draggingWindow=NO;_model.scrollDelta=0;_model.suppressActions=true;
        _model.windowMin=_model.windowMax={};_lastLayoutSize=bounds;
        if(_backend)_backend->suspend(now);
    }
    id<CAMetalDrawable> drawable=view.currentDrawable;MTLRenderPassDescriptor*pass=view.currentRenderPassDescriptor;if(!drawable||!pass)return;
    id<MTLCommandBuffer>command=[self.commandQueue commandBuffer];if(!command)return;
    _model.time=now;_model.insetMin={float(view.safeAreaInsets.left),float(view.safeAreaInsets.top)};_model.insetMax={float(view.safeAreaInsets.right),float(view.safeAreaInsets.bottom)};
    // Silent background license check (every 30 minutes, never interrupts gameplay or disables features)
    if(_model.license.valid&&!_model.license.key.empty()&&now>=_nextLicenseCheck&&!_activatingKey){
        _nextLicenseCheck=now+1800.0;
        NSString*savedKey=[NSString stringWithUTF8String:_model.license.key.c_str()];
        NSString*savedToken=[NSString stringWithUTF8String:_model.license.token.c_str()];
        [self checkOrActivateKey:savedKey token:savedToken];
    }
    // Only connect backend when user opens the menu or enables a feature,
    // staying completely passive during app startup and account login.
    if(_model.visible||vt::ios::runtimeRequested(_model)){
        [self tryStartBackend:now];
    }
    if(now>=_resumeAfter&&_backend)_backend->tick(_model,now);
    bool busy=_model.visible||vt::ios::runtimeRequested(_model);view.preferredFramesPerSecond=busy?60:30;
    ImGui_ImplMetal_NewFrame(pass);ImGui::NewFrame();
    if(_backend&&_model.license.valid&&(_model.enabled[vt::Line]||_model.enabled[vt::Esp])){
        auto*d=ImGui::GetBackgroundDrawList();
        const float w=io.DisplaySize.x,h=io.DisplaySize.y;
        const ImU32 darkOutline=IM_COL32(0,0,0,185);
        // 1. Draw Route Line segments (Ported from Windows overlay.html)
        if(_model.enabled[vt::Line]){
            for(const auto&seg:_backend->route()){
                const ImVec2 a{seg.a.x*w,seg.a.y*h};
                const ImVec2 b{seg.b.x*w,seg.b.y*h};
                ImU32 col=IM_COL32(53,255,101,242); // #35ff65 green (current stage walk)
                if(seg.drop)col=IM_COL32(255,213,74,235); // #ffd54a yellow (drop)
                else if(!seg.currentStage)col=IM_COL32(32,224,255,220); // #20e0ff cyan (next stage walk)
                if(seg.drop){
                    const float dx=b.x-a.x,dy=b.y-a.y;
                    const float len=hypotf(dx,dy);
                    if(len<1.f)continue;
                    const float ux=dx/len,uy=dy/len;
                    const float dashLen=7.f,gapLen=5.f;
                    for(float t=0.f;t<len;t+=dashLen+gapLen){
                        const float tEnd=fminf(t+dashLen,len);
                        const ImVec2 p0{a.x+ux*t,a.y+uy*t};
                        const ImVec2 p1{a.x+ux*tEnd,a.y+uy*tEnd};
                        d->AddLine(p0,p1,darkOutline,4.5f);
                        d->AddLine(p0,p1,col,2.25f);
                    }
                }else{
                    d->AddLine(a,b,darkOutline,4.5f);
                    d->AddLine(a,b,col,2.25f);
                }
            }
        }
        // 2. Draw Minigame ESP rings + center dots + labels (Ported from Windows overlay.html)
        if(_model.enabled[vt::Esp]){
            std::vector<ImVec2> ringPts;
            ringPts.reserve(16);
            for(const auto&p:_backend->esp()){
                ImU32 col=IM_COL32(53,255,101,245); // #35ff65 green
                if(p.color=="red")col=IM_COL32(255,77,77,235); // #ff4d4d red
                else if(p.color=="yellow")col=IM_COL32(255,213,74,240); // #ffd54a yellow

                if(p.hasRing&&p.ring.size()>=3){
                    ringPts.clear();
                    for(const auto&pt:p.ring){
                        ringPts.push_back({pt.x*w,pt.y*h});
                    }
                    d->AddPolyline(ringPts.data(),int(ringPts.size()),darkOutline,ImDrawFlags_Closed,3.8f);
                    d->AddPolyline(ringPts.data(),int(ringPts.size()),col,ImDrawFlags_Closed,2.0f);
                }

                const ImVec2 center{p.x*w,p.y*h};
                if(center.x>=-32.f&&center.x<=w+32.f&&center.y>=-32.f&&center.y<=h+32.f){
                    d->AddCircleFilled(center,4.2f,darkOutline,12);
                    d->AddCircleFilled(center,2.8f,col,12);

                    if(!p.label.empty()){
                        const ImVec2 tsz=ImGui::CalcTextSize(p.label.c_str());
                        const ImVec2 tpos{center.x-tsz.x*0.5f,center.y-tsz.y-8.f};
                        d->AddRectFilled({tpos.x-4.f,tpos.y-2.f},{tpos.x+tsz.x+4.f,tpos.y+tsz.y+2.f},IM_COL32(8,12,18,195),4.f);
                        d->AddText(tpos,col,p.label.c_str());
                    }
                }
            }
        }
    }
    vt::ios::draw(_model,_actions);ImGui::Render();
    id<MTLRenderCommandEncoder>encoder=[command renderCommandEncoderWithDescriptor:pass];if(!encoder)return;
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(),command,encoder);[encoder endEncoding];[command presentDrawable:drawable];[command commit];
}
-(void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size{}
-(void)dealloc{
    if(_backend){_backend->stop(_model);_backend.reset();}
    _metalView.delegate=nil;_metalView.touchOwner=nil;
    if(_context){ImGui::SetCurrentContext(_context);ImGui_ImplMetal_Shutdown();ImGui::DestroyContext(_context);_context=nullptr;}
}
@end

static VTMenuController* VTController;
static __weak UIView* VTHostView;
static __weak UIWindowScene* VTHostScene;
static __weak UIWindow* VTHostWindow;
static NSTimer* VTHostTimer;

static UIView* VTFindGameView(UIView*view,int depth){
    if(!view||depth>24||view.hidden||view.alpha<.01||!view.window||[view isKindOfClass:VTPassthroughView.class])return nil;
    Class unity=NSClassFromString(@"UnityView"),rendering=NSClassFromString(@"UnityRenderingView");
    const bool unityView=unity&&[view isKindOfClass:unity];
    if((rendering&&[view isKindOfClass:rendering])||(unityView&&[view.layer isKindOfClass:CAMetalLayer.class]))return view;
    for(UIView*child in view.subviews){UIView*found=VTFindGameView(child,depth+1);if(found)return found;}
    if([view.layer isKindOfClass:CAMetalLayer.class]||unityView)return view;
    return nil;
}

static void VTAttachJumpRecognizers(UIView*unity,UIView*container){
    if(!VTController)return;
    UIView*touchTarget=unity?unity:container;
    if(touchTarget){
        BOOL hasJumpRec=NO;
        for(UIGestureRecognizer*gr in touchTarget.gestureRecognizers){
            if([gr isKindOfClass:VTJumpGestureRecognizer.class]){
                ((VTJumpGestureRecognizer*)gr).menuController=VTController;
                hasJumpRec=YES;
                break;
            }
        }
        if(!hasJumpRec){
            VTJumpGestureRecognizer*jg=[[VTJumpGestureRecognizer alloc]init];
            jg.menuController=VTController;
            [touchTarget addGestureRecognizer:jg];
        }
    }
    if(container&&container!=touchTarget){
        BOOL hasOnContainer=NO;
        for(UIGestureRecognizer*gr in container.gestureRecognizers){
            if([gr isKindOfClass:VTJumpGestureRecognizer.class]){
                ((VTJumpGestureRecognizer*)gr).menuController=VTController;
                hasOnContainer=YES;
                break;
            }
        }
        if(!hasOnContainer){
            VTJumpGestureRecognizer*jg=[[VTJumpGestureRecognizer alloc]init];
            jg.menuController=VTController;
            [container addGestureRecognizer:jg];
        }
    }
}

static void VTDetach(){
    if(!VTController)return;
    [VTController setJumpHeld:NO];
    [VTController setRenderingActive:NO];
    [VTController.view removeFromSuperview];
    VTController=nil;VTHostView=nil;VTHostScene=nil;VTHostWindow=nil;
}

static void VTAttach(){
    if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{VTAttach();});return;}
    if(UIApplication.sharedApplication.applicationState==UIApplicationStateBackground)return;
    UIWindow*game=nil;UIView*unity=nil;
    for(UIScene*scene in UIApplication.sharedApplication.connectedScenes){
        if(![scene isKindOfClass:UIWindowScene.class])continue;
        if(scene.activationState!=UISceneActivationStateForegroundActive&&scene.activationState!=UISceneActivationStateForegroundInactive)continue;
        for(UIWindow*window in ((UIWindowScene*)scene).windows){
            if(window.hidden||window.alpha<.01||![window.rootViewController isViewLoaded])continue;
            UIView*found=VTFindGameView(window.rootViewController.view,0);
            if(!found)continue;
            if(!game||[window isKeyWindow]){game=window;unity=found;}
            if([window isKeyWindow])break;
        }
        if(game&&[game isKeyWindow])break;
    }
    if(!game||!unity)return;
    UIViewController*parent=game.rootViewController;
    if(!parent||![parent isViewLoaded])return;
    UIView*container=parent.view;
    if(!container)return;
    if(!CGAffineTransformIsIdentity(container.transform))container=game;
    if(container!=game&&container.window!=game)return;
    const CGSize size=container.bounds.size;
    if(size.width<=0||size.height<=0)return;
    if(VTController){
        VTHostView=container;VTHostScene=game.windowScene;VTHostWindow=game;
        if(VTController.view.superview!=container){
            [container addSubview:VTController.view];
        }
        if(!CGRectEqualToRect(VTController.view.frame,container.bounds))VTController.view.frame=container.bounds;
        if(!CGRectEqualToRect(VTController.metalView.frame,VTController.view.bounds))VTController.metalView.frame=VTController.view.bounds;
        if(!VTController.keyAlertShowing&&container.subviews.lastObject!=VTController.view)[container bringSubviewToFront:VTController.view];
        VTAttachJumpRecognizers(unity,container);
        if(VTController.view.hidden||VTController.metalView.paused){
            VTController.view.hidden=NO;
            [VTController updateAntiCaptureState];
            [VTController setRenderingActive:YES];
        }
        return;
    }
    VTController=[[VTMenuController alloc]initWithFrame:container.bounds];
    VTHostView=container;VTHostScene=game.windowScene;VTHostWindow=game;
    [container addSubview:VTController.view];
    [container bringSubviewToFront:VTController.view];
    VTAttachJumpRecognizers(unity,container);
}

extern "C" __attribute__((visibility("default"))) void VTMenuStart(){dispatch_async(dispatch_get_main_queue(),^{VTAttach();});}
__attribute__((constructor)) static void VTBootstrap(){
    VTSpoofBundleIdentifier();
    dispatch_async(dispatch_get_main_queue(),^{
        NSNotificationCenter*c=NSNotificationCenter.defaultCenter;
        [c addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){VTAttach();}];
        [c addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){VTAttach();}];
        [c addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){[VTController setRenderingActive:NO];}];
        [c addObserverForName:UISceneDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){if(n.object==VTHostScene)VTDetach();}];
        [c addObserverForName:UIScreenCapturedDidChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){
            if(VTController)[VTController handleScreenCaptureChanged:UIScreen.mainScreen.isCaptured];
        }];
        VTHostTimer=[NSTimer timerWithTimeInterval:.5 repeats:YES block:^(NSTimer*t){VTAttach();}];
        [[NSRunLoop mainRunLoop]addTimer:VTHostTimer forMode:NSRunLoopCommonModes];
        VTAttach();
    });
}
