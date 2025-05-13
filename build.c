#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX

#include "nob.h"

// cc -o build build.c
int main(int argc, char** argv) {       
    NOB_GO_REBUILD_URSELF(argc, argv);

    // build 1
    Cmd cmd = {0};
    cmd_append(&cmd, 
        "cc", 
        "server.c",             
        "-I/usr/include",
        "-lczmq",
        // "-Wall", 
        // "-Wextra", 
        "-o", "server"                                    
    );

    if (!cmd_run_sync(cmd)) {
        nob_log(NOB_ERROR, "Build 1 failed");
        return 1;
    }  

    // build 2
    Cmd cmd2 = {0};
    cmd_append(&cmd2,
        "cc", 
        "client.c",             
        "-I/usr/include",
        "-lczmq",
        // "-Wall", 
        // "-Wextra", 
        "-o", "client"                                    
    );
    
  

    if (!cmd_run_sync(cmd2)) {
        nob_log(NOB_ERROR, "Build 2 failed");
        return 1;
    }    

    return 0;
}