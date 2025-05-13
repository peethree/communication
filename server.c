//  Hello World server
#include <czmq.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>


// kill server: ps aux | grep server ----- kill -9 with server pid
int main (void)
{
    //  Socket to talk to clients
    zsock_t *responder = zsock_new (ZMQ_REP);
    int rc = zsock_bind (responder, "tcp://*:5555");
    assert (rc == 5555);

    while (1) {
        char *str = zstr_recv (responder);
        printf ("Received %s\n", str);
        sleep (1);          //  Do some 'work'
        zstr_send (responder, "World");
        zstr_free (&str);
    }
    return 0;
}
