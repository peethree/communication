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
    char *most_recent_received_message;
    char *message_to_send;
    char *sender_id;
    char *recipient_id;    
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

    pthread_mutex_unlock(&args->mutex);
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

        zframe_t *sender_id = zmsg_pop(reply);
        zframe_t *message_content = zmsg_pop(reply);      

        if (message_content && sender_id) {
            char *text = zframe_strdup(message_content);
            char *sender = zframe_strdup(sender_id);

            assert(text != NULL);
            assert(sender != NULL);

            char* self_id = zsock_identity(args->dealer);

            // kill the thread in case of receive thread blocking
            if (strcmp(text, "/shutdown") == 0 && strcmp(sender, self_id)) {
                // free resources and break loop
                free(text);
                free(sender);
                break;
            }

            if (text && sender){        
                if (strcmp(sender, self_id) != 0){
                    pthread_mutex_lock(&args->mutex); 
                    args->message_data.sender_id = sender;
                    args->message_data.most_recent_received_message = text;
                }  
                pthread_mutex_unlock(&args->mutex);
            }
            else {
                free(text);
                free(sender);
            } 
        } 
        zframe_destroy(&message_content);
        zframe_destroy(&sender_id);
        // printf("most recently received message: %s\n", args->message_data.most_recent_received_message);
        zmsg_destroy(&reply); 
    }
    return NULL;
}

void *send_messages(void *args_ptr)
{
    Receiver *args = (Receiver *)args_ptr;
    
    while (args->running && !zsys_interrupted) {
        pthread_mutex_lock(&args->mutex);
        bool send = args->is_there_a_msg_to_send;

        // there is no new message to send        
        if (!send) {   
            pthread_mutex_unlock(&args->mutex);
            usleep(10000);  // 10ms sleep
            continue;
        }

        zmsg_t *msg = zmsg_new();

        char* recipient_id = args->message_data.recipient_id;
        assert(recipient_id != NULL);

        char* sender_id = zsock_identity(args->dealer);
        assert(sender_id != NULL);

        // the dealer decides the recipient of the msg, recipient is the first frame, sender is added automatically no need to add its frame
        zframe_t *recip_id = zframe_new(recipient_id, strlen(recipient_id));
        zframe_t *content = zframe_new(args->message_data.message_to_send, strlen(args->message_data.message_to_send));
        
        // [recipient][sender][message_content]
        zmsg_append(msg, &recip_id);
        zmsg_append(msg, &content);
    
        // send
        zmsg_send(&msg, args->dealer);
        args->is_there_a_msg_to_send = false;

        pthread_mutex_unlock(&args->mutex);
    }             
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

    // TODO: zsock_identity might not be the right approach
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

    // avoid adding the same sent message twice
    for (int i = chat_log->count - 1; i >= 0; i--) {
        if (chat_log->items[i].sent && strcmp(chat_log->items[i].sent_msg, msg.sent_msg) == 0) {
            free(msg.sent_msg);  // avoid memory leak
            pthread_mutex_unlock(&args->mutex);
            return;
        }
    }

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
    char *inc_msg = args->message_data.most_recent_received_message;
    assert(sender != NULL);
    assert(inc_msg != NULL);

    size_t format_buffer = 5 + strlen(sender) + strlen(inc_msg); // 5: '[]: + " " + null terminator'
    char msg_buffer[format_buffer];
    snprintf(msg_buffer, sizeof(msg_buffer), "[%s]: %s", sender, inc_msg);
    
    // compare the most recent received message with the final entry in chat log
    // if they differ, it's new and should be added to the chat log
    bool new = (strcmp(msg_buffer, latest_received_message) != 0);
    printf("result of strcmp for new msg check: %d\n", new);

    pthread_mutex_unlock(&args->mutex);
    return new;    
}

// ideally use a font that doesn't have different pixel width per character
void draw_chat_history(ChatHistory *chat_log)
{
    // max required time_str buffer for "%d-%m %H:%M:%S" is 14 + 1 for '\0'
    char time_str[15];
    int fontsize = 20;
    
    for (size_t i = 0; i < chat_log->count; i++) {
        Message *msg = &chat_log->items[i];        
        strftime(time_str, sizeof(time_str), "%d-%m %H:%M:%S", localtime(&msg->timestamp));

        int time_str_width = MeasureText(time_str, fontsize);
        // assumption that 140 is the widest the time_str can be
        assert(time_str_width < 141);        
        int padding = 140 - time_str_width;

        int timestamp_x = 10;      
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

        get_user_input(&input);       
        erase_user_input(&input);     
        mousewheel_scroll(&camera);
        draw_user_input(&input);  

        // prevent empty messages from being sent
        if (IsKeyPressed(KEY_ENTER) && input.count > 0) {
            user_input_taken = true;
            // printf("taken user_input\n");
        }       

        // only formulate the string once for now
        if (user_input_taken && !user_string) {
            user_string = formulate_string_from_user_input(&input);   
            // printf("formulating string\n"); 
        }

        // broadcast the message to the server
        if (user_input_taken && user_string && !message_sent) {
            message_sent = true;
            send_user_input(user_string, "user1", args);            
        }
        
        // clear message / reset states for next message
        if (message_sent) {   
            message_sent = false;
            user_input_taken = false;     
            add_sent_message_to_chat_log(args, &chat_log);         
            free_user_input(&input, &user_string);        
            // printf("adding sent message to chat log\n");    
        }
        
        // set the message_received flag to true if there is a new message
        if (!new_message_received) {
            new_message_received = check_for_new_message(args, &chat_log);
            // printf("check for new message\n");
        }

        // add new message to chat log
        if (new_message_received) {
            new_message_received = false;
            add_to_chat_log(args, &chat_log);            
            // printf("adding received message to chatlog\n");
        }        

        // TODOS:
        // encrypt / decrypt messages?
        // account creation / database?
        EndDrawing();
    }        
    free_chat_log(&chat_log);
    args->running = false;

    // dummy message to kill off the blocking receive thread
    zmsg_t *shutdown_msg = zmsg_new();
    char* self_id = zsock_identity(args->dealer);
    zmsg_addstr(shutdown_msg, self_id);         
    zmsg_addstr(shutdown_msg, "/shutdown");      
    zmsg_send(&shutdown_msg, args->dealer);

    CloseWindow();
}

int main(void)
{   
    // connect to server
    printf ("Connecting to server...\n");    
    zsock_t *dealer = zsock_new(ZMQ_DEALER);
    zsock_set_identity(dealer, "user2");
    zsock_connect(dealer, "tcp://localhost:5555");

    // arguments to be passed around where needed
    Receiver args = {
        .message_data = {
            .message_to_send = NULL,
            .most_recent_received_message = NULL,
            .recipient_id = NULL,
            .sender_id = NULL
        },
        .dealer = dealer,
        .running = true,
        .is_there_a_msg_to_send = false,
        .user_input = NULL
    };

    // mutex to prevent race conditions between the threads
    pthread_mutex_init(&args.mutex, NULL);
    pthread_t receive_messages_thread;
    pthread_t send_messages_thread;

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

    // cleanup 
    free_message_data(&args.message_data);

    if (args.user_input) {
        free(args.user_input);
        args.user_input = NULL;
    }

    pthread_join(receive_messages_thread, NULL);
    printf("shutting down receive thread...\n");
    pthread_join(send_messages_thread, NULL);
    printf("shutting down send thread...\n");

    pthread_mutex_destroy(&args.mutex);

    zsock_destroy(&dealer);    

    return 0;
}

