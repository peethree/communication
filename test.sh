#!/bin/bash

# open first terminal, cd into communication dir, execute user1's client
kitty @ launch --type=window --title dealer1 bash -c 'eval "$(zoxide init bash)"; z communication && ./dealer user2; exec bash'

# pause
sleep 1

# open second terminal, cd into communication dir, execute user2's client
kitty @ launch --type=window --title dealer2 bash -c 'eval "$(zoxide init bash)"; z communication && ./dealer user1; exec bash'
