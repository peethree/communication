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

    printf("router started successfully...\n");    

    zpoller_t *poller = zpoller_new(router, NULL);

    while (!zsys_interrupted) {
        void *signaled_socket = zpoller_wait(poller, 1); // 1ms timeout
        if (!signaled_socket)
            continue;

        if (signaled_socket == router) {
            // message framing is as follows dealer > router > dealer: [sender_id][recipient_id][message_content] ???
            zmsg_t *msg = zmsg_recv(router);
            if (!msg) {
                printf("Interrupted or error receiving message\n");
                break;
            }

            // printf("Received: \n");
            // pop sender id
            zframe_t *sender_id = zmsg_pop(msg); 
            // printf("sender id: %s\n", zframe_strhex(sender_id));

            // pop recipient id
            zframe_t *rec_id = zmsg_pop(msg);
            // printf("recipient id: %s\n", zframe_strhex(rec_id));

            // pop msg content
            zframe_t *message_data = zmsg_pop(msg);   
            // zframe_print(message_data, "cipher: ");
            
            // reply ... forward to recipient
            zmsg_t *reply = zmsg_new();
            zmsg_append(reply, &rec_id);        // ROUTING: destination frame
            zmsg_append(reply, &sender_id);     // CONTENT: original sender ID (as body)
            zmsg_append(reply, &message_data);  // CONTENT: message
            zmsg_send(&reply, router);

            //cleanup
            zmsg_destroy(&msg);
        }
    }
    zpoller_destroy(&poller);
    zsock_destroy(&router);
    return 0;
}
