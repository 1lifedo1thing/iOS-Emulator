// Built only into the Debug variant. Fixed legal TCG program, no firmware recipe.
#include <jni.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <string>
#include <vector>
#include <cstdio>
extern "C" JNIEXPORT jint JNICALL Java_org_lighttouch_android_TcgProbeNative_run(JNIEnv *env,jobject,jstring path) {
    static std::atomic<bool> consumed{false};
    if(consumed.exchange(true))return -2;
    const char *raw=env->GetStringUTFChars(path,nullptr);if(!raw)return -3;
    std::string directory(raw);env->ReleaseStringUTFChars(path,raw);
    int log=open((directory+"/engine.log").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(log<0)return -4;
    dup2(log,STDOUT_FILENO);dup2(log,STDERR_FILENO);close(log);
    void *library=dlopen("libqemu-arm.so",RTLD_NOW|RTLD_LOCAL);
    if(!library){fprintf(stderr,"Engine dlopen failed: %s\n",dlerror());return -5;}
    auto main=reinterpret_cast<int (*)(int,char **)>(dlsym(library,"qemu_ios_main"));
    if(!main){fprintf(stderr,"qemu_ios_main missing: %s\n",dlerror());return -6;}
    std::vector<std::string> args={"qemu","-M","virt","-cpu","cortex-a15","-m","16M","-nodefaults","-display","none","-serial","none","-monitor","none","-qmp","unix:"+directory+"/qmp.sock,server=on,wait=off","-device","loader,file="+directory+"/guest.bin,addr=0x40200000,cpu-num=0"};
    std::vector<char *> argv;for(auto &arg:args)argv.push_back(arg.data());argv.push_back(nullptr);
    return main(int(args.size()),argv.data());
}
