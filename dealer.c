#include <czmq.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

// raylib LOG enums interfere with system.h logging
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#include <raylib.h>

// TODO: have the raylib window and message receiving on seperate threads so receiving doesn't block and raylib loop can go on as usual
#define MAX_MSG_LEN 256 * 4
#define MAX_ID_LEN 64
#include <pthread.h>

typedef struct {
    char** items;
    int capacity;
    int count;
} UserInput;

typedef struct {
    char most_recent_received_message[MAX_MSG_LEN];
    char send_message[MAX_MSG_LEN];
    char recipient_id[MAX_ID_LEN];
    char* user_input;
    zsock_t *dealer;    
    pthread_mutex_t mutex;    
    bool running;
    bool message_to_send;
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
    int pos_y = 200;
    
    for (int i = 0; i < input->count; i++) {
        // calculate how many characters fit per line
        int chars_per_line = (GetScreenWidth() - 20 - pos_x) / 50;  
        int line = i / chars_per_line;
        
        // position on the current line
        int line_position = i % chars_per_line;                
        int current_x = pos_x + (line_position * 50);
        int current_y = pos_y + (line * 60);
        
        if (input->items[i]) DrawText(input->items[i], current_x, current_y, 60, RED);
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
    pthread_mutex_lock(&args->mutex);
        strncpy(args->send_message, user_input, MAX_MSG_LEN - 1);
        args->send_message[MAX_MSG_LEN - 1] = '\0';

        strncpy(args->recipient_id, recipient_id, MAX_ID_LEN - 1);
        args->recipient_id[MAX_ID_LEN - 1] = '\0';

        args->message_to_send = true;
    pthread_mutex_unlock(&args->mutex);
}

// (required signature for pthread_create() function in C.)
void *receive_and_send_message(void *args_ptr)
{
    Receiver *args = (Receiver *)args_ptr;

    while (args->running && !zsys_interrupted) { //  CZMQ: "Global signal indicator, TRUE when user presses Ctrl-C or the process"  
        pthread_mutex_lock(&args->mutex);
            bool send = args->message_to_send;
            char msg_copy[MAX_MSG_LEN];
            char id_copy[MAX_ID_LEN];

            strncpy(msg_copy, args->send_message, MAX_MSG_LEN);
            msg_copy[MAX_MSG_LEN - 1] = '\0';

            strncpy(id_copy, args->recipient_id, MAX_ID_LEN); 
            id_copy[MAX_ID_LEN - 1] = '\0';

            args->message_to_send = false;
        pthread_mutex_unlock(&args->mutex);

        // new message 2 send        
        if (send) {   
            zmsg_t *msg = zmsg_new();

            // the dealer decides the recipient of the msg, recipient is the first frame
            zframe_t *id = zframe_new(id_copy, strlen(id_copy));
            zmsg_append(msg, &id);
            zmsg_addstr(msg, msg_copy);
        
            // send
            printf("Sending to: %s --- %s ...\n", args->recipient_id, args->user_input);   
            zmsg_send(&msg, args->dealer);
            args->message_to_send = false;
        }        
        
        // reply
        zmsg_t *reply = zmsg_recv(args->dealer);
        if (reply) {
            // reply id
            zframe_t *reply_id = zmsg_pop(reply);

            // actual message
            zframe_t *message_content = zmsg_pop(reply);
            if (message_content) {
                char *text = zframe_strdup(message_content);
                printf("Received: %s...\n", text); 

                // lock mutex and copy the most recent message into args field
                pthread_mutex_lock(&args->mutex);

                    strncpy(args->most_recent_received_message, text, MAX_MSG_LEN - 1);
                    args->most_recent_received_message[MAX_MSG_LEN - 1] = '\0';

                // unlock n clean up 
                pthread_mutex_unlock(&args->mutex);

                free(text);
                zframe_destroy(&message_content);
            }   
            zframe_destroy(&reply_id);   
            zmsg_destroy(&reply);          
        }           
    }
    return NULL;
}

void free_user_input(UserInput *input, char** user_string) 
{
    input->count = 0;
    free(*user_string);
    *user_string = NULL;
}

void init_raylib(Receiver *args)
{
    InitWindow(800, 600, "client");
    SetTargetFPS(24);

    UserInput input = {0};

    char* user_string = NULL;    
    bool user_input_taken = false;
    bool message_sent = false;

    while (!WindowShouldClose())
    {
        BeginDrawing();

        ClearBackground(BLACK);
        DrawText("Client", 0, 0, 60, RED);

        get_user_input(&input);       
        erase_user_input(&input);
        draw_user_input(&input);  

        if (IsKeyPressed(KEY_ENTER)) {
            user_input_taken = true;
        }       

        // only formulate the string once for now
        if (user_input_taken && !user_string) {
            user_string = formulate_string_from_user_input(&input);    
        }

        // broadcast the message to the server
        if (user_input_taken && user_string && !message_sent) {
            send_user_input(user_string, "user2", args);
            message_sent = true;
        }
        
        // clear message / reset states for next message
        if (message_sent) {
            message_sent = false;
            user_input_taken = false;
            free_user_input(&input, &user_string);            
        }

        EndDrawing();
    }    
    CloseWindow();
}

// TODO: show incoming messages inside the raylib window, 
// reset user input when enter is pressed to allow for multiple messages following after
int main(void)
{   
    // connect to server
    printf ("Connecting to server…\n");    
    zsock_t *dealer = zsock_new(ZMQ_DEALER);
    // TODO: each user should be able to set their own account, add a db?
    zsock_set_identity(dealer, "user1");
    zsock_connect(dealer, "tcp://localhost:5555");

    // launch a second thread for the message receiver function
    Receiver args = {
        .dealer = dealer,
        .running = true,
        .message_to_send = false
    };

    pthread_mutex_init(&args.mutex, NULL);
    pthread_t receiver_thread;

    if (pthread_create(&receiver_thread, NULL, receive_and_send_message, &args) != 0) {
        fprintf(stderr, "Failed to create receiver thread\n");
        zsock_destroy(&dealer);
        return 1;
    }

    // start raylib window and pass along the Receiver struct which holds the connected socket (dealer is not thread-safe)
    printf("Initializing raylib...\n");
    init_raylib(&args);

    // cleanup receiver thread and its mutex
    args.running = false;
    pthread_join(receiver_thread, NULL);
    pthread_mutex_destroy(&args.mutex);
    zsock_destroy(&dealer);
    return 0;
}

