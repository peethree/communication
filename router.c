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
    printf("router bound to: %d\n", rc);

    while (1) {
        // message framing is as follows dealer > router > dealer: [sender_id][recipient_id][message_content]
        zmsg_t *msg = zmsg_recv(router);
        if (!msg) {
            break;
        }
        
        printf("Received: \n");
        // pop sender id
        zframe_t *sender_id = zmsg_pop(msg); 
        printf("sender id: %s\n", zframe_strhex(sender_id));

        // pop recipient id
        zframe_t *rec_id = zmsg_pop(msg);
        printf("recipient id: %s\n", zframe_strhex(rec_id));

        // pop msg content
        zframe_t *message_data = zmsg_pop(msg);   
        char* content = zframe_strdup(message_data);
        printf("content: %s\n", content);          
        
        // reply
        zmsg_t *reply = zmsg_new();
        zmsg_append(reply, &rec_id);
        zmsg_addstr(reply, content);
        zmsg_send(&reply, router);

        //cleanup
        zframe_destroy(&sender_id);
        zframe_destroy(&rec_id);
        zframe_destroy(&message_data);
    }

    zsock_destroy(&router);
    return 0;
}
