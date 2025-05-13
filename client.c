//  Hello World client
#include <czmq.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

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
        char* newChar = malloc(8); 
        if (newChar) {
            snprintf(newChar, 8, "%c", unicode);
            nob_da_append(input, newChar);
        }
    } 
}

void draw_user_input(UserInput *input)
{
    int pos_x = 10;
    int pos_y = 200;
    
    // draw the user's input
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

void init_raylib()
{
    InitWindow(800, 600, "client");

    UserInput input = {0};

    while (!WindowShouldClose())
    {
        BeginDrawing();

        ClearBackground(BLACK);
        DrawText("Client", 0, 0, 60, RED);

        // get user input
        get_user_input(&input);

        // erase
        erase_user_input(&input);

        // draw user input on the screen
        draw_user_input(&input);

   

        EndDrawing();
    }
    CloseWindow();
}

int main(void)
{   
    // connect to server
    printf ("Connecting to hello world server…\n");
    zsock_t *requester = zsock_new (ZMQ_REQ);
    zsock_connect (requester, "tcp://localhost:5555");

    // start raylib window
    printf("Initializing raylib...\n");
    init_raylib();

    // TODO: inside the raylib window, receive and send messages
    // take user input
    // print received msg
    // send user input as a str

    int request_nbr;
    for (request_nbr = 0; request_nbr != 10; request_nbr++) {
        printf ("Sending Hello %d…\n", request_nbr);

        // send
        zstr_send (requester, "Hello");

        // receive
        char *str = zstr_recv (requester);
        printf ("Received:: %s -- %d\n", str, request_nbr);

        // free string mem
        zstr_free (&str);
    }
    zsock_destroy(&requester);
    return 0;
}

