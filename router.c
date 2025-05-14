#include <czmq.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>


// kill router: ps aux | grep router ----- kill -9 with associated ./router pid
int main (void)
{
    zsock_t *router = zsock_new(ZMQ_ROUTER);
    int rc = zsock_bind(router, "tcp://*:5555");
    assert (rc == 5555);

    while (1) {
        char *str = zstr_recv(router);
        printf("Received:: %s\n", str);
        sleep(1);          
        zstr_send(router, "World");
        zstr_free(&str);
    }
    return 0;
}
