#include <czmq.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

// raylib LOG enums interfere with system.h logging
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#include <raylib.h>

#define MAX_MSG_LEN 512
#define MAX_ID_LEN 64
#include <pthread.h>

typedef struct {
    char** items;
    int capacity;
    int count;
} UserInput;

typedef struct {
    char sent_msg[MAX_MSG_LEN];
    char received_msg[MAX_MSG_LEN];
    bool sent;
    bool received;
    time_t timestamp;
} Message;

typedef struct {
    Message *items;
    int capacity;
    int count;
} ChatHistory;

typedef struct {
    char most_recent_received_message[MAX_MSG_LEN];
    char message_to_send[MAX_MSG_LEN];
    char sender_id[MAX_ID_LEN];
    char recipient_id[MAX_ID_LEN];    
} MessageData;

typedef struct {
    MessageData message_data;    
    char* user_input;
    zsock_t *dealer;    
    pthread_mutex_t mutex;    
    bool running;
    bool is_there_a_msg_to_send;
} Receiver;

void get_user_input(UserInput *input)
{
    int unicode = GetCharPressed();
    if (unicode > 0) {          
        char* newChar = malloc(8); // +7 padding to avoid overflow from weird characters
        if (newChar) {
            snprintf(newChar, 8, "%c", unicode);
            nob_da_append(input, newChar);
        }
    } 
}

// TODO: have a look at the magic numbers
void draw_user_input(UserInput *input)
{
    int pos_x = 10;
    int pos_y = 560;
    
    for (int i = 0; i < input->count; i++) {
        // calculate how many characters fit per line
        int chars_per_line = (GetScreenWidth() - 20 - pos_x) / 20;  
        int line = i / chars_per_line;
        
        // position on the current line
        int line_position = i % chars_per_line;                
        int current_x = pos_x + (line_position * 20);
        int current_y = pos_y + (line * 25);
        
        if (input->items[i]) DrawText(input->items[i], current_x, current_y, 20, RED);
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
    int len = 0;
    for (int i = 0; i < input->count; i++) {
        len += strlen(input->items[i]);
    }

    // allocate memory based on total length
    char* str = malloc(len + 1);    
    if (!str) return NULL;
    
    // strcat expects str to start with a '\0' else it won't know where to start appending
    str[0] = '\0';
    for (int i = 0; i < input->count; i++) {
        strcat(str, input->items[i]);
    }
    return str;
}

// copy user input into message buffer of the second thread
void send_user_input(const char* user_input, char* recipient_id, Receiver *args) 
{   
    // mutex
    // printf("preparing to send your message...\n");
    pthread_mutex_lock(&args->mutex);
        strncpy(args->message_data.message_to_send, user_input, MAX_MSG_LEN - 1);
        args->message_data.message_to_send[MAX_MSG_LEN - 1] = '\0';

        strncpy(args->message_data.recipient_id, recipient_id, MAX_ID_LEN - 1);
        args->message_data.recipient_id[MAX_ID_LEN - 1] = '\0';

        args->is_there_a_msg_to_send = true;
    pthread_mutex_unlock(&args->mutex);
    // printf("succesfully prepared your message...\n");
}

/* 
this function will run concurrent with the raylib window
(it follows the required signature for the pthread_create() function in C.) 
*/
void *receive_and_send_message(void *args_ptr) 
{
    Receiver *args = (Receiver *)args_ptr;
    zpoller_t *poller = zpoller_new(args->dealer, NULL);

    while (args->running && !zsys_interrupted) { //  CZMQ: "Global signal indicator, TRUE when user presses Ctrl-C"  

        // poll for incoming messages
        void *signaled_socket = zpoller_wait(poller, 1);  
        if (zpoller_terminated(poller)) {
            break; 
        }

        /* 
        Router sends: [recipient routing ID][sender ID][message content]
        Dealer receives: [sender ID][message content] 
        */
        if (signaled_socket == args->dealer) {
            zmsg_t *reply = zmsg_recv(args->dealer);

            // there is a reply
            if (reply) {

                zframe_t *sender_id = zmsg_pop(reply);
                zframe_t *message_content = zmsg_pop(reply);

                if (message_content) {
                    char *text = zframe_strdup(message_content);
                    char *sender = zframe_strdup(sender_id);
                    // printf("Received: %s...\n", text); 

                    // mutex 
                    // store message + sender_id for later gui purposes
                    pthread_mutex_lock(&args->mutex);                    
                        strncpy(args->message_data.sender_id, sender, MAX_ID_LEN - 1);
                        args->message_data.sender_id[MAX_ID_LEN - 1] = '\0';

                        strncpy(args->message_data.most_recent_received_message, text, MAX_MSG_LEN - 1);
                        args->message_data.most_recent_received_message[MAX_MSG_LEN - 1] = '\0';
                    pthread_mutex_unlock(&args->mutex);

                    free(text);
                    free(sender);
                    zframe_destroy(&message_content);

                    // printf("most recently received message: %s\n", args->message_data.most_recent_received_message);
                }   
                zmsg_destroy(&reply);        
            }  
        }   

        // mutex
        pthread_mutex_lock(&args->mutex);
            bool send = args->is_there_a_msg_to_send;
            char msg_copy[MAX_MSG_LEN];
            char id_copy[MAX_ID_LEN];

            strncpy(msg_copy, args->message_data.message_to_send, MAX_MSG_LEN);
            msg_copy[MAX_MSG_LEN - 1] = '\0';

            strncpy(id_copy, args->message_data.recipient_id, MAX_ID_LEN); 
            id_copy[MAX_ID_LEN - 1] = '\0';

            args->is_there_a_msg_to_send = false;
        pthread_mutex_unlock(&args->mutex);

        // there is a new message to send        
        if (send) {   
            zmsg_t *msg = zmsg_new();

            // the dealer decides the recipient of the msg, recipient is the first frame
            zframe_t *id = zframe_new(id_copy, strlen(id_copy));
            zmsg_append(msg, &id);
            zmsg_addstr(msg, msg_copy);
        
            // send
            zmsg_send(&msg, args->dealer);
            args->is_there_a_msg_to_send = false;
        }             
    }
    zpoller_destroy(&poller);
    return NULL;
}

void free_user_input(UserInput *input, char** user_string) 
{
    input->count = 0;
    free(*user_string);
    *user_string = NULL;
}

// reading data from args will need a mutex to prevent stale data reads
void add_to_chat_log(Receiver *args, ChatHistory *chat_log)
{
    pthread_mutex_lock(&args->mutex);        

        char *inc_msg = args->message_data.most_recent_received_message;

        // only continue if there are actually messages
        if (!inc_msg || !*inc_msg) {
            pthread_mutex_unlock(&args->mutex);
            return; 
        }

        time_t current_time;
        time(&current_time);   
                
        char *sender = args->message_data.sender_id;

        // by prefixing the message with a log "[user1]: bla-bla-bla", message might end up being truncated, so adding extra memory to the buffer here.
        // magic number needs to be fixed in the future based on max user name length I reckon.
        char formatted_msg_buffer[MAX_MSG_LEN + 100];
        snprintf(formatted_msg_buffer, sizeof(formatted_msg_buffer) + 1, "[%s]: %s", sender, inc_msg);

        // if chat_log is not empty, check the last received message
        for (int i = chat_log->count - 1; i >= 0; i--) {
            if (chat_log->items[i].received) {       
                // compare to see if it's a fresh message or if it's already been seen, return in that case         
                if (strcmp(chat_log->items[i].received_msg, formatted_msg_buffer) == 0) {
                    pthread_mutex_unlock(&args->mutex);
                    return;
                }
                break; 
            }
        }        

        Message msg = {0};
        strncpy(msg.received_msg, formatted_msg_buffer, MAX_MSG_LEN - 1);
        msg.received_msg[MAX_MSG_LEN - 1] = '\0';
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
        char formatted_msg_buffer[MAX_MSG_LEN + 100];

        Message msg = {0};
        snprintf(msg.sent_msg, sizeof(formatted_msg_buffer) + 1, "[%s]: %s", sender, sent_message);
        msg.sent = true;
        msg.timestamp = current_time;

        da_append(chat_log, msg);
    pthread_mutex_unlock(&args->mutex);
}

bool check_for_new_message(Receiver *args, ChatHistory *chat_log)
{
    pthread_mutex_lock(&args->mutex);

        // first message
        if (chat_log->count == 0) {
            pthread_mutex_unlock(&args->mutex);
            return true;
        }
        
        const char* last_indexed_chat_log_item = chat_log->items[chat_log->count - 1].received_msg;
        const char* latest_received_message = args->message_data.most_recent_received_message;
        
        // compare the most recent received message with the final entry in chat log
        // if they differ, it's new and should be added to the chat log
        bool new = (strcmp(last_indexed_chat_log_item, latest_received_message) != 0);

    pthread_mutex_unlock(&args->mutex);
    return new;    
}

void draw_chat_history(ChatHistory *chat_log)
{
    char time_str[32];
    

    // for (int i = 0; i < chat_log->count; i++){
    //     DrawText(chat_log->items[i].received_msg, 10, 0 + (i * 32), 20, WHITE);
    //     DrawText(chat_log->items[i].sent_msg, 400, 0 + (i * 32), 20, WHITE);
    // }
    for (int i = 0; i < chat_log->count; i++) {
        Message *msg = &chat_log->items[i];
        strftime(time_str, sizeof(time_str), "%d-%m %H:%M:%S", localtime(&msg->timestamp));

        if (msg->received) {
            DrawText(TextFormat("%s %s", time_str, msg->received_msg), 10, 0 + (i * 32), 20, WHITE);
        }
        if (msg->sent) {
            DrawText(TextFormat("%s %s", time_str, msg->sent_msg), 10, 0 + (i * 32), 20, WHITE);
        }
    }
}

void free_chat_log(ChatHistory *chat_log) 
{
    // for (int i = 0; i < chat_log->count; i++) {
    //     free(chat_log->items[i].received_msg);
    // }
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

void init_raylib(Receiver *args)
{
    InitWindow(800, 600, "client");
    SetTargetFPS(24);

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

        BeginMode2D(camera);

        // draw inside raylib window if chat log is populated
        if (chat_log.count > 0) {
            draw_chat_history(&chat_log);
        }    

        EndMode2D();
        // DrawText("Client", 0, 0, 60, RED);

        get_user_input(&input);       
        erase_user_input(&input);     
        mousewheel_scroll(&camera);
        draw_user_input(&input);  

        // prevent empty messages from being sent
        if (IsKeyPressed(KEY_ENTER) && input.count > 0) {
            user_input_taken = true;
        }       

        // only formulate the string once for now
        if (user_input_taken && !user_string) {
            user_string = formulate_string_from_user_input(&input);    
        }

        // broadcast the message to the server
        if (user_input_taken && user_string && !message_sent) {
            send_user_input(user_string, "user1", args);
            message_sent = true;
        }
        
        // clear message / reset states for next message
        if (message_sent) {    
            add_sent_message_to_chat_log(args, &chat_log);        
            message_sent = false;
            user_input_taken = false;            
            free_user_input(&input, &user_string);            
        }
        
        // set the message_received flag to true if there is a new message
        if (!new_message_received) {
            new_message_received = check_for_new_message(args, &chat_log);
        }

        // add new message to chat log
        if (new_message_received) {
            add_to_chat_log(args, &chat_log);
            new_message_received = false;
        }        

        // TODOS:
        // timestamps?
        // encrypt / decrypt messages?
        // account creation / database?

        EndDrawing();
    }        
    free_chat_log(&chat_log);
    args->running = false;
    CloseWindow();
}

int main(void)
{   
    // connect to server
    printf ("Connecting to server…\n");    
    zsock_t *dealer = zsock_new(ZMQ_DEALER);
    zsock_set_identity(dealer, "user2");
    zsock_connect(dealer, "tcp://localhost:5555");

    // launch a second (concurrent) thread for the message receiver / sender function
    // with the goal of not blocking the raylib gameloop 
    Receiver args = {
        .dealer = dealer,
        .running = true,
        .is_there_a_msg_to_send = false
    };

    pthread_mutex_init(&args.mutex, NULL);
    pthread_t receiver_thread;

    if (pthread_create(&receiver_thread, NULL, receive_and_send_message, &args) != 0) {
        fprintf(stderr, "Failed to create receiver thread\n");
        zsock_destroy(&dealer);
        return 1;
    } else {
        printf("Receiver thread created\n");
    }

    // start raylib window and pass along the Receiver struct which holds the connected socket (dealer is not thread-safe)
    printf("Initializing raylib...\n");
    init_raylib(&args);

    // cleanup 
    pthread_join(receiver_thread, NULL);
    pthread_mutex_destroy(&args.mutex);
    zsock_destroy(&dealer);
    printf("shutting down receiver thread...\n");

    return 0;
}

