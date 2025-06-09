#include <czmq.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

// TODO: add curvezmq authentication
// both the router and dealer need a set of public and secret keys

// ROUTER:	server identity; stays constant.
// DEALER:	ephemeral or long-term identity.

// dealers use the router's public key to authenticate it
// router uses the dealer's public kye to authorize or deny access

// kill router if perpetually blocked: ps aux | grep router ----- kill -9 with associated ./router pid
int main()
{
    // load certs from certificate directory
    const char* directory = "keys_router";
    printf("Making a certificate store of the %s directory...\n", directory);
    zcertstore_t *cert_store = zcertstore_new(directory);
    if (!cert_store){
        fprintf(stderr, "Failed to create certificate store\n");
        return -1;
    }    

    // zcertstore_print(certstore);

    // look up router public key
    const char* router_pub_key = "A9Iz>yq^pr*w=I1.vTE)NDguZ0[#>GXl-hZ=B>&0";
    printf("Looking up the router's certificate\n");
    zcert_t *router_cert = zcertstore_lookup(cert_store, router_pub_key);
    if (!router_cert) {
        printf("Certificate does not match the store lookup\n");
        zcertstore_destroy(&cert_store);
        return -1;
    }

    // TODO:
    // registration keys
    // only allow the registration cert for registering, not for messaging

    // zauth actor instance (NULL: default) configurations needed?
    // runs concurrently with the rest of the program (async authentication?)
    zactor_t *auth = zactor_new(zauth, NULL);
    if (!auth){
        printf("Unable to create authentication actor\n");
        zcert_destroy(&router_cert);
        zcertstore_destroy(&cert_store);
        return 3;
    }

    int send_ok = zstr_sendx(auth, "CURVE", directory, NULL);
    if (send_ok == -1) {
        printf("[ERROR]: Could not send strings\n");
    }
    zsock_wait(auth);

    printf("CURVE authentication configured\n");
    
    zsock_t *router = zsock_new(ZMQ_ROUTER);
    if (!router){
        printf("Failed to create router socket\n");
        zactor_destroy(&auth);        
        zcert_destroy(&router_cert);
        zcertstore_destroy(&cert_store);   
        return 2;
    }

    // apply the router's certificate to the socket
    zcert_apply(router_cert, router);
    printf("Applied router's certificate to its socket\n");

    // set to act as CURVE server
    zsock_set_curve_server(router, 1);
    printf("Set socket option to: CURVE\n");

    int rc = zsock_bind(router, "tcp://*:5555");
    if (rc == -1){
        zactor_destroy(&auth);        
        zcert_destroy(&router_cert);
        zcertstore_destroy(&cert_store);   
        printf("[ERROR]: Unable to bind socket to formatted endpoint");
        return 1;
    }
    printf("router started successfully on port %d...\n", rc);        

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

        // messages must adhere to certain shape and size
        size_t msg_size = zmsg_size(msg);

        printf("received message size: %zu\n", msg_size);

        // if msg size has size of a registration message (3):
        // [sender id][registration key][user cert  ]
        if (msg_size == 3) {
            // size 2 msg received
            // registration sender id
            zframe_t *reg_id = zmsg_pop(msg);
            zframe_print(reg_id, "registration id: ");

            // registering user cert
            zframe_t *reg_cert = zmsg_pop(msg);
            zframe_print(reg_cert, "registration key: ");

            // check if the user provided the correct registration key
            char *reg_cert_str = zframe_strdup(reg_cert);
            if (!reg_cert_str) {
                printf("no key received\n");
                // TODO:
            }

            printf("reg cert string: %s\n", reg_cert_str);

            if (zcertstore_lookup(cert_store, reg_cert_str) == NULL) {
                printf("false registration certificate\n");
                break;
            }            

            // registration code here.
            // add user's pub key to certstore
            // break;

            zframe_t *user_cert = zmsg_pop(msg);
            zframe_print(user_cert, "user's actual cert: ");

            char *user_cert_str = zframe_strdup(user_cert);
            if (!user_cert_str) {
                printf("no key received\n");
            }

            // make a new certificate for the router to store as an accepted user
            zcert_t *user_cert_pub = zcert_new_from((const unsigned char*)user_cert_str, NULL);

            // get the username and format where to store the cert
            char* username = zframe_strdup(reg_id);
            size_t cert_buffer = strlen(username) + 18; // formatted text + '\0'
            char* certificate_location = malloc(cert_buffer);
            snprintf(certificate_location, cert_buffer, "keys_router/%s.cert", username);

            // save the cert to disc
            zcert_save(user_cert_pub, certificate_location);

            // free when no longer needed
            free(username);
            free(certificate_location);

            // everything should be ok, send signal
            zmsg_t *reply = zmsg_new_signal(0);
            int signal = zmsg_send(&reply, reg_id);
            if (signal != 0) {
                printf("Unable to send the registration signal\n");
                zmsg_destroy(&reply);
            }
        } 

        // regular message size
        if (msg_size == 4) {
            // pop sender id
            zframe_t *sender_id = zmsg_pop(msg); 
            // printf("sender id: %s\n", zframe_strhex(sender_id));
            zframe_print(sender_id, "sender id: ");

            // add sender's public key's message frame
            zframe_t *sender_pub_key = zmsg_pop(msg);
            zframe_print(sender_pub_key, "sender pub key:");

            // TODO: if sender_pub_key is not known by the router, stop here
            char* sender_key_string = zframe_strdup(sender_pub_key);
            zcert_t *sender_cert = zcertstore_lookup(cert_store, sender_key_string);
            if (!sender_cert){
                // sender cert is null n therefore not found
                // skip this iteration and continue, cleanup
                printf("Unknown sender\n");
                free(sender_key_string);
                zframe_destroy(&sender_pub_key);
                zmsg_destroy(&msg);            
                continue;
            }      

            // pop recipient id
            zframe_t *rec_id = zmsg_pop(msg);
            // printf("recipient id: %s\n", zframe_strhex(rec_id));
            zframe_print(rec_id, "recipient id: ");

            // pop msg content
            zframe_t *message_data = zmsg_pop(msg);   
            zframe_print(message_data, "cipher: ");
            
            // reply ... forward to recipient
            zmsg_t *reply = zmsg_new();
            zmsg_append(reply, &rec_id);                // ROUTING: destination frame
            zmsg_append(reply, &sender_id);             // CONTENT: original sender ID (as body)
            zmsg_append(reply, &message_data);          // CONTENT: message
            int result = zmsg_send(&reply, router);     // resulting in 2 frames in the received message?
            if (result != 0) {
                printf("Failed to send message\n");
                // zmsg_send destroys the message on success, but not on failure
                zmsg_destroy(&reply);
            }

            free(sender_key_string);
            zframe_destroy(&sender_pub_key);
        }
    }
    // zpoller_destroy(&poller);
    zsock_destroy(&router);
    zactor_destroy(&auth);        
    // zcert_destroy(&router_cert);
    zcertstore_destroy(&cert_store);    

    return 0;
}
