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

typedef struct {
    char** items;
    int capacity;
    int count;
} UserInput;

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
    // 8 bytes per character here too, since we allow that many in our input
    char* str = malloc(8 * input->count + 1);    
    if (!str){
        return NULL;
    }

    // input->items: char**, needs to be dereferenced to char* for the assignment to str
    for (int i = 0; i < input->count; i++) {
        str[i] = *(input->items[i]);
    }
    return str;
}

void send_user_input(char** user_input, zsock_t *dealer) 
{
    int request_nbr;
    for (request_nbr = 0; request_nbr != 10; request_nbr++) {
        printf("Sending: %s --- %d…\n", *user_input, request_nbr);        

        // send
        zstr_send(dealer, *user_input);

        // receive
        char *str = zstr_recv(dealer);
        printf("Received:: %s -- %d\n", str, request_nbr);

        zstr_free(&str);
        sleep(1);
    }
}

void init_raylib(zsock_t *dealer)
{
    InitWindow(800, 600, "client");

    UserInput input = {0};
    char* user_string = NULL;    
    bool user_input_taken = false;

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
        if (user_input_taken && user_string) {
            send_user_input(&user_string, dealer);
        }

        EndDrawing();
    }
    free(user_string);
    CloseWindow();
}

int main(void)
{   
    // connect to server
    printf ("Connecting to server…\n");    
    zsock_t *dealer = zsock_new(ZMQ_DEALER);
    // TODO: each user should be able to set their own account, add a db?
    zsock_set_identity(dealer, "user1");
    zsock_connect(dealer, "tcp://localhost:5555");

    // start raylib window and pass along the connected socket
    printf("Initializing raylib...\n");
    init_raylib(dealer);

    zsock_destroy(&dealer);
    return 0;
}

