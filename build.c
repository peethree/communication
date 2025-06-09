#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX

#include "nob.h"

// cc -o build build.c
int main(int argc, char** argv) {       
    NOB_GO_REBUILD_URSELF(argc, argv);

    Cmd cmd = {0};
    cmd_append(&cmd, 
        "cc", 
        "router.c",   
        "-g",          
        "-I/usr/include",
        "-lczmq",
        "-Wall", 
        "-Wextra", 
        "-o", "router"                                    
    );

    if (!cmd_run_sync(cmd)) {
        nob_log(NOB_ERROR, "Build 1 (router) failed");
        return 1;
    }  

    Cmd cmd2 = {0};
    cmd_append(&cmd2,
        "cc", 
        "dealer.c",   
        "-g",                                     
        "-I/usr/include",               //czmq
        "-lczmq", 
        "-I/usr/local/include",         // raylib
        "-L/usr/local/lib",         
        "-lraylib", 
        "-lm",
        "-lssl",                        // ssl
        "-lcrypto",
        "-Wall", 
        "-Wextra",
        "-o", 
        "dealer"                                    
    );      

    if (!cmd_run_sync(cmd2)) {
        nob_log(NOB_ERROR, "Build 2 (dealer) failed");
        return 1;
    }    

    return 0;
}