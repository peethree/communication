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
    assert (rc != -1);

    while (1) {
        // message framing is as follows: [identity][message]
        zmsg_t *msg = zmsg_recv(router);
        if (!msg) {
            break;
        }
        
        printf("Received: \n");
        // pop id
        zframe_t *messenger_id = zmsg_pop(msg); 
        printf("id: %s\n", zframe_strhex(messenger_id));

        // pop msg content
        zframe_t *message_data = zmsg_pop(msg);   
        printf("content: %s\n", zframe_strdup(message_data));                
        
        zstr_send(router, "aaaaaaaa");
    }
    return 0;
}
