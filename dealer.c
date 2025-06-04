#include <czmq.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

// raylib LOG enums interfere with system.h logging
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#include <raylib.h>

// encryption
#include <openssl/evp.h>
#include <openssl/rand.h>

typedef struct {
    char** items;
    size_t capacity;
    size_t count;
} UserInput;

typedef struct {
    char *sent_msg;
    char *received_msg;
    time_t timestamp;
    bool sent;
    bool received;    
} Message;

typedef struct {
    Message *items;
    size_t capacity;
    size_t count;
} ChatHistory;

typedef struct {
    zcert_t *user_certificate;
    char *most_recent_received_message;
    char *message_to_send;
    char *sender_id;
    char *recipient_id;    
} MessageData;

typedef struct {
    MessageData message_data;   
    char* user_name; 
    char* user_input;
    char* recipient;
    unsigned char* iv;
    unsigned char* key;
    zsock_t *dealer;    
    pthread_mutex_t mutex;    
    pthread_cond_t send_cond;
    pthread_cond_t user_name_cond;
    bool running;
    bool is_there_a_msg_to_send;
    bool username_processed;
} Receiver;

void get_user_input(UserInput *input)
{
    int unicode = GetCharPressed();
    if (unicode > 0) {          
        char* newChar = malloc(8); // +7 padding to avoid overflow from weird characters/emojis
        if (newChar) {
            snprintf(newChar, 8, "%c", unicode);
            da_append(input, newChar);
        }
    } 
}

// TODO: have a look at the magic numbers
void draw_user_input(UserInput *input)
{
    int pos_x = 10;
    int pos_y = 560;
    int fontsize = 20;
    
    for (size_t i = 0; i < input->count; i++) {
        // calculate how many characters fit per line
        int chars_per_line = (GetScreenWidth() - fontsize - pos_x) / fontsize;  
        int line = i / chars_per_line;
        
        // position on the current line
        int line_position = i % chars_per_line;                
        int current_x = pos_x + (line_position * fontsize);
        int current_y = pos_y + (line * 25);

        if (input->items[i]) DrawText(input->items[i], current_x, current_y, fontsize, RED);
    }
}

// drawing helper function for hiding the user's pw input
void draw_user_input_hidden(UserInput *input){

    int pos_x = 10;
    int pos_y = 560;
    int fontsize = 20;
    
    for (size_t i = 0; i < input->count; i++) {
        int chars_per_line = (GetScreenWidth() - fontsize - pos_x) / fontsize;  
        int line = i / chars_per_line;
        
        int line_position = i % chars_per_line;                
        int current_x = pos_x + (line_position * fontsize);
        int current_y = pos_y + (line * 25);

        if (input->items[i]) DrawText("*", current_x, current_y, fontsize, RED);
    }
}

// TODO: while holding down backspace, gradually decrement count. a while loop in combination with
// iskeydown causes issues and the process needs to be manually killed
void erase_user_input(UserInput *input)
{
    if (IsKeyPressed(KEY_BACKSPACE)) {            
        if (input->count > 0) input->count--;            
    }
}

char* formulate_string_from_user_input(UserInput *input) 
{    
    // calculate total length of the resulting string
    size_t len = 0;
    for (size_t i = 0; i < input->count; i++) len += strlen(input->items[i]);    

    // allocate memory based on total length of the input
    char* str = malloc(len + 1);    
    if (!str) return NULL;
    
    // strcat expects str to start with a '\0' else it won't know where to start appending
    str[0] = '\0';
    for (size_t i = 0; i < input->count; i++) strcat(str, input->items[i]);

    return str;
}

// copy user input into message buffer of the send message thread (args->message_data)
void send_user_input(const char* user_input, char* recipient_id, Receiver *args) 
{   
    // mutex
    pthread_mutex_lock(&args->mutex);

    // make sure the memory is free 
    free(args->message_data.message_to_send);
    free(args->message_data.recipient_id);

    size_t input_len = strlen(user_input);
    args->message_data.message_to_send = malloc(input_len + 1);
    if (args->message_data.message_to_send){
        strncpy(args->message_data.message_to_send, user_input, input_len);
        args->message_data.message_to_send[input_len] = '\0';
    } else {
        printf("ERROR: buy more RAM!\n");
        return;
    }  

    size_t id_len = strlen(recipient_id);
    args->message_data.recipient_id = malloc(id_len + 1);
    if (args->message_data.recipient_id) {
        strncpy(args->message_data.recipient_id, recipient_id, id_len);
        args->message_data.recipient_id[id_len] = '\0';
    } else {
        printf("ERROR: buy more RAM!\n");
        return;
    }
    args->is_there_a_msg_to_send = true;
    // there is a message to send, so send a signal the the messaging thread to wake it up 
    pthread_cond_signal(&args->send_cond);

    pthread_mutex_unlock(&args->mutex);
}

// encrypt plaintext into ciphertext using aes
void aes_encrypt(unsigned char* key, unsigned char* iv, unsigned char* plaintext, unsigned char* ciphertext, size_t *ciphertext_len, size_t plaintext_len)
{
    // printf("plaintext len: %zu\n", plaintext_len);
    // create encryption context
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        fprintf(stderr, "Failed to create a context for encryption.\n");
        return;
    }

    // turn on byte padding 
    EVP_CIPHER_CTX_set_padding(ctx, 1);

    // init aes encryption
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv) != 1) {
        fprintf(stderr, "Failed to initialize encryption.\n");
        EVP_CIPHER_CTX_free(ctx);
        return;
    }
   
    int len;    
     // encrypt 16 byte blocks
    if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len) != 1) {
        fprintf(stderr, "Failed to encrypt data.\n");
        EVP_CIPHER_CTX_free(ctx);
        return;
    }
    int total_len = len; 

    assert(EVP_CIPHER_CTX_get_block_size(ctx) == 16);    
    // int iv_length = EVP_CIPHER_CTX_get_iv_length(ctx);
    // printf("iv length: %d\n", iv_length);
    // int key_length = EVP_CIPHER_CTX_get_key_length(ctx);
    // printf("key length: %d\n", key_length);

    // finalize
    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) {
        fprintf(stderr, "Failed to finalize encryption.\n");
        EVP_CIPHER_CTX_free(ctx);
        return;
    }
    total_len += len;
    *ciphertext_len = total_len;

    // printf("Encrypted message length: %zu\n", *ciphertext_len);

    EVP_CIPHER_CTX_free(ctx);
}

// decrypt the ciphertext
void aes_decrypt(unsigned char* key, unsigned char* iv, unsigned char* ciphertext, unsigned char* plaintext, size_t ciphertext_len)
{
    // printf("Received ciphertext length: %zu\n", ciphertext_len);

    // create decryption context
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();    
    if (!ctx) {
        fprintf(stderr, "Failed to create context for decryption.\n");
        return;
    }

    // turn on byte padding / should be on by default
    EVP_CIPHER_CTX_set_padding(ctx, 1);

    // init aes decryption
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv) != 1) {
        fprintf(stderr, "Failed to initialize decryption.\n");
        EVP_CIPHER_CTX_free(ctx);
        return;
    }

    assert(EVP_CIPHER_CTX_get_block_size(ctx) == 16);
    // int iv_length = EVP_CIPHER_CTX_get_iv_length(ctx);
    // printf("iv length: %d\n", iv_length);
    // int key_length = EVP_CIPHER_CTX_get_key_length(ctx);
    // printf("key length: %d\n", key_length);

    // printf("decrypt--cipher text len: %zu\n", ciphertext_len);
    int len;
    int plaintext_len;
    
    // processes the data in chunks during decryption
    if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, (int)ciphertext_len) != 1) {
        fprintf(stderr, "Failed to decrypt data.\n");
        EVP_CIPHER_CTX_free(ctx);
        return;
    }     
    plaintext_len = len;
    // printf("decrypt--cipher text len after update: %d\n", plaintext_len);
    // printf("decrypted so far: %s\n", plaintext);

    // finalize decryption
    if (EVP_DecryptFinal_ex(ctx, plaintext + len, &len) != 1) {
        fprintf(stderr, "Failed to finalize decryption.\n");
        EVP_CIPHER_CTX_free(ctx);
        return;
    }
    plaintext_len += len;    
    // nul terminate plaintext
    plaintext[plaintext_len] = '\0'; 

    EVP_CIPHER_CTX_free(ctx);
}

size_t calculate_padding(size_t text_len)
{   
    size_t remainder = text_len % 16;
    size_t padding = 0;
    if (remainder != 0) padding = 16 - remainder;

    return padding;
}

/* 
this function will run concurrent with the raylib window and the send_messages function
(it follows the required signature for the pthread_create() function in C.) 
*/
void *receive_messages(void *args_ptr)
{
    Receiver *args = (Receiver *)args_ptr;
    
    while (args->running && !zsys_interrupted) { // zsys_interrupted CZMQ: "Global signal indicator, TRUE when user presses Ctrl-C"
        // this blocks until a message is received
        zmsg_t *reply = zmsg_recv(args->dealer); 
        if (!reply) {
            continue;
        }

        // reply format [sender id][message content]
        zframe_t *sender_id = zmsg_pop(reply);
        zframe_t *message_content = zmsg_pop(reply);      

        if (message_content && sender_id) {  
            // check for shutdown message
            char* shutdown_msg = zframe_strdup(message_content);             
            if (strcmp(shutdown_msg, "/shutdown") == 0){
                printf("Shutting down...\n");
                zframe_destroy(&message_content);
                zframe_destroy(&sender_id);
                zmsg_destroy(&reply);
                free(shutdown_msg);
                return NULL;
            }

            // encrypted message received / zframe_strdup would truncate at \0 byte           
            unsigned char* ciphertext = zframe_data(message_content);
            char *sender = zframe_strdup(sender_id);

            assert(ciphertext != NULL);
            assert(sender != NULL);

            pthread_mutex_lock(&args->mutex);

            size_t ciphertext_len = zframe_size(message_content);

            // the evp encryption already does the padding for you, but just in case add it manually           
            size_t padding = calculate_padding(ciphertext_len);
            size_t total = ciphertext_len + padding;

            // allocate memory for the plaintext + a bonus 16
            unsigned char* plaintext = (unsigned char*)malloc(total + 16);
            if (!plaintext){
                printf("ERROR: buy more Ram!\n");
                // pthread_mutex_unlock(&args->mutex); 
                // zframe_destroy(&message_content);
                // zframe_destroy(&sender_id);
                // zframe_destroy(&recipient_id);
                // zframe_destroy(&sender_pub_key);
                // zmsg_destroy(&reply);
                return NULL;
            }
                    
            // decrypt the message with aes  
            aes_decrypt(args->key, args->iv, ciphertext, plaintext, total);

            if (args->message_data.sender_id) {
                free(args->message_data.sender_id);
            }

            if (args->message_data.most_recent_received_message) {
                free(args->message_data.most_recent_received_message);
            }

            args->message_data.sender_id = sender;
            args->message_data.most_recent_received_message = (char *)plaintext;
             
            pthread_mutex_unlock(&args->mutex);
        }
        zframe_destroy(&message_content);
        zframe_destroy(&sender_id);
        zmsg_destroy(&reply); 
    }
    return NULL;
}

void *send_messages(void *args_ptr)
{
    Receiver *args = (Receiver *)args_ptr;    

    while (args->running && !zsys_interrupted) { 
        pthread_mutex_lock(&args->mutex);    

        while (!args->is_there_a_msg_to_send && args->running) {   
            // if there is no message to send, wait for the thread to be signalled (idle before then)
            pthread_cond_wait(&args->send_cond, &args->mutex);
        }

        // when thread gets woken up by a signal on shutdown, break out of the loop
        if (!args->running) {
            pthread_mutex_unlock(&args->mutex);
            break;
        }

        size_t plaintext_len = strlen(args->message_data.message_to_send);
        size_t padding = calculate_padding(plaintext_len);  
        size_t ciphertext_len = plaintext_len + padding; 
       
        unsigned char *plaintext = (unsigned char*)args->message_data.message_to_send;
        unsigned char *ciphertext = (unsigned char *)malloc(ciphertext_len + 16);
        if (!ciphertext) {
            printf("ERROR: buy more Ram!\n");
            pthread_mutex_unlock(&args->mutex);
            break;
        }     

        aes_encrypt(args->key, args->iv, plaintext, ciphertext, &ciphertext_len, plaintext_len);    

        zmsg_t *msg = zmsg_new();

        // char* self_id = zsock_identity(args->dealer);      

        // Return public part of key pair as 32-byte binary string
        const byte *sender_pub_key = zcert_public_key(args->message_data.user_certificate);

        char* recipient_id = args->message_data.recipient_id;
        assert(recipient_id != NULL);

        // char* sender_id = zsock_identity(args->dealer);
        // assert(sender_id != NULL);

        // not encrypted 
        // zframe_t *recip_id = zframe_new(recipient_id, strlen(recipient_id));
        // zframe_t *content = zframe_new(args->message_data.message_to_send, strlen(args->message_data.message_to_send));

        // encrypted
        zframe_t *sender_pub = zframe_new(sender_pub_key, 32);
        zframe_t *recip_id = zframe_new(recipient_id, strlen(recipient_id));
        zframe_t *content = zframe_new(ciphertext, ciphertext_len);
        
        // [sender pub key][recipient][message_content]
        zmsg_append(msg, &sender_pub);
        zmsg_append(msg, &recip_id);
        zmsg_append(msg, &content);
    
        // send
        printf("Frame count before sending: %zu\n", zmsg_size(msg));
        zmsg_send(&msg, args->dealer);
        args->is_there_a_msg_to_send = false;

        pthread_mutex_unlock(&args->mutex);    
    }             
    return NULL;
}

zcert_t *get_user_certificate(char* username)
{
    // current user certificate       
    size_t user_cert_buffer = strlen(username) + 11; // formatted text + '\0'
    char* user_certificate_location = malloc(user_cert_buffer);
    snprintf(user_certificate_location, user_cert_buffer, "keys/%s.cert", username);
    // printf("location to load: %s\n", user_certificate_location);
    zcert_t *dealer_cert = zcert_load(user_certificate_location);

    return dealer_cert;
}

// TODO: give this a more appropriate name or refactor it somewhat
void *process_user_input(void *args_ptr)
{
    Receiver *args = (Receiver *)args_ptr; 
    
    pthread_mutex_lock(&args->mutex);

    // wait for the signal that a username has been submitted
    while (!args->user_name && !zsys_interrupted){ 
        pthread_cond_wait(&args->user_name_cond, &args->mutex);
    }   

    if (zsys_interrupted) {
        pthread_mutex_unlock(&args->mutex);
        return NULL;
    }

    // get the user's cert with a helper function            
    args->message_data.user_certificate = get_user_certificate(args->user_name);
    if (!args->message_data.user_certificate) {
        printf("User does not have a certificate\n");
        pthread_mutex_unlock(&args->mutex);
        return NULL;
    }

    // zcert_print(args->user_cerfificate);

    // unlock for some I/O
    pthread_mutex_unlock(&args->mutex);

    // server certificate
    const char* server_cert_location = "keys/router.cert";
    zcert_t *server_cert = zcert_load(server_cert_location);
    if (!server_cert) {
        printf("Unable to load the router's certificate\n");
        return NULL;
    }

    const char *router_key = zcert_public_txt(server_cert);
    printf("router key: %s\n", router_key);
    // zcert_print(server_cert);

    pthread_mutex_lock(&args->mutex);

    // finish setting up curve server on dealer side
    zcert_apply(args->message_data.user_certificate, args->dealer);
    zsock_set_curve_serverkey(args->dealer, router_key);
    zsock_set_identity(args->dealer, args->user_name);

    printf("Connecting to server...\n");
    int rc = zsock_connect(args->dealer, "tcp://localhost:5555"); 
    if (rc != 0) {
        printf("Unable to connect to port 5555\n");
        pthread_mutex_unlock(&args->mutex);
        return NULL;
    } 
    printf("Connected to server...\n");   

    args->username_processed = true;    

    pthread_mutex_unlock(&args->mutex); 
    
    zcert_destroy(&server_cert);    

    return NULL;
}

void free_user_input(UserInput *input, char** user_string) 
{
    if (input) {
        for (size_t i = 0; i < input->count; i++) {
            if (input->items[i]) {
                free(input->items[i]);
                input->items[i] = NULL;
            }
        }
        input->count = 0;
    }
    
    if (user_string && *user_string) {
        free(*user_string);
        *user_string = NULL;
    }
}

void free_message_data(MessageData *data)
{
    if (data) {
        if (data->most_recent_received_message) {
            free(data->most_recent_received_message);
            data->most_recent_received_message = NULL;
        }
        if (data->message_to_send) {
            free(data->message_to_send);
            data->message_to_send = NULL;
        }
        if (data->sender_id) {
            free(data->sender_id);
            data->sender_id = NULL;
        }
        if (data->recipient_id) {
            free(data->recipient_id);
            data->recipient_id = NULL;
        }
        if (data->user_certificate) {
            zcert_destroy(&data->user_certificate);
        }
    }
}

// reading data from args will need a mutex to prevent stale data reads
void add_to_chat_log(Receiver *args, ChatHistory *chat_log)
{
    pthread_mutex_lock(&args->mutex);        

    char *inc_msg = args->message_data.most_recent_received_message;

    // only continue if there is an incoming message
    if (!inc_msg || !*inc_msg) {
        pthread_mutex_unlock(&args->mutex);
        return; 
    }

    time_t current_time;
    time(&current_time);   
            
    char *sender = args->message_data.sender_id;
    assert(sender != NULL);

    // prefixing the message with a log "[user1]: bla-bla-bla"
    size_t format_buffer = 5 + strlen(sender) + strlen(inc_msg); // 5: '[]: + " " + null terminator'
    char msg_buffer[format_buffer];
    snprintf(msg_buffer, sizeof(msg_buffer), "[%s]: %s", sender, inc_msg);

    // if chat_log is not empty, check the last received message
    for (int i = chat_log->count - 1; i >= 0; i--) {
        if (chat_log->items[i].received) {       
            // compare to see if it's a fresh message or if it's already been seen, return in that case         
            if (strcmp(chat_log->items[i].received_msg, msg_buffer) == 0) {
                pthread_mutex_unlock(&args->mutex);
                return;
            }
            break; 
        }
    }        

    Message msg = {0};
    msg.received_msg = strdup(msg_buffer);
    if (!msg.received_msg) {
        printf("ERROR: buy more Ram! cannot strdup msg_buffer (add to chatlog)\n");
        return;
    }
    msg.received = true;
    msg.timestamp = current_time;
    da_append(chat_log, msg);

    pthread_mutex_unlock(&args->mutex);
}

// slight alternatation of the func above to avoid duplicate adds to chat_log
void add_sent_message_to_chat_log(Receiver *args, ChatHistory *chat_log)
{
    pthread_mutex_lock(&args->mutex);
    char *sent_message = args->message_data.message_to_send;

    // not necessary per se, but a precaution
    if (!sent_message || !*sent_message) {
        pthread_mutex_unlock(&args->mutex);
        return; 
    }

    time_t current_time;   
    time(&current_time);      

    char *sender = zsock_identity(args->dealer);
    assert(sender != NULL);

    size_t format_buffer = 5 + strlen(sender) + strlen(sent_message); // 5: '[]: + " " + null terminator'
    char msg_buffer[format_buffer];
    snprintf(msg_buffer, sizeof(msg_buffer), "[%s]: %s", sender, sent_message);

    Message msg = {0};
    // allocate memory for sent_msg
    msg.sent_msg = strdup(msg_buffer); 
    if (!msg.sent_msg) {
        printf("ERROR: buy more Ram, cannot strdup msg_buffer (sent)!\n");
        // free(sender);
        pthread_mutex_unlock(&args->mutex);
        return;
    }
    msg.sent = true;
    msg.timestamp = current_time;
    da_append(chat_log, msg);

    pthread_mutex_unlock(&args->mutex);
}

bool check_for_new_message(Receiver *args, ChatHistory *chat_log)
{
    pthread_mutex_lock(&args->mutex);

    // return early if no message
    if (!args->message_data.most_recent_received_message) {
        pthread_mutex_unlock(&args->mutex);
        return false;
    }

    // first message
    if (chat_log->count == 0) {
        pthread_mutex_unlock(&args->mutex);
        return true;
    }
    
    const char* latest_received_message = NULL;
    for (int i = chat_log->count - 1; i >= 0; i--) {
        if (chat_log->items[i].received && chat_log->items[i].received_msg) {
            // get most recently received message
            latest_received_message = chat_log->items[i].received_msg;
            break;
        }
    }

    // probably also not a necessary check
    if (!latest_received_message) {
        pthread_mutex_unlock(&args->mutex);
        return true;
    }

    // format for comparison
    char *sender = args->message_data.sender_id;
    assert(sender != NULL);
    char *inc_msg = args->message_data.most_recent_received_message;   
    assert(inc_msg != NULL);

    size_t format_buffer = 5 + strlen(sender) + strlen(inc_msg); // 5: '[]: + " " + null terminator'
    char msg_buffer[format_buffer];
    snprintf(msg_buffer, sizeof(msg_buffer), "[%s]: %s", sender, inc_msg);
    
    // compare the most recent received message with the final entry in chat log
    // if they differ, it's new and should be added to the chat log
    bool new = (strcmp(msg_buffer, latest_received_message) != 0);
    // printf("result of strcmp for new msg check: %d\n", new);

    pthread_mutex_unlock(&args->mutex);

    return new;    
}

// TODO: add text wrapping somehow
// use raylib’s MeasureText() for measuring pixel width.
// split the string on spaces/newlines, 
// build lines incrementally.
void draw_chat_history(ChatHistory *chat_log)
{
    // max required time_str buffer for "%d-%m %H:%M:%S" is 14 + 1 for '\0'
    char time_str[15];
    int fontsize = 20; 
    int timestamp_x = 10;   
    
    for (size_t i = 0; i < chat_log->count; i++) {
        Message *msg = &chat_log->items[i];        
        strftime(time_str, sizeof(time_str), "%d-%m %H:%M:%S", localtime(&msg->timestamp));

        int time_str_width = MeasureText(time_str, fontsize);
        // assumption that 150 is the widest the time_str can be
        assert(time_str_width < 151);        
        int padding = 150 - time_str_width;
             
        int msg_x = timestamp_x + time_str_width + padding;   
        int y = 0 + (i * 32);     

        if (msg->received && msg->received_msg) {
            //prefix with timestamp
            DrawText(time_str, timestamp_x, y, fontsize, WHITE);
            DrawText(msg->received_msg, msg_x, y, fontsize, WHITE);
        }

        if (msg->sent && msg->sent_msg) {
            DrawText(time_str, timestamp_x, y, fontsize, WHITE);
            DrawText(msg->sent_msg, msg_x, y, fontsize, WHITE);
        }
    }
}

void free_chat_log(ChatHistory *chat_log) 
{
    for (size_t i = 0; i < chat_log->count; i++) {
        if (chat_log->items[i].received_msg) {
            free(chat_log->items[i].received_msg);
            chat_log->items[i].received_msg = NULL;
        }
        if (chat_log->items[i].sent_msg) {
            free(chat_log->items[i].sent_msg);
            chat_log->items[i].sent_msg = NULL;
        }
    }
    chat_log->count = 0;
}

void mousewheel_scroll(Camera2D *camera) 
{
    // allow for scrolling up and down
    Vector2 scroll = GetMouseWheelMoveV();        
    camera->target.y -= scroll.y * 46.0f;

    // clamp at y = 0
    if (camera->target.y < 0) {
        camera->target.y = 0;
    }
}

// compare password hash with db password hash from char* username
// int authenticate_user(char* username, char* password)
// {
//     //
// }   

void init_raylib(Receiver *args)
{
    InitWindow(800, 600, "client");
    SetTargetFPS(24);

    bool authenticated = false;
    
    UserInput username = {0};
    char* username_string = NULL;
    bool username_submitted = false;
    bool username_printed = false;
    
    UserInput password = {0};
    char* password_string = NULL;
    bool password_submitted = false;
    bool password_printed = false;

    UserInput input = {0};
    char* user_string = NULL;    
    bool user_input_taken = false;
    bool message_sent = false;
    bool new_message_received = false;

    ChatHistory chat_log = {0};

    Camera2D camera = {0};
    camera.target = (Vector2){0.0f, 0.0f};
    camera.rotation = 0.0f;
    camera.zoom = 1.0f;

    while (!WindowShouldClose())
    {
        BeginDrawing();
        ClearBackground(BLACK);

        // TODO: do some user authentication
        if (!authenticated){    
            // get the user to input his username and password (or register an account?)
            // first username, then press enter or backspace, then password followed by backspace or enter for submission    
            if (!username_submitted) {
                DrawText("USERNAME: ", 10, 0, 50, WHITE);   

                get_user_input(&username);
                erase_user_input(&username);
                draw_user_input(&username);
            }

            // prevent empty username from being used
            if (IsKeyPressed(KEY_ENTER) && username.count > 0) {
                username_submitted = true;
            }       

            // formulate the username string 
            if (username_submitted && !username_string) {
                username_string = formulate_string_from_user_input(&username); 

                pthread_mutex_lock(&args->mutex);
                // at this point the user_name becomes available, 
                // signal user_name_cond so it can stop waiting.
                args->user_name = username_string;
                pthread_cond_signal(&args->user_name_cond);
                pthread_mutex_unlock(&args->mutex);
            }            

            if (username_submitted && username_string && !username_printed){
                username_printed = true;
                printf("username_string: %s\n", username_string);
            }

            if (username_submitted && !password_submitted){
                DrawText("Password: ", 10, 0, 50, WHITE);

                get_user_input(&password);       
                erase_user_input(&password);
                draw_user_input_hidden(&password);
            }

            // prevent empty password from being used
            if (IsKeyPressed(KEY_ENTER) && password.count > 0) {
                password_submitted = true;
            }       

            // formulate the username string 
            if (password_submitted && !password_string) {
                password_string = formulate_string_from_user_input(&password);   
            }   

            // TDOO: check if the username exists before checking the password

            // hash password
            if (password_submitted && password_string && !password_printed){
                password_printed = true;
                printf("password_string: %s\n", password_string);
                free_user_input(&password, &password_string);
            }

            // if (authenticate_user(username_string, password_string) == 0) {
            //     authenticated = true;
            // }

            if (password_printed){
                authenticated = true;
            }
        }

        if (authenticated){
            BeginMode2D(camera);

            // draw inside raylib window if chat log is populated
            if (chat_log.count > 0) {
                draw_chat_history(&chat_log);
            }    

            EndMode2D();

            get_user_input(&input);       
            erase_user_input(&input);     
            mousewheel_scroll(&camera);
            draw_user_input(&input);  

            // prevent empty messages from being sent
            if (IsKeyPressed(KEY_ENTER) && input.count > 0) {
                user_input_taken = true;
            }       

            // formulate the string 
            if (user_input_taken && !user_string) {
                user_string = formulate_string_from_user_input(&input);   
            }

            // copy over user input to Receiver struct -- send_message thread will send messages based on send flag being set to true
            if (user_input_taken && user_string && !message_sent) {
                message_sent = true;
                send_user_input(user_string, args->recipient, args);            
            }
            
            // clear message / reset states for next message / add sent message to chat log
            if (message_sent) {   
                message_sent = false;
                user_input_taken = false;     
                add_sent_message_to_chat_log(args, &chat_log);         
                free_user_input(&input, &user_string);        
            }
            
            // set the message_received flag to true if there is a new message
            if (!new_message_received) {
                new_message_received = check_for_new_message(args, &chat_log);
            }

            // add new message to chat log
            if (new_message_received) {
                new_message_received = false;
                add_to_chat_log(args, &chat_log);            
            }     
        }   

        EndDrawing();
    }        
    free_chat_log(&chat_log);    
    
    // trigger the conditions for thread clean up
    pthread_mutex_lock(&args->mutex);
    args->running = false;
    args->is_there_a_msg_to_send = true;
    pthread_cond_signal(&args->send_cond);
    pthread_mutex_unlock(&args->mutex);

    // dummy message on shutdown to kill off the blocking receive thread
    zmsg_t *shutdown_msg = zmsg_new();
    char* self_id = zsock_identity(args->dealer);
    unsigned char dummy_pub_key[32] = {0}; 
    zframe_t *key_frame = zframe_new(dummy_pub_key, 32);
    zmsg_append(shutdown_msg, &key_frame);  
    zmsg_addstr(shutdown_msg, self_id);
    zmsg_addstr(shutdown_msg, "/shutdown");     
    zmsg_send(&shutdown_msg, args->dealer);

    CloseWindow();
}

// TODO: figure out how to get an AES key to both parties safely.
// figure out a way to pick a recipient through a GUI
// implement raygui for gui building?
// add dealer authentication
// create users / auth users
// change usage to use username instead of cl arg   
// account creation / database?
// emoji support?
// file sharing?
// gui needs a lot of tweaks obviously -- logout button

/* 
PGP

the router will need to support dynamic user registration

Limit account registration somehow, rate limiting?

Upon user registering / logging in for the first time: generate rsa keys / certificate?
inside dealer.c after logging in check if the current (unique) user already has a curve certificate, otherwise make one

file based key management pattern
public keys (*.cert)
private keys (*.key) <- encrypt this?

Use CURVE to get the recipient's public rsa key safely in the sender's possession

sender:
generate random AES key (session key): 
encrypt the message with the session key
encrypt the aes session key with the recipient's public (PGP) key (RSA or ECC assymmetric encryption)
message form can be: [Encrypted session key][AES-encrypted message][Metadata, algorithm info, etc.]
send

recipient:
use recipient's private key to decrypt the aes session key
use the session key to decrypt the message
*/

int main(int argc, char* argv[])
{   
    // get current user and recipient 
    if (argc < 2) {
        printf("Usage: %s conversation-partner\n", argv[0]);
        return 1;
    }

    // TODO: get the user from the log in instead, add the user's pub key as a message frame
    // so it matches the pattern the router expects
    // probably have to move the socket binding logic a bit if I want to get the dealer
    // identity from user input
    // current certificates have both public and secret keys in them. they should probably be seperated

    char* recipient = argv[1];
    // printf("assign user and recipient\n");  
    
    // do i need a context? 
    zsock_t *dealer = zsock_new(ZMQ_DEALER);

    // aes key
    unsigned char key[16] = {
        0x3f, 0x5a, 0x1c, 0x8e,
        0x4b, 0x2d, 0x7a, 0x9f,
        0x6e, 0x0b, 0x3d, 0x8a,
        0x5c, 0x1f, 0x2e, 0x4a
    };

    // initialization value
    unsigned char iv[16] = {
        0xa1, 0x2b, 0x3c, 0x4d,
        0x5e, 0x6f, 0x70, 0x81,
        0x92, 0x03, 0x14, 0x25,
        0x36, 0x47, 0x58, 0x69
    }; 

    
    // arguments to be passed around where needed
    Receiver args = {
        .message_data = {
            .message_to_send = NULL,
            .most_recent_received_message = NULL,
            .recipient_id = NULL,
            .sender_id = NULL
        },
        .key = key,
        .iv = iv,
        .dealer = dealer,
        // TODO: set it to true where necessary
        .running = true,
        .is_there_a_msg_to_send = false,
        .user_input = NULL,
        .recipient = recipient,
        .user_name = NULL,
        .username_processed = false           
    };   

    // mutex to prevent race conditions between the threads
    pthread_mutex_init(&args.mutex, NULL);

    // init condition paired with "is_there_new_msg_to_send"
    pthread_cond_init(&args.send_cond, NULL);

    // init condition for user_name processing
    pthread_cond_init(&args.user_name_cond, NULL);

    pthread_t process_user_input_thread;
    pthread_t receive_messages_thread;
    pthread_t send_messages_thread;

    // a new thread to run concurrent to the raylib gameloop to get the username, then set
    // the identity of the dealer and connect to the socket.
    if (pthread_create(&process_user_input_thread, NULL, process_user_input, &args) != 0) {
        fprintf(stderr, "Failed to create process user input thread\n");
        zsock_destroy(&dealer);
        return 1;
    } else {
        printf("Process user input thread created...\n");
    }

    // launch concurrent threads for the message receiver / sender functions
    if (pthread_create(&receive_messages_thread, NULL, receive_messages, &args) != 0) {
        fprintf(stderr, "Failed to create receive messages thread\n");
        zsock_destroy(&dealer);
        return 1;
    } else {
        printf("Receive messages thread created...\n");
    }

    if (pthread_create(&send_messages_thread, NULL, send_messages, &args) != 0) {
        fprintf(stderr, "Failed to create send messages thread\n");
        zsock_destroy(&dealer);
        return 1;
    } else {
        printf("Send messages thread created...\n");
    }

    // start raylib window and pass along the Receiver struct 
    // (args.dealer is not thread-safe so mutex is locked when args is being read or written to and unlocked after)
    printf("Initializing raylib...\n");
    init_raylib(&args);

    // cleanup after raylib loop closes   
    pthread_join(process_user_input_thread, NULL);
    printf("shutting down process user input thread\n");
    pthread_join(receive_messages_thread, NULL);
    printf("shutting down receive thread...\n");
    pthread_join(send_messages_thread, NULL);
    printf("shutting down send thread...\n");

    pthread_cond_destroy(&args.user_name_cond);
    pthread_cond_destroy(&args.send_cond);
    pthread_mutex_destroy(&args.mutex);

    free_message_data(&args.message_data);

    if (args.user_input) {
        free(args.user_input);
        args.user_input = NULL;
    }

    if (args.user_name){
        free(args.user_name);
        args.user_name = NULL;
    }

    zsock_destroy(&dealer);      

    return 0;
}

