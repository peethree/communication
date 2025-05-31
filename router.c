#include <czmq.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <dirent.h>

// TODO: add curvezmq authentication
// both the router and dealer need a set of public and secret keys

// ROUTER:	server identity; stays constant.
// DEALER:	ephemeral or long-term identity.

// dealers use the router's public key to authenticate it
// router uses the dealer's public kye to authorize or deny access

zcertstore_t *load_certs(char* directory) 
{
    zcertstore_t *new = zcertstore_new(directory);
    DIR *dir = opendir(directory);
    struct dirent *entry;

    if (!dir){
        printf("Directory not found\n");
        return NULL;
    }

    while ((entry = readdir(dir)) != NULL) {
        // skip current and parent directory
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // construct full path to the certificate file
        size_t filepath_buffer = strlen(directory) + strlen(entry->d_name) + 2; // +2 '\0' and "/"
        char *filepath = malloc(filepath_buffer); 
        if (!filepath) {
            printf("Buy more ram!\n");
            return NULL;
        } else {
            snprintf(filepath, filepath_buffer, "%s/%s", directory, entry->d_name);

            // load cert and add it to the store
            zcert_t *cert = zcert_load(filepath);
            if (cert) {
                zcertstore_insert(new, &cert);
            } else {
                printf("Failed to load certificate: %s\n", filepath);
            }            
        }  
        free(filepath);
    }

    closedir(dir);
    return new;
}

// kill router if perpetually blocked: ps aux | grep router ----- kill -9 with associated ./router pid
int main(void)
{
    // load certs from certificate directory
    zcertstore_t *certstore = load_certs("keys"); 
    if (!certstore){
        printf("No certificates loaded\n");
        return -1;
    }

    zcertstore_print(certstore);

    // load router certificate and apply it to the socket
    zcert_t *router_cert = zcert_load("router.cert");
    if (!router_cert){
        printf("Router certificate not found\n");
        return 1;
    }
    zsock_t *router = zsock_new(ZMQ_ROUTER);
    if (!router){
        printf("Failed to create router socket\n");
        zcert_destroy(&router_cert);
        return 2;
    }
    
    zcert_apply(router_cert, router);

    // set to act as CURVE server
    zsock_set_curve_server(router, true);

    // zauth actor instance
    zactor_t *auth = zactor_new(zauth, router_cert);
    if (!auth){
        printf("Unable to create authentication actor\n");
        zsock_destroy(&router);
        zcert_destroy(&router_cert);
        return 3;
    }

    int rc = zsock_bind(router, "tcp://*:5555");
    assert (rc != -1);
    if (rc != 0) {
        printf("Unable to bind to socket\n");
        zactor_destroy(&auth);
        zsock_destroy(&router);
        zcert_destroy(&router_cert);
        return 4;
    }

    printf("router started successfully...\n");        

    // non-blocking:
    // zpoller_t *poller = zpoller_new(router, NULL);

    // while (!zsys_interrupted) {
    //     void *signaled_socket = zpoller_wait(poller, 1); // 1ms timeout
    //     if (!signaled_socket)
    //         continue;

    //     if (signaled_socket == router) {
    //         // message framing is as follows dealer > router > dealer: [sender_id][recipient_id][message_content] ???
    //         zmsg_t *msg = zmsg_recv(router);
    //         if (!msg) {
    //             printf("Interrupted or error receiving message\n");
    //             break;
    //         }

    // blocking
    while (!zsys_interrupted) {
        zmsg_t *msg = zmsg_recv(router);  
        if (!msg) {
            printf("Interrupted or error receiving message\n");
            break;
        }     

        // add sender's public key's message frame
        // zframe_t *sender_pub_key = zmsg_pop(msg);
        // zcertstore_lookup()

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
        //zmsg_append(reply, &sender_pub_key);
        zmsg_append(reply, &rec_id);        // ROUTING: destination frame
        zmsg_append(reply, &sender_id);     // CONTENT: original sender ID (as body)
        zmsg_append(reply, &message_data);  // CONTENT: message
        zmsg_send(&reply, router);

        //cleanup
        zmsg_destroy(&msg);
    }
    // zpoller_destroy(&poller);
    zactor_destroy(&auth);
    zsock_destroy(&router);
    zcert_destroy(&router_cert);

    return 0;
}
